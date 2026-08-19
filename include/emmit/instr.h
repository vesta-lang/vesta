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

    ir::Reg reg{ir::Reg::Bank::GP, 0};
    ir::Mem mem{ir::Reg::gp(0)};
    int64_t imm = 0;
    std::string name;                        ///< solo con Label o SymRef.
    Directive sym_kind = Directive::ABS_REF; ///< solo con SymRef.

    Operand() = default;

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
    static Operand of(const ir::Lbl &l) {
        Operand o;
        o.kind = OperandKind::Label;
        o.name = l.name;
        return o;
    }
    static Operand of(const ir::Ann &a) {
        Operand o;
        o.kind = OperandKind::SymRef;
        o.name = a.value;
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
struct Instr {
    Mnemonic mnem = Mnemonic::kCount;
    Operand ops[kMaxOperandos];
    int n_ops = 0;

    int source_line = 0;      ///< linea Vesta que la origino, o 0.
    int source_column = 0;    ///< columna, o 0.
    std::string stackmap_hex; ///< stackmap preciso del safepoint, o vacio.

    /// Anade un operando.  Ignora los que pasen del maximo, que es un error de
    /// quien emite y no algo que deba corromper la instruccion.
    void add(Operand o) {
        if (n_ops < kMaxOperandos) ops[n_ops++] = std::move(o);
    }
};

} // namespace emmit

#endif // EMMIT_INSTR_H
