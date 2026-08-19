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
 * --, cuando lo que los define es el patron.  Para esos hay constructores con
 * el indice ACOTADO, que es lo que de verdad impide un `r99`.
 */
enum class SpecialReg : uint8_t {
    RIP,
    RBP,
    RSP,
    RFLAGS,
    CUR0,
    CUR1,
    CUR2,
    CUR3
};

/// El texto de un registro especial, indexado por el enum (no buscado).
inline const char *text_of(SpecialReg r) {
    constexpr const char *kNames[] = {"rip",  "rbp",  "rsp",  "rflags",
                                      "cur0", "cur1", "cur2", "cur3"};
    return kNames[static_cast<uint8_t>(r)];
}

struct Reg {
    /// Ancho de la vista: el `.vel` lo escribe como sufijo del nombre.
    enum class Width : uint8_t { Q, D, W, B };

    /**
     * @brief De que banco es el registro.
     *
     * Guardar el BANCO y el INDICE, y no el nombre, es lo que quita las
     * asignaciones de memoria: `Reg::gp(13)` construia antes `"r" +
     * std::to_string(13)`, o sea un `std::string` en el monton POR REGISTRO Y
     * POR EMISION, y el emisor emite cientos de miles.  El nombre sale de una
     * tabla indexada cuando hace falta escribirlo, que es una sola vez y sin
     * copiar nada.
     *
     * Cachear los nombres repetidos habria aliviado el sintoma; no crearlos lo
     * quita.
     */
    enum class Bank : uint8_t { GP, FP, XMM, YMM, ZMM, Special, Raw };

    /// El sufijo del `.vel` para cada ancho.  Interno: la version publica es
    /// @ref suffix_of, que no puede declararse antes que este tipo.
    static const char *suffix_of_(Width w) {
        switch (w) {
        case Width::B: return "b";
        case Width::W: return "w";
        case Width::D: return "d";
        case Width::Q: break;
        }
        return "";
    }

    Bank bank = Bank::GP;
    uint8_t index = 0; ///< 0..15, o el @ref SpecialReg si @c bank es Special.
    Width width = Width::Q;
    /// Solo con @c Bank::Raw: nombre ya calculado.  Puente temporal.
    std::string raw;

    /**
     * @brief Construye desde banco e indice.  Sin monton: son tres bytes.
     *
     * No es `constexpr` porque @c raw es un `std::string`, que hace el tipo
     * no-literal.  Da igual para lo que se buscaba: un `std::string` vacio no
     * pide memoria, asi que construir un `Reg` de un banco indexado sigue sin
     * tocar el monton.  Cuando el puente @c Bank::Raw desaparezca, el campo se
     * va con el y esto puede volver a ser `constexpr`.
     */
    Reg(Bank b, uint8_t i, Width w = Width::Q) noexcept
        : bank(b), index(i), width(w) {}

    /**
     * @brief Registro de proposito general `r0`..`r15`.
     * @param n Indice; fuera de 0..15 no es un registro y se ACOTA en vez de
     *          producir un nombre que no existe.
     */
    static Reg gp(unsigned n, Width w = Width::Q) noexcept {
        return Reg(Bank::GP, static_cast<uint8_t>(n > 15 ? 15 : n), w);
    }
    /// Registro escalar de coma flotante `f0`..`f15`.
    static Reg fp(unsigned n) noexcept {
        return Reg(Bank::FP, static_cast<uint8_t>(n > 15 ? 15 : n));
    }
    /// Vectorial: `xmm`/`ymm`/`zmm` `0`..`15`.
    static Reg xmm(unsigned n) noexcept {
        return Reg(Bank::XMM, static_cast<uint8_t>(n > 15 ? 15 : n));
    }
    static Reg ymm(unsigned n) noexcept {
        return Reg(Bank::YMM, static_cast<uint8_t>(n > 15 ? 15 : n));
    }
    static Reg zmm(unsigned n) noexcept {
        return Reg(Bank::ZMM, static_cast<uint8_t>(n > 15 ? 15 : n));
    }
    /// Uno de los que tienen nombre propio.
    static Reg special(SpecialReg s) noexcept {
        return Reg(Bank::Special, static_cast<uint8_t>(s));
    }

    /**
     * @brief Desde un nombre ya calculado.  PROVISIONAL.
     *
     * Muchos sitios reciben el registro del asignador como CADENA
     * (`reg_name(...)`), asi que hoy hace falta.  El paso siguiente es que
     * quien lo produce devuelva ya un @c Reg: mientras esta puerta exista, un
     * nombre mal formado sigue siendo posible por aqui.
     */
    Reg(std::string n, Width w = Width::Q) noexcept
        : bank(Bank::Raw), width(w), raw(std::move(n)) {}

    /**
     * @brief El nombre del registro, SIN el sufijo de ancho.
     *
     * Sale de tablas indexadas por el numero: no se construye ninguna cadena
     * salvo en el camino @c Raw, que es el puente que va a desaparecer.
     */
    const std::string &base_name() const {
        static const std::string kGP[16] = {
            "r0", "r1", "r2",  "r3",  "r4",  "r5",  "r6",  "r7",
            "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
        static const std::string kFP[16] = {
            "f0", "f1", "f2",  "f3",  "f4",  "f5",  "f6",  "f7",
            "f8", "f9", "f10", "f11", "f12", "f13", "f14", "f15"};
        static const std::string kXMM[16] = {
            "xmm0",  "xmm1",  "xmm2",  "xmm3", "xmm4",  "xmm5",
            "xmm6",  "xmm7",  "xmm8",  "xmm9", "xmm10", "xmm11",
            "xmm12", "xmm13", "xmm14", "xmm15"};
        static const std::string kYMM[16] = {
            "ymm0",  "ymm1",  "ymm2",  "ymm3", "ymm4",  "ymm5",
            "ymm6",  "ymm7",  "ymm8",  "ymm9", "ymm10", "ymm11",
            "ymm12", "ymm13", "ymm14", "ymm15"};
        static const std::string kZMM[16] = {
            "zmm0",  "zmm1",  "zmm2",  "zmm3", "zmm4",  "zmm5",
            "zmm6",  "zmm7",  "zmm8",  "zmm9", "zmm10", "zmm11",
            "zmm12", "zmm13", "zmm14", "zmm15"};
        static const std::string kSpecial[8] = {
            "rip", "rbp", "rsp", "rflags", "cur0", "cur1", "cur2", "cur3"};
        switch (bank) {
        case Bank::GP: return kGP[index & 15];
        case Bank::FP: return kFP[index & 15];
        case Bank::XMM: return kXMM[index & 15];
        case Bank::YMM: return kYMM[index & 15];
        case Bank::ZMM: return kZMM[index & 15];
        case Bank::Special: return kSpecial[index & 7];
        case Bank::Raw: break;
        }
        return raw;
    }

    /**
     * @brief El registro es el general numero @p n.
     *
     * Pregunta por el REGISTRO, no por la vista: `r14` y `r14b` son el mismo, y
     * quien esto pregunta -- para invalidar una cache de constante, por ejemplo
     * -- se refiere al registro, no al ancho con el que se escribe.
     *
     * Compara banco e indice; solo mira el texto cuando el @c Reg vino del
     * puente @c Raw, donde no hay otra cosa que mirar.
     */
    bool is_gp(unsigned n) const {
        if (bank == Bank::GP) return index == n;
        if (bank != Bank::Raw) return false;
        return n < 16 && raw == Reg::gp(n).base_name();
    }

    /**
     * @brief PUENTE TEMPORAL: el registro como texto.
     *
     * Existe para poder migrar por tandas.  Muchas funciones del emisor aun
     * reciben `const std::string&`, y sin esto habria que convertirlas TODAS de
     * golpe -- unas sesenta -- en el mismo cambio, que es justo la clase de
     * salto que sale mal.
     *
     * Mientras exista, un @c Reg se degrada a cadena sin que nadie lo note, o
     * sea que la garantia de tipo se pierde por aqui.  Se quita cuando esas
     * funciones tomen @c Reg, y el compilador dira exactamente cuales quedan.
     */
    operator std::string() const { return base_name() + suffix_of_(width); }

    /// El mismo registro visto a 8 bits.
    static Reg b(std::string n) { return Reg(std::move(n), Width::B); }
    /// A 16 bits.
    static Reg w(std::string n) { return Reg(std::move(n), Width::W); }
    /// A 32 bits.
    static Reg d(std::string n) { return Reg(std::move(n), Width::D); }
};

/**
 * @brief Comprueba si @p r es ese registro concreto.
 *
 * Comparar con el NOMBRE, no con el texto emitido: `r14` y `r14b` son el mismo
 * registro en vistas distintas, y quien pregunta si es r14 -- para invalidar
 * una cache de constante, por ejemplo -- se refiere al registro.
 */
inline bool operator==(const Reg &r, const char *name) {
    return r.base_name() == name;
}
inline bool operator!=(const Reg &r, const char *name) {
    return !(r == name);
}
/// Igual contra una cadena ya calculada.  Parte del puente temporal: cuando el
/// emisor trabaje solo con @c Reg, estas dos sobran.
inline bool operator==(const Reg &r, const std::string &name) {
    return r.base_name() == name;
}
inline bool operator!=(const Reg &r, const std::string &name) {
    return !(r == name);
}
/// Dos registros son el mismo operando si coinciden nombre Y vista.
inline bool operator==(const Reg &a, const Reg &b) {
    return a.base_name() == b.base_name() && a.width == b.width;
}
inline bool operator!=(const Reg &a, const Reg &b) {
    return !(a == b);
}

/// El sufijo que el `.vel` espera para cada ancho.
inline const char *suffix_of(Reg::Width w) {
    return Reg::suffix_of_(w);
}

/**
 * @brief Un ACCESO A MEMORIA como operando: `[base]`, `[base+8]`, `[b+i*4]`.
 *
 * Los corchetes los pone el operando, no quien lo escribe.  Cosidos a mano
 * (`<< ", [" << reg << "]"`) es facil dejarse uno, o abrirlo donde el `.vel`
 * espera un registro -- y eso no falla hasta el ensamblador.
 */
struct Mem {
    std::string base;   ///< registro base.
    std::string index;  ///< registro indice, o vacio.
    unsigned scale = 1; ///< 1/2/4/8; solo cuenta con @c index.
    long long disp = 0; ///< desplazamiento, con signo.

    explicit Mem(std::string b) noexcept : base(std::move(b)) {}
    Mem(std::string b, long long off) noexcept
        : base(std::move(b)), disp(off) {}
    Mem(std::string b, std::string idx, unsigned sc) noexcept
        : base(std::move(b)), index(std::move(idx)), scale(sc) {}
};

/**
 * @brief Una ETIQUETA como operando (destino de salto o de llamada).
 *
 * Distinta de un registro a proposito: `jmp r0` y `jmp fin` no son lo mismo, y
 * con cadenas los dos se escriben igual.
 */
struct Lbl {
    std::string name;
    explicit Lbl(std::string n) noexcept : name(std::move(n)) {}
};

/**
 * @brief Una REFERENCIA A SIMBOLO como operando: `@Absolute("code.fin")`.
 *
 * El `.vel` las escribe como anotaciones, y cosidas a mano son cuatro trozos
 * -- la arroba, el nombre de la clase, los parentesis y las comillas -- que hay
 * que acertar en orden.  Una comilla que falta no la ve nadie hasta ensamblar.
 *
 * La CLASE va en un enum y no en la cadena porque es lista cerrada: un
 * `@Absolut` mal escrito deja de ser posible, en vez de descubrirse al final.
 */
enum class AnnKind : uint8_t {
    Absolute, ///< direccion absoluta de un simbolo (`@Absolute("code.x")`).
    Method,   ///< metodo de un modulo nativo (`@Method("lib:fn")`).
    Name,     ///< nombre a resolver por el enlazador (`@Name("x")`).
};

/// El texto de cada clase, indexado por el enum (no buscado).
inline const char *text_of(AnnKind k) {
    constexpr const char *kNames[] = {"Absolute", "Method", "Name"};
    return kNames[static_cast<uint8_t>(k)];
}

struct Ann {
    AnnKind kind;
    std::string value;

    Ann(AnnKind k, std::string v) noexcept : kind(k), value(std::move(v)) {}

    /// `@Absolute("<v>")`.
    static Ann absolute(std::string v) {
        return Ann(AnnKind::Absolute, std::move(v));
    }
    /// `@Method("<v>")`.
    static Ann method(std::string v) {
        return Ann(AnnKind::Method, std::move(v));
    }
    /// `@Name("<v>")`.
    static Ann name(std::string v) { return Ann(AnnKind::Name, std::move(v)); }
};

/// Una referencia se imprime con su clase, sus parentesis y sus comillas.
inline std::ostream &operator<<(std::ostream &os, const Ann &a) {
    return os << '@' << text_of(a.kind) << "(\"" << a.value << "\")";
}

/// Un registro se imprime con su ancho pegado, que es como lo espera el `.vel`.
inline std::ostream &operator<<(std::ostream &os, const Reg &r) {
    return os << r.base_name() << suffix_of(r.width);
}

/// La memoria se imprime con sus corchetes y solo con las partes que tiene.
///
/// Los espacios alrededor del signo NO son cosmetica: es la forma que ya
/// escribia el emisor a mano (`[r15 + r13]`), y cambiarla haria que la misma
/// entrada produjera un `.vel` distinto -- que es justo lo que este refactor
/// tiene que poder descartar para saber que no ha roto nada.
inline std::ostream &operator<<(std::ostream &os, const Mem &m) {
    os << '[' << m.base;
    if (!m.index.empty()) {
        os << " + " << m.index;
        if (m.scale != 1) os << " * " << m.scale;
    }
    if (m.disp > 0)
        os << " + " << m.disp;
    else if (m.disp < 0)
        os << " - " << -m.disp;
    return os << ']';
}

/// Una etiqueta se imprime por su nombre.
inline std::ostream &operator<<(std::ostream &os, const Lbl &l) {
    return os << l.name;
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
        text_ << v;
        return *this;
    }

    /**
     * @brief Emite una instruccion con el mnemonico TIPADO.
     *
     * Primer paso de "una emision, dos destinos": la ENTRADA pasa a ser tipada
     * aunque la salida siga siendo texto.  Con `<<` un mnemonico mal escrito
     * (`movv`) compila igual y no falla hasta ensamblar; aqui no existe -- el
     * enum viene de @c emmit/instr_list.h, que es la lista UNICA de la que ya
     * salen el enum, sus nombres y su categoria, asi que esto no anade una
     * copia de ese conocimiento.
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
        text_ << "    " << emmit::text_of(m);
        int n = 0;
        // Separador: el primer operando va tras un espacio; el resto, tras
        // coma.
        (void)std::initializer_list<int>{
            ((text_ << (n++ == 0 ? " " : ", ") << ops), 0)...};
        text_ << "\n";
        return *this;
    }

    /// Instruccion tipada SIN operandos (`ret`, `leave`, ...).
    VelSink &emit(emmit::Mnemonic m) {
        text_ << "    " << emmit::text_of(m) << "\n";
        return *this;
    }

    /// El `.vel` acumulado.  Sigue haciendo falta: es lo que se vuelca con
    /// `--vx-emit-only` y lo que se lee para depurar.
    std::string text() const { return text_.str(); }

    /// El `.vel` acumulado, vaciando el sumidero.  Evita copiar un texto que
    /// para un programa grande son megabytes.
    std::string take_text() { return std::move(text_).str(); }

    /// Si no se ha emitido nada.  No es `const` porque preguntar la posicion de
    /// escritura de un stream no lo es -- y falsear la constancia con un cast
    /// seria mentir sobre lo que hace.
    bool empty() { return text_.tellp() == std::streampos(0); }

  private:
    std::ostringstream text_;
};

} // namespace ir

#endif // IR_VEL_SINK_H
