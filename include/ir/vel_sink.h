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

#include "emmit/directive.h"
#include "emmit/mnemonic.h"

#include <initializer_list>
#include <iomanip>
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
    enum class Bank : uint8_t { GP, FP, XMM, YMM, ZMM, Special };

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

    /**
     * @brief Construye desde banco e indice.
     *
     * TRES BYTES y `constexpr`: un registro ES un banco, un numero y un ancho.
     * Tuvo dentro un `std::string` mientras hizo falta el puente desde los
     * nombres que el emisor calculaba a mano; al quedarse ese puente sin
     * usuarios, el campo se fue con el.  Ahora el tipo es trivial y pasarlo por
     * valor no copia nada.
     */
    constexpr Reg(Bank b, uint8_t i, Width w = Width::Q) noexcept
        : bank(b), index(i), width(w) {}

    /**
     * @brief Registro de proposito general `r0`..`r15`.
     * @param n Indice; fuera de 0..15 no es un registro y se ACOTA en vez de
     *          producir un nombre que no existe.
     */
    static constexpr Reg gp(unsigned n, Width w = Width::Q) noexcept {
        return Reg(Bank::GP, static_cast<uint8_t>(n > 15 ? 15 : n), w);
    }
    /// Registro escalar de coma flotante `f0`..`f15`.
    static constexpr Reg fp(unsigned n) noexcept {
        return Reg(Bank::FP, static_cast<uint8_t>(n > 15 ? 15 : n));
    }
    /// Vectorial: `xmm`/`ymm`/`zmm` `0`..`15`.
    static constexpr Reg xmm(unsigned n) noexcept {
        return Reg(Bank::XMM, static_cast<uint8_t>(n > 15 ? 15 : n));
    }
    static constexpr Reg ymm(unsigned n) noexcept {
        return Reg(Bank::YMM, static_cast<uint8_t>(n > 15 ? 15 : n));
    }
    static constexpr Reg zmm(unsigned n) noexcept {
        return Reg(Bank::ZMM, static_cast<uint8_t>(n > 15 ? 15 : n));
    }
    /// Uno de los que tienen nombre propio.
    static constexpr Reg special(SpecialReg s) noexcept {
        return Reg(Bank::Special, static_cast<uint8_t>(s));
    }

    /**
     * @brief El nombre del registro, SIN el sufijo de ancho.
     *
     * Devuelve `const char *` y las tablas son `constexpr`: no hay ni una
     * cadena que construir ni que alojar.  Tuvo tablas de `std::string`
     * mientras existio el puente desde nombres calculados a mano -- y esas se
     * alojan en el monton al arrancar el programa, una vez por tabla, sin que
     * nadie las pida.  Al no haber ya puente, sobran.
     */
    constexpr const char *name() const {
        constexpr const char *kGP[16] = {
            "r0", "r1", "r2",  "r3",  "r4",  "r5",  "r6",  "r7",
            "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
        constexpr const char *kFP[16] = {
            "f0", "f1", "f2",  "f3",  "f4",  "f5",  "f6",  "f7",
            "f8", "f9", "f10", "f11", "f12", "f13", "f14", "f15"};
        constexpr const char *kXMM[16] = {"xmm0",  "xmm1",  "xmm2",  "xmm3",
                                          "xmm4",  "xmm5",  "xmm6",  "xmm7",
                                          "xmm8",  "xmm9",  "xmm10", "xmm11",
                                          "xmm12", "xmm13", "xmm14", "xmm15"};
        constexpr const char *kYMM[16] = {"ymm0",  "ymm1",  "ymm2",  "ymm3",
                                          "ymm4",  "ymm5",  "ymm6",  "ymm7",
                                          "ymm8",  "ymm9",  "ymm10", "ymm11",
                                          "ymm12", "ymm13", "ymm14", "ymm15"};
        constexpr const char *kZMM[16] = {"zmm0",  "zmm1",  "zmm2",  "zmm3",
                                          "zmm4",  "zmm5",  "zmm6",  "zmm7",
                                          "zmm8",  "zmm9",  "zmm10", "zmm11",
                                          "zmm12", "zmm13", "zmm14", "zmm15"};
        constexpr const char *kSpecial[8] = {"rip",  "rbp",  "rsp",  "rflags",
                                             "cur0", "cur1", "cur2", "cur3"};
        switch (bank) {
        case Bank::GP: return kGP[index & 15];
        case Bank::FP: return kFP[index & 15];
        case Bank::XMM: return kXMM[index & 15];
        case Bank::YMM: return kYMM[index & 15];
        case Bank::ZMM: return kZMM[index & 15];
        case Bank::Special: break;
        }
        return kSpecial[index & 7];
    }

    /**
     * @brief El registro es el general numero @p n.
     *
     * Pregunta por el REGISTRO, no por la vista: `r14` y `r14b` son el mismo, y
     * quien esto pregunta -- para invalidar una cache de constante, por ejemplo
     * -- se refiere al registro, no al ancho con el que se escribe.
     */
    constexpr bool is_gp(unsigned n) const {
        return bank == Bank::GP && index == n;
    }
};

/**
 * @brief Dos registros son el mismo operando si coinciden banco, numero y
 * vista.
 *
 * Comparaba los NOMBRES, y eso era comparar cadenas para responder a una
 * pregunta que son tres bytes.  Hubo ademas comparaciones contra literales
 * (`r == "r14"`), que es preguntar por como se escribe en vez de por cual es:
 * para eso esta @ref Reg::is_gp, y esas comparaciones ya no existen.
 */
constexpr bool operator==(const Reg &a, const Reg &b) {
    return a.bank == b.bank && a.index == b.index && a.width == b.width;
}
constexpr bool operator!=(const Reg &a, const Reg &b) {
    return !(a == b);
}

/// El sufijo que el `.vel` espera para cada ancho.
inline const char *suffix_of(Reg::Width w) {
    return Reg::suffix_of_(w);
}

/**
 * @brief El ancho de vista que corresponde a un tamano en BYTES.
 *
 * El emisor calculaba el sufijo a mano en cada sitio (`(n == 4) ? "d" : ...`)
 * y lo pegaba al nombre del registro.  Escrito asi, el ancho es una cadena
 * suelta que se puede olvidar, duplicar o poner donde no va; aqui es el mismo
 * valor que el operando ya lleva dentro.
 *
 * @param bytes Tamano del elemento (1, 2, 4 o cualquier otro = palabra).
 * @return El ancho correspondiente.
 */
inline Reg::Width width_for_bytes(unsigned bytes) {
    switch (bytes) {
    case 1: return Reg::Width::B;
    case 2: return Reg::Width::W;
    case 4: return Reg::Width::D;
    default: return Reg::Width::Q;
    }
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
 * La clase sale de @c emmit::Directive, que es la lista UNICA de anotaciones
 * del `.vel`.  Tuvo aqui su propio enum durante una tanda, y eso era empezar la
 * quinta copia de la misma lista -- el error que este refactor viene a cerrar.
 */
struct Ann {
    emmit::Directive kind;
    std::string value;

    Ann(emmit::Directive k, std::string v) noexcept
        : kind(k), value(std::move(v)) {}

    /// `@Absolute("<v>")`.
    static Ann absolute(std::string v) {
        return Ann(emmit::Directive::ABSOLUTE, std::move(v));
    }
    /// `@Method("<v>")`.
    static Ann method(std::string v) {
        return Ann(emmit::Directive::METHOD, std::move(v));
    }
    /// `@Name("<v>")`.
    static Ann name(std::string v) {
        return Ann(emmit::Directive::NAME, std::move(v));
    }
};

/// Una referencia se imprime con su clase, sus parentesis y sus comillas.
inline std::ostream &operator<<(std::ostream &os, const Ann &a) {
    return os << '@' << emmit::text_of(a.kind) << "(\"" << a.value << "\")";
}

/// Un registro se imprime con su ancho pegado, que es como lo espera el `.vel`.
inline std::ostream &operator<<(std::ostream &os, const Reg &r) {
    return os << r.name() << suffix_of(r.width);
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

    // -- Lo que NO es una instruccion ------------------------------------
    //
    // El `.vel` no es solo instrucciones: lleva secciones, etiquetas, bloques
    // de datos y comentarios.  Todo eso se escribia igual que se escribian las
    // instrucciones antes de esta tanda -- cosiendo trozos de texto --, con los
    // mismos fallos posibles: el nombre de una anotacion mal escrito, unas
    // comillas de mas o de menos, o una llave de cierre que se queda por el
    // camino.  Aqui cada cosa tiene su entrada y su forma.

    /**
     * @brief Un comentario.  No es codigo: el ensamblador lo descarta.
     * @param texto Sin el `//`, que lo pone esto.
     */
    VelSink &comment(const std::string &texto) {
        text_ << "    // " << texto << "\n";
        return *this;
    }

    /// Un comentario SIN sangrar, de los que separan secciones del fichero.
    VelSink &comment_top(const std::string &texto) {
        text_ << "// " << texto << "\n";
        return *this;
    }

    /**
     * @brief Una etiqueta: `nombre:`.
     *
     * Va sin sangrar y con los dos puntos puestos aqui, que es la parte que se
     * olvida.  Distinta de un comentario a proposito: una etiqueta SI es
     * codigo, el enlazador la resuelve.
     */
    VelSink &label(const std::string &nombre) {
        text_ << nombre << ":\n";
        return *this;
    }

    /// Una linea en blanco, para separar bloques del fichero.
    VelSink &blank() {
        text_ << "\n";
        return *this;
    }

    /**
     * @brief Una anotacion suelta con argumento: `@Format("elf")`.
     *
     * Las comillas las decide la LISTA (@ref emmit::form_of), no quien llama:
     * `@Module(x)` va sin ellas y `@Format("x")` con ellas, y equivocarse es un
     * fichero que no ensambla.  Aqui esa decision esta en un solo sitio.
     *
     * @param d   Que anotacion.
     * @param arg Su argumento, sin comillas ni parentesis.
     */
    VelSink &directive(emmit::Directive d, const std::string &arg) {
        text_ << '@' << emmit::text_of(d);
        if (emmit::form_of(d) == emmit::ArgForm::Quoted)
            text_ << "(\"" << arg << "\")";
        else
            text_ << '(' << arg << ')';
        text_ << "\n";
        return *this;
    }

    /// Anotacion con argumento NUMERICO: `@Align(0x1000)`.  Siempre en hex,
    /// que es como la escribe el `.vel`.
    VelSink &directive(emmit::Directive d, uint64_t valor, int digitos = 0) {
        text_ << '@' << emmit::text_of(d) << "(0x" << std::hex;
        if (digitos > 0) text_ << std::setw(digitos) << std::setfill('0');
        text_ << valor << std::dec << std::setfill(' ') << ")\n";
        return *this;
    }

    /**
     * @class Block
     * @brief Un bloque de anotacion abierto: `@Section { ... }`.
     *
     * La llave de cierre la pone el DESTRUCTOR.  Antes era un `out << "}\n\n"`
     * suelto, a varias lineas del que abria, y nada obligaba a que estuvieran
     * los dos: un bloque sin cerrar produce un fichero que el ensamblador ya no
     * entiende, y el error aparece donde empieza lo siguiente.
     */
    class Block {
      public:
        Block(VelSink &s, emmit::Directive d) : sink_(&s) {
            sink_->text_ << '@' << emmit::text_of(d) << " {\n";
        }
        Block(Block &&o) noexcept : sink_(o.sink_) { o.sink_ = nullptr; }
        Block(const Block &) = delete;
        Block &operator=(const Block &) = delete;
        ~Block() {
            if (sink_ != nullptr) sink_->text_ << "}\n\n";
        }

        /// Una entrada del bloque: `    @Name("code"),`.
        Block &entry(emmit::Directive d, const std::string &arg) {
            sink_->text_ << "    @" << emmit::text_of(d);
            if (emmit::form_of(d) == emmit::ArgForm::Quoted)
                sink_->text_ << "(\"" << arg << "\")";
            else
                sink_->text_ << '(' << arg << ')';
            sink_->text_ << ",\n";
            return *this;
        }

        /// Entrada con argumento numerico: `    @Align(0x1000),`.
        Block &entry(emmit::Directive d, uint64_t valor, int digitos = 0) {
            sink_->text_ << "    @" << emmit::text_of(d) << "(0x" << std::hex;
            if (digitos > 0)
                sink_->text_ << std::setw(digitos) << std::setfill('0');
            sink_->text_ << valor << std::dec << std::setfill(' ') << "),\n";
            return *this;
        }

        /// El sumidero, para las entradas que aun no tienen forma propia.
        VelSink &raw() { return *sink_; }

      private:
        VelSink *sink_;
    };

    /// Abre un bloque de anotacion.  Se cierra solo al salir del ambito.
    Block block(emmit::Directive d) { return Block(*this, d); }

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
