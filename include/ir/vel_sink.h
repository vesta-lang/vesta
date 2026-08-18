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
 * @file ir/vel_sink.h
 * @brief Por donde sale lo que emite el backend del IR.
 *
 * El emisor escribia a un `std::ostringstream` y de ahi salia texto `.vel`, que
 * el lexer y el parser deshacian acto seguido para codificarlo a bytecode.  El
 * texto es un formato de ENTRADA del lenguaje -- se escribe a mano y hay que
 * poder ensamblarlo -- pero cuando el origen es nuestro propio IR, serializarlo
 * a una cadena para volver a parsearla es trabajo que no compra nada.
 *
 * Este tipo es la bisagra para quitarlo SIN duplicar el backend.  Hoy hace
 * exactamente lo de antes: acumula texto.  Su razon de existir es que sea UN
 * sitio -- y no los miles de `out << ...` repartidos por el emisor -- el que
 * decida a donde va lo emitido.  Cuando la emision estructurada entre, entra
 * aqui, y el resto del emisor no se toca.
 *
 * Lo que NO va a pasar, porque es el error que este diseno evita: escribir un
 * segundo backend que codifique el IR por su cuenta.  La codificacion, las
 * relocations y los simbolos ya estan escritos y funcionando; lo caro es el
 * viaje por texto, no codificar.  Una emision, dos destinos.
 */
#ifndef IR_VEL_SINK_H
#define IR_VEL_SINK_H

#include "emmit/mnemonic.h"

#include <initializer_list>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>

namespace ir {

/**
 * @class VelSink
 * @brief Destino de lo que emite el backend del IR.
 *
 * ## El `operator<<` es PROVISIONAL, y no es el objetivo
 *
 * Aceptar cualquier cosa por `<<` no hace la emision mas robusta que el texto:
 * mueve el problema de sitio.  Un `out << "movv r0, 1\n"` con el mnemonico mal
 * escrito compila igual y falla al ensamblar, y un operando que no existe
 * tampoco lo ve nadie hasta entonces.
 *
 * El objetivo es que la ENTRADA sea tipada aunque la salida siga siendo texto:
 * el mnemonico de un ENUM, los operandos de tipos que solo admitan lo que la
 * instruccion acepta.  Asi un mnemonico inexistente o un operando de la clase
 * equivocada no compilan, en vez de descubrirse al final de la cadena.
 *
 * ## Restriccion al escribir ese enum: NO duplicar la lista
 *
 * Los mnemonicos ya viven en dos sitios -- `InstrSet` (parser) e `InstrTable`
 * (emisor de bytecode) --, los dos indexados por CADENA.  Un enum escrito a
 * mano seria una tercera copia del mismo conocimiento, y las tres se separarian
 * en cuanto alguien anada una instruccion a dos de ellas.
 *
 * Lo correcto es al reves: el enum pasa a ser la FUENTE, y esas tablas indexan
 * por el.  Entonces anadir una instruccion se hace en un sitio y el compilador
 * obliga a completar el resto.
 *
 * Mientras eso no este, `<<` sigue siendo lo que hay -- pero como lo que hay,
 * no como el diseno.
 */
// ---------------------------------------------------------------------------
// Los OPERANDOS, tipados.  Cada uno sabe imprimirse como lo espera el `.vel`,
// asi que quien emite no cose corchetes, comas ni sufijos: dice QUE es cada
// cosa.  Un corchete que falta o un sufijo de ancho en el sitio equivocado
// dejan de ser posibles, en vez de descubrirse al ensamblar.
// ---------------------------------------------------------------------------

/**
 * @brief Un REGISTRO como operando.
 *
 * El ancho es parte del operando, no un sufijo que se pega a la cadena.  Antes
 * se escribia `out << scratch << "b, "`, y esa `"b"` suelta se podia olvidar, o
 * poner donde no iba, sin que nada lo notara hasta ensamblar.
 */
/**
 * @brief Los registros con nombre PROPIO: los que no siguen ningun patron.
 *
 * Son lista cerrada, asi que van como enum.  Los bancos indexados (`r0..r15`,
 * `f0..f15`, `xmm/ymm/zmm`) NO: escribir sus cien nombres a mano seria una
 * tercera copia del mismo conocimiento -- el error que @c instr_list.h advierte
 * --, cuando lo que los define es el patron.  Para esos hay constructores con el
 * indice ACOTADO, que es lo que de verdad impide un `r99`.
 */
enum class RegEspecial : uint8_t { RIP, RBP, RSP, RFLAGS, CUR0, CUR1, CUR2, CUR3 };

/// El texto de un registro especial, indexado por el enum (no buscado).
inline const char *text_of(RegEspecial r) {
    constexpr const char *kNombres[] = {"rip",  "rbp",  "rsp",  "rflags",
                                        "cur0", "cur1", "cur2", "cur3"};
    return kNombres[static_cast<uint8_t>(r)];
}

struct Reg {
    /// Ancho de la vista: el `.vel` lo escribe como sufijo del nombre.
    enum class Ancho : uint8_t { Q, D, W, B };

    std::string nombre; ///< `r0`, `rbp`, `f3`, `ymm2`...
    Ancho ancho = Ancho::Q;

    /**
     * @brief Registro de proposito general `r0`..`r15`.
     * @param n Indice; fuera de 0..15 no es un registro y se ACOTA en vez de
     *          producir un nombre que no existe.
     */
    static Reg gp(unsigned n, Ancho a = Ancho::Q) {
        return Reg("r" + std::to_string(n > 15 ? 15 : n), a);
    }
    /// Registro escalar de coma flotante `f0`..`f15`.
    static Reg fp(unsigned n) { return Reg("f" + std::to_string(n > 15 ? 15 : n)); }
    /// Vectorial: `xmm`/`ymm`/`zmm` `0`..`15`.
    static Reg xmm(unsigned n) { return Reg("xmm" + std::to_string(n > 15 ? 15 : n)); }
    static Reg ymm(unsigned n) { return Reg("ymm" + std::to_string(n > 15 ? 15 : n)); }
    static Reg zmm(unsigned n) { return Reg("zmm" + std::to_string(n > 15 ? 15 : n)); }
    /// Uno de los que tienen nombre propio.
    static Reg esp(RegEspecial e) { return Reg(text_of(e)); }

    /**
     * @brief Desde un nombre ya calculado.  PROVISIONAL.
     *
     * Muchos sitios reciben el registro del asignador como CADENA
     * (`reg_name(...)`), asi que hoy hace falta.  El paso siguiente es que quien
     * lo produce devuelva ya un @c Reg: mientras esta puerta exista, un nombre
     * mal formado sigue siendo posible por aqui.
     */
    explicit Reg(std::string n, Ancho a = Ancho::Q) noexcept
        : nombre(std::move(n)), ancho(a) {}

    /// El mismo registro visto a 8 bits.
    static Reg b(std::string n) { return Reg(std::move(n), Ancho::B); }
    /// A 16 bits.
    static Reg w(std::string n) { return Reg(std::move(n), Ancho::W); }
    /// A 32 bits.
    static Reg d(std::string n) { return Reg(std::move(n), Ancho::D); }
};

/// El sufijo que el `.vel` espera para cada ancho.
inline const char *sufijo_de(Reg::Ancho a) {
    switch (a) {
    case Reg::Ancho::B: return "b";
    case Reg::Ancho::W: return "w";
    case Reg::Ancho::D: return "d";
    case Reg::Ancho::Q: break;
    }
    return "";
}

/**
 * @brief Un ACCESO A MEMORIA como operando: `[base]`, `[base+8]`, `[b+i*4]`.
 *
 * Los corchetes los pone el operando, no quien lo escribe.  Cosidos a mano
 * (`<< ", [" << reg << "]"`) es facil dejarse uno, o abrirlo donde el `.vel`
 * espera un registro -- y eso no falla hasta el ensamblador.
 */
struct Mem {
    std::string base;           ///< registro base.
    std::string indice;         ///< registro indice, o vacio.
    unsigned escala = 1;        ///< 1/2/4/8; solo cuenta con @c indice.
    long long desplazamiento = 0;

    explicit Mem(std::string b) noexcept : base(std::move(b)) {}
    Mem(std::string b, long long off) noexcept
        : base(std::move(b)), desplazamiento(off) {}
    Mem(std::string b, std::string idx, unsigned esc) noexcept
        : base(std::move(b)), indice(std::move(idx)), escala(esc) {}
};

/**
 * @brief Una ETIQUETA como operando (destino de salto o de llamada).
 *
 * Distinta de un registro a proposito: `jmp r0` y `jmp fin` no son lo mismo, y
 * con cadenas los dos se escriben igual.
 */
struct Lbl {
    std::string nombre;
    explicit Lbl(std::string n) noexcept : nombre(std::move(n)) {}
};

/// Un registro se imprime con su ancho pegado, que es como lo espera el `.vel`.
inline std::ostream &operator<<(std::ostream &os, const Reg &r) {
    return os << r.nombre << sufijo_de(r.ancho);
}

/// La memoria se imprime con sus corchetes y solo con las partes que tiene.
inline std::ostream &operator<<(std::ostream &os, const Mem &m) {
    os << '[' << m.base;
    if (!m.indice.empty()) {
        os << '+' << m.indice;
        if (m.escala != 1) os << '*' << m.escala;
    }
    if (m.desplazamiento > 0) os << '+' << m.desplazamiento;
    else if (m.desplazamiento < 0) os << '-' << -m.desplazamiento;
    return os << ']';
}

/// Una etiqueta se imprime por su nombre.
inline std::ostream &operator<<(std::ostream &os, const Lbl &l) {
    return os << l.nombre;
}

class VelSink {
  public:
    VelSink() = default;

    /**
     * @brief Acepta cualquier cosa que un stream aceptaria.
     *
     * Plantilla y no una sobrecarga por tipo: el emisor manda cadenas, enteros,
     * registros y lo que haga falta, y enumerarlos aqui seria una lista que hay
     * que ampliar cada vez que el emisor manda algo nuevo.
     */
    template <typename T> VelSink &operator<<(const T &v) {
        texto_ << v;
        return *this;
    }

    /**
     * @brief Emite una instruccion con el mnemonico TIPADO.
     *
     * Primer paso de "una emision, dos destinos": la ENTRADA pasa a ser tipada
     * aunque la salida siga siendo texto.  Con `<<` un mnemonico mal escrito
     * (`movv`) compila igual y no falla hasta ensamblar; aqui no existe -- el
     * enum viene de @c emmit/instr_list.h, que es la lista UNICA de la que ya
     * salen el enum, sus nombres y su categoria, asi que esto no anade una copia
     * de ese conocimiento.
     *
     * Hoy escribe texto, para que convertir un sitio NO cambie la salida y se
     * pueda migrar de uno en uno con el `.velb` como oraculo.  Cuando el
     * sumidero pase a construir `vm::Instruction`, se cambia AQUI y los sitios
     * ya convertidos no se tocan: ese es todo el objetivo de la bisagra.
     *
     * @param m Mnemonico.
     * @param ops Operandos ya formateados, en el orden de la instruccion.  Que
     *        tambien sean tipados es el paso siguiente; hacerlo ahora obligaria
     *        a describir la forma de cada instruccion antes de tener un solo
     *        sitio convertido.
     */
    template <typename... Ops>
    VelSink &emit(emmit::Mnemonic m, const Ops &...ops) {
        texto_ << "    " << emmit::text_of(m);
        int n = 0;
        // Separador: el primer operando va tras un espacio; el resto, tras coma.
        (void)std::initializer_list<int>{
            ((texto_ << (n++ == 0 ? " " : ", ") << ops), 0)...};
        texto_ << "\n";
        return *this;
    }

    /// Instruccion tipada SIN operandos (`ret`, `leave`, ...).
    VelSink &emit(emmit::Mnemonic m) {
        texto_ << "    " << emmit::text_of(m) << "\n";
        return *this;
    }

    /// El `.vel` acumulado.  Sigue haciendo falta: es lo que se vuelca con
    /// `--vx-emit-only` y lo que se lee para depurar.
    std::string texto() const { return texto_.str(); }

    /// El `.vel` acumulado, vaciando el sumidero.  Evita copiar un texto que
    /// para un programa grande son megabytes.
    std::string tomar_texto() { return std::move(texto_).str(); }

    /// Si no se ha emitido nada.  No es `const` porque preguntar la posicion de
    /// escritura de un stream no lo es -- y falsear la constancia con un cast
    /// seria mentir sobre lo que hace.
    bool vacio() { return texto_.tellp() == std::streampos(0); }

  private:
    std::ostringstream texto_;
};

} // namespace ir

#endif // IR_VEL_SINK_H
