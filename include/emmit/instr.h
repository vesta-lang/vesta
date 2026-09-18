/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 */

/**
 * @file emmit/instr.h
 * @brief Una instruccion lista para codificar: mnemonico y operandos, tipados.
 *
 * ## Que problema resuelve
 *
 * El codificador recibe hoy el AST del parser: `vm::Instruction` con
 * `std::vector<std::unique_ptr<ASTNode>>`.  Eso obliga a que cada funcion de
 * emision DESENTIERRE lo que ya se sabia:
 *
 *     auto reg1 = dynamic_cast<vm::RegisterOperand
 * *>(instr->operands[0].get()); if (reg1 == nullptr) throw ...              //
 * la clase, en EJECUCION bool sig = is_signed(instr->opcode);        // hash de
 * la CADENA uint8_t mode = encode_mode(reg1->size_bits);
 *
 * Son 139 `dynamic_cast` en 58 funciones de emision.  Cuando el productor es
 * nuestro propio emisor del IR, todo eso ya estaba dicho al compilar: que
 * instruccion es, que clase tiene cada operando, cual es el registro y de que
 * ancho.  El AST lo entierra en nodos polimorficos del monton -- una asignacion
 * por operando -- para que el codificador lo saque con RTTI.
 *
 * ## La forma
 *
 * Un mnemonico tipado y hasta tres operandos, cada uno una union ETIQUETADA de
 * las clases que el `.vel` admite.  Tamano fijo, sin monton y sin RTTI:
 * preguntar que es un operando es leer un byte.
 *
 * ## Tamano, medido
 *
 * `Operand` son 80 bytes e `Instr` 376.  Parece mucho hasta que se compara con
 * lo que sustituye: por instruccion, el AST son un nodo `Instruction` (con su
 * vector y dos `std::string`), un nodo por operando alojado APARTE, y el bloque
 * del vector -- tres o cuatro llamadas al asignador por instruccion, mas la
 * destruccion de todo eso despues.  Aqui no hay ninguna.
 *
 * En el ejemplo mas grande del corpus (11 700 lineas de `.vel`) son 4,4 MB
 * transitorios.  Encoger `Operand` -- las clases son excluyentes, asi que la
 * mitad de esos bytes estan siempre sin usar -- es una optimizacion POSTERIOR y
 * con su propia medicion: hacerla ahora, a la vez que el cambio de camino,
 * haria imposible saber cual de las dos cosas movio el numero.
 *
 * ## Un codificador, dos productores
 *
 * Esto NO sustituye al `.vel` como formato: el texto es una entrada del
 * lenguaje y se escribe a mano.  Lo que cambia es que deja de ser el UNICO
 * camino.  El parser de texto produce esto mismo, y el emisor del IR tambien --
 * directamente, sin pasar por el texto ni por el AST.
 */
#ifndef EMMIT_INSTR_H
#define EMMIT_INSTR_H

#include "emmit/mnemonic.h"
#include "emmit/operand.h"

#include <cstdint>
#include <string>

namespace emmit {

/// De que clase es un operando.  Un byte, no una jerarquia con RTTI.
enum class OperandKind : uint8_t {
    None,   ///< hueco vacio (la instruccion tiene menos de tres operandos).
    Reg,    ///< registro: `r0`, `f2`, `rsp`, `r14b`.
    Mem,    ///< acceso a memoria: `[r10]`, `[r15 + r13]`, `[r3 - 8]`.
    Imm,    ///< inmediato.
    Label,  ///< etiqueta: destino de un salto escrito por su nombre.
    SymRef, ///< referencia a simbolo: `@Absolute("code.fin")`.
};

/**
 * @brief Un operando cualquiera del `.vel`.
 *
 * Union etiquetada y no jerarquia: las clases son LISTA CERRADA -- las que el
 * `.vel` admite y ni una mas --, asi que la extensibilidad que da la herencia
 * no compra nada y a cambio cuesta una asignacion y un `dynamic_cast` por
 * operando.
 *
 * Los dos con nombre (etiqueta y referencia a simbolo) llevan `std::string`
 * porque el nombre es de verdad texto, y ahi no hay nada que quitar.  Los otros
 * tres son valores: un registro son tres bytes, un acceso a memoria
 * veinticuatro, y un inmediato ocho.
 */
struct Operand {
    OperandKind kind = OperandKind::None;
    /// En que BASE se escribio.  El `.vel` admite las dos y el ensamblador las
    /// lee igual, pero es parte de como se escribio: el emisor formatea a mano
    /// algunos valores en hex (`0x%016llx`) y renderizarlos en decimal
    /// cambiaria el fichero.
    uint8_t imm_digitos_hex = 0; ///< 0 = decimal; >0 = hex con esos digitos.
    /// Si el inmediato se escribio SIN signo.  No es cosmetica: `ins.imm` es
    /// `uint64_t`, y guardar 0xFFFFFFFFFFFFFFFF como `int64_t` lo escribiria
    /// como `-1` -- otro numero, sin que nada avise.
    bool imm_sin_signo = false;
    Directive sym_kind = Directive::ABS_REF; ///< solo con SymRef.

    /**
     * @brief El VALOR, y solo uno de los tres a la vez.
     *
     * UNA UNION DE VERDAD, que es lo que el comentario de arriba lleva diciendo
     * desde que se escribio: un operando es un registro, O un acceso a memoria,
     * O un inmediato -- nunca dos.  Guardarlos en campos separados hacia pagar
     * los tres siempre: tres bytes, dieciseis y ocho, mas el relleno para
     * alinearlos, en CADA uno de los cuatro operandos de CADA instruccion del
     * programa.
     *
     * Medido sobre un proyecto de 144.000 lineas: el operando pasa de 88 bytes
     * a 56, y la instruccion de 408 a 280.  El vector que las guarda es lo que
     * mas memoria larga sostiene de todo el compilador.
     *
     * QUIEN LEE, MIRA `kind` PRIMERO.  Eso ya era cierto antes de la union --
     * `render_operando` es el unico lector y va por `switch` --, pero antes
     * leer el campo equivocado daba un cero y ahora es un error: la union
     * convierte en ruidoso lo que era silencioso.
     *
     * Los tres son trivialmente copiables, asi que la union no necesita
     * constructor de copia, de movimiento ni destructor propios.
     */
    union {
        ir::Reg reg;
        ir::Mem mem;
        int64_t imm;
    };

    /**
     * @brief El nombre, solo con Label o SymRef.  Nunca nulo.
     *
     * UN PUNTERO AL POZO y no una cadena propia: era un `std::string`, treinta
     * y dos bytes en cada operando de cada instruccion, y medido sobre un
     * proyecto de 144.000 lineas solo unas 24.000 de los millones de operandos
     * emitidos llevan nombre.  Los demas pagaban la cadena para dejarla vacia.
     *
     * De paso deja de pedirse memoria por nombre repetido: una etiqueta a la
     * que saltan veinte instrucciones se aloja UNA vez.  Ver @ref
     * intern_operand_name.
     */
    const std::string *name = nullptr;

    /// El nombre, o la cadena vacia si no lleva.  Asi quien lee no comprueba.
    const std::string &name_text() const {
        return name != nullptr ? *name : *empty_name();
    }

    /// Arranca como inmediato cero: una union tiene que nacer con un miembro
    /// activo, y `None` no guarda ningun valor -- el inmediato es el mas barato
    /// de los tres y el unico con un cero que signifique algo.
    Operand() noexcept : imm(0) {}

    static Operand of(ir::Reg r) {
        Operand o;
        o.kind = OperandKind::Reg;
        o.reg = r;
        return o;
    }
    static Operand of(const ir::Mem &m) {
        Operand o;
        o.kind = OperandKind::Mem;
        o.mem = m;
        return o;
    }
    static Operand of_imm(int64_t v) {
        Operand o;
        o.kind = OperandKind::Imm;
        o.imm = v;
        return o;
    }
    /// Inmediato SIN signo.  Sobrecarga aparte y no un `static_cast` en el
    /// llamante: el que llama no tiene por que acordarse de cual de los dos es.
    static Operand of_imm(uint64_t v) {
        Operand o;
        o.kind = OperandKind::Imm;
        o.imm = static_cast<int64_t>(v);
        o.imm_sin_signo = true;
        return o;
    }
    /// Inmediato escrito en HEXADECIMAL, con  digitos rellenados con ceros.
    static Operand of_imm_hex(uint64_t v, int digitos) {
        Operand o = of_imm(v);
        o.imm_digitos_hex = static_cast<uint8_t>(digitos > 0 ? digitos : 1);
        return o;
    }
    static Operand of(const ir::Lbl &l) {
        Operand o;
        o.kind = OperandKind::Label;
        o.name = intern_operand_name(l.name);
        return o;
    }
    static Operand of(const ir::Ann &a) {
        Operand o;
        o.kind = OperandKind::SymRef;
        o.name = intern_operand_name(a.value);
        o.sym_kind = a.kind;
        return o;
    }

    /// Si el hueco esta ocupado.
    bool ocupado() const { return kind != OperandKind::None; }
};

/// Cuantos operandos caben en una instruccion del `.vel`.
inline constexpr int kMaxOperandos = 4;

/**
 * @brief Una instruccion lista para codificar.
 *
 * Los tres campos de depuracion viajan como CAMPOS y no como comentarios.  Hoy
 * el emisor los escribe en el texto (`// @line N`, `// @sm <hex>`), el lexer
 * los vuelve a sacar y el parser los cuelga del nodo: un rodeo por texto para
 * acabar donde ya estaban.
 */
/* EL ORDEN NO ES ESTETICO.  `ops` y el `std::string` se alinean a ocho, asi que
 * cualquier escalar suelto entre ellos deja su hueco de relleno multiplicado
 * por todas las instrucciones del programa.  Con `mnem` delante de `ops` y los
 * demas detras -- que era el orden natural de escribirlo -- se perdian seis
 * bytes tras el mnemonico y cuatro antes del stackmap: diez de cada
 * instruccion.
 *
 * Juntos delante caben en el relleno que `ops` iba a dejar igualmente, y la
 * instruccion pasa de 280 bytes a 272 sin quitar ni un campo. */
struct Instr {
    Mnemonic mnem = Mnemonic::kCount;
    /// Cuantos operandos tiene.  El maximo es @ref kMaxOperandos, que son
    /// cuatro, asi que un byte sobra.
    uint8_t n_ops = 0;
    uint8_t _pad = 0;
    /// Donde empiezan sus operandos en el POZO de quien la guarda.
    ///
    /// Los operandos NO viven aqui dentro.  Vivian, en un `Operand[4]` de 128
    /// bytes, y medido sobre un programa de 441.000 lineas la media es 2,23
    /// operandos: 57 bytes de relleno por instruccion, 52 MiB en el buffer del
    /// emisor.  Y ninguna instruccion del programa llegaba a usar las cuatro
    /// ranuras -- el reparto es 73.501 sin ninguno, 73.501 con uno, 367.500 con
    /// dos y 440.999 con tres.
    ///
    /// Fuera, en un vector contiguo del emisor, cada instruccion paga lo que
    /// tiene.  Quien lea los operandos necesita el pozo ademas de la
    /// instruccion; esa es toda la contrapartida.
    uint32_t ops_off = 0;
};

/* La estructura entera son OCHO bytes, y antes eran 176.  Ademas de los
 * operandos, se fueron tres campos que NADIE leia ni escribia:
 * `source_line`, `source_column` y un `std::string stackmap_hex` de 32 bytes.
 *
 * No es que se hayan perdido: esos datos viajan en el MARCADOR, que es un item
 * aparte del emisor, y ahi tienen que estar.  El motivo lo explica el propio
 * `VelSink`: hay instrucciones del IR que no producen bytecode propio -- una
 * comparacion que se fusiona con el salto --, asi que la marca cae ENTRE dos
 * instrucciones emitidas y su POSICION es un dato.  Colgarla de "la siguiente
 * instruccion" la mueve de sitio. */
static_assert(sizeof(Instr) <= 8,
              "Instr se guarda una por instruccion emitida del programa "
              "entero: lo que crezca aqui se multiplica por millones");

} // namespace emmit

#endif // EMMIT_INSTR_H
