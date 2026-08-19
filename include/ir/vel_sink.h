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
#include "emmit/instr.h"
#include "emmit/operand.h"
#include "emmit/mnemonic.h"

#include <initializer_list>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <type_traits>
#include <vector>
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
 */
// Los OPERANDOS viven en `emmit/operand.h`: son del lenguaje ensamblador, no
// del IR, y el CODIFICADOR tambien tiene que poder usarlos.
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
        escribir(v);
        return *this;
    }

    /**
     * @brief Emite una instruccion con el mnemonico y los operandos TIPADOS.
     *
     * Construye una @ref emmit::Instr y la guarda.  El texto ya no se escribe
     * aqui: sale de renderizar lo guardado (@ref text).  Es la diferencia entre
     * tener UNA fuente con dos vistas y tener dos cosas que se pueden separar
     * -- que es como acaban siempre los sitios donde se escribe lo mismo dos
     * veces, y este refactor existe justamente por eso.
     *
     * @param m   Mnemonico.
     * @param ops Operandos, en el orden de la instruccion.
     */
    template <typename... Ops>
    VelSink &emit(emmit::Mnemonic m, const Ops &...ops) {
        emmit::Instr in;
        in.mnem = m;
        (void)std::initializer_list<int>{(in.add(hacer_operando(ops)), 0)...};
        anadir_instr(std::move(in));
        return *this;
    }

    /// Instruccion tipada SIN operandos (`ret`, `leave`, ...).
    VelSink &emit(emmit::Mnemonic m) {
        emmit::Instr in;
        in.mnem = m;
        anadir_instr(std::move(in));
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
        escribir("    // ");
        escribir(texto);
        escribir("\n");
        return *this;
    }

    /// Un comentario SIN sangrar, de los que separan secciones del fichero.
    VelSink &comment_top(const std::string &texto) {
        escribir("// ");
        escribir(texto);
        escribir("\n");
        return *this;
    }

    /**
     * @brief Una etiqueta: `nombre:`.
     *
     * Va sin sangrar y con los dos puntos puestos aqui, que es la parte que se
     * olvida.  Distinta de un comentario a proposito: una etiqueta SI es
     * codigo, el enlazador la resuelve -- y en el arbol que consume el
     * ensamblador, ademas, POSEE todo lo que viene debajo hasta la siguiente.
     */
    VelSink &label(const std::string &nombre) {
        cerrar_crudo();
        orden_.push_back(
            {TipoItem::Etiqueta, static_cast<uint32_t>(etiquetas_.size())});
        etiquetas_.push_back(nombre);
        return *this;
    }

    /**
     * @brief La linea Vesta que origina la SIGUIENTE instruccion.
     *
     * Viajaba como un comentario `// @line N` en el texto, que el lexer volvia
     * a leer y el parser colgaba del nodo: un rodeo por texto para acabar donde
     * ya estaba.  Ahora es un campo de la instruccion, y el comentario lo
     * escribe el renderizador a partir de el.
     *
     * @param linea   Linea, o 0 para no marcar nada.
     * @param columna Columna, o 0 si no consta.
     */
    VelSink &debug_line(int linea, int columna = 0) {
        Marca m;
        m.linea = linea;
        m.columna = columna;
        anadir_marca(std::move(m));
        return *this;
    }

    /// El stackmap preciso del SIGUIENTE safepoint.  Mismo caso que
    /// @ref debug_line: era el comentario `// @sm <hex>`.
    VelSink &stackmap(const std::string &hex) {
        Marca m;
        m.stackmap = hex;
        anadir_marca(std::move(m));
        return *this;
    }

    /// Una linea en blanco, para separar bloques del fichero.
    VelSink &blank() {
        escribir("\n");
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
        std::ostringstream os;
        os << '@' << emmit::text_of(d);
        if (emmit::form_of(d) == emmit::ArgForm::Quoted)
            os << "(\"" << arg << "\")";
        else
            os << '(' << arg << ')';
        os << "\n";
        escribir(os.str());
        return *this;
    }

    /// Anotacion con argumento NUMERICO: `@Align(0x1000)`.  Siempre en hex,
    /// que es como la escribe el `.vel`.
    VelSink &directive(emmit::Directive d, uint64_t valor, int digitos = 0) {
        std::ostringstream os;
        os << '@' << emmit::text_of(d) << "(0x" << std::hex;
        if (digitos > 0) os << std::setw(digitos) << std::setfill('0');
        os << valor << std::dec << ")\n";
        escribir(os.str());
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
            std::ostringstream os;
            os << '@' << emmit::text_of(d) << " {\n";
            sink_->escribir(os.str());
        }
        Block(Block &&o) noexcept : sink_(o.sink_) { o.sink_ = nullptr; }
        Block(const Block &) = delete;
        Block &operator=(const Block &) = delete;
        ~Block() {
            if (sink_ != nullptr) sink_->escribir("}\n\n");
        }

        /// Una entrada del bloque: `    @Name("code"),`.
        Block &entry(emmit::Directive d, const std::string &arg) {
            std::ostringstream os;
            os << "    @" << emmit::text_of(d);
            if (emmit::form_of(d) == emmit::ArgForm::Quoted)
                os << "(\"" << arg << "\")";
            else
                os << '(' << arg << ')';
            os << ",\n";
            sink_->escribir(os.str());
            return *this;
        }

        /// Entrada con argumento numerico: `    @Align(0x1000),`.
        Block &entry(emmit::Directive d, uint64_t valor, int digitos = 0) {
            std::ostringstream os;
            os << "    @" << emmit::text_of(d) << "(0x" << std::hex;
            if (digitos > 0) os << std::setw(digitos) << std::setfill('0');
            os << valor << std::dec << "),\n";
            sink_->escribir(os.str());
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
    std::string text() const {
        // El item en curso puede tener algo sin cerrar; se renderiza detras.
        std::ostringstream os;
        for (const Ref &r : orden_) {
            switch (r.tipo) {
            case TipoItem::Crudo: os << crudos_[r.idx]; break;
            case TipoItem::Etiqueta: os << etiquetas_[r.idx] << ":\n"; break;
            case TipoItem::Instr: render_instr(os, instrs_[r.idx]); break;
            case TipoItem::Marca: render_marca(os, marcas_[r.idx]); break;
            }
        }
        os << crudo_.str();
        return os.str();
    }

    /// El `.vel` acumulado, vaciando el sumidero.  Evita copiar un texto que
    /// para un programa grande son megabytes.
    std::string take_text() { return text(); }

    /// Si no se ha emitido nada.  No es `const` porque preguntar la posicion de
    /// escritura de un stream no lo es -- y falsear la constancia con un cast
    /// seria mentir sobre lo que hace.
    bool empty() { return orden_.empty(); }

  private:
    // -- El modelo: UNA fuente, dos vistas ---------------------------------
    //
    // Lo emitido se guarda como una secuencia de ITEMS: instrucciones tipadas
    // y trozos de texto crudo (secciones, etiquetas, datos, comentarios, y lo
    // que aun no tiene forma propia).  El `.vel` es el resultado de
    // RENDERIZAR esa secuencia, no algo que se escriba en paralelo.
    //
    // Se guardan en vectores separados y un indice de orden, en vez de un
    // vector de una estructura que valga para las dos cosas: asi un item de
    // texto ocupa lo que ocupa su cadena, y no lo que ocupa la instruccion mas
    // grande.
    enum class TipoItem : uint8_t { Crudo, Instr, Etiqueta, Marca };

    /// Un marcador de depuracion: la linea Vesta o el stackmap del safepoint.
    ///
    /// Es un ITEM y no un campo de la instruccion siguiente porque su POSICION
    /// es un dato: hay instrucciones del IR que no producen bytecode propio --
    /// una comparacion que se fusiona con el salto -- y entonces la marca cae
    /// entre dos instrucciones emitidas.  Aplicarla "a la siguiente" la mueve
    /// de sitio.  Quien construya el arbol la aplica igual que hace hoy el
    /// parser: a la instruccion que venga despues.
    struct Marca {
        int linea = 0;
        int columna = 0;
        std::string stackmap;
    };
    struct Ref {
        TipoItem tipo;
        uint32_t idx;
    };

    std::vector<Ref> orden_;
    std::vector<std::string> crudos_;
    std::vector<emmit::Instr> instrs_;
    std::vector<std::string> etiquetas_;
    std::vector<Marca> marcas_;

    /// Anade una instruccion al final.
    void anadir_instr(emmit::Instr in) {
        cerrar_crudo();
        orden_.push_back(
            {TipoItem::Instr, static_cast<uint32_t>(instrs_.size())});
        instrs_.push_back(std::move(in));
    }

    /// Anade un marcador de depuracion en la posicion actual.
    void anadir_marca(Marca m) {
        cerrar_crudo();
        orden_.push_back(
            {TipoItem::Marca, static_cast<uint32_t>(marcas_.size())});
        marcas_.push_back(std::move(m));
    }

    /// El buffer del item de texto en curso.
    ///
    /// Es UN stream reutilizado y no uno por llamada, y eso NO es un detalle:
    /// el emisor deja manipuladores pegados al stream entre un `<<` y el
    /// siguiente -- los bloques de datos hacen `<< std::hex` una vez y escriben
    /// muchos bytes despues --, asi que un stream nuevo por llamada perderia el
    /// estado y `0xff` saldria como `0x255`.  Pasa exactamente eso si se
    /// intenta.
    std::ostringstream crudo_;
    bool crudo_abierto_ = false;

    /// Cierra el item de texto en curso, si lo hay.
    void cerrar_crudo() {
        if (!crudo_abierto_) return;
        orden_.push_back(
            {TipoItem::Crudo, static_cast<uint32_t>(crudos_.size())});
        crudos_.push_back(crudo_.str());
        crudo_.str(std::string()); // vacia el contenido; los manipuladores
                                   // siguen puestos, igual que antes
        crudo_abierto_ = false;
    }

    /// Escribe texto crudo en el item en curso.
    template <typename T> void escribir(const T &v) {
        crudo_ << v;
        crudo_abierto_ = true;
    }

    // -- De lo que recibe `emit` a un operando tipado -----------------------
    //
    // Sobrecargas y no una plantilla con `if constexpr`: asi un tipo que no sea
    // un operando valido no compila, en vez de acabar como texto.
    static emmit::Operand hacer_operando(const Reg &r) {
        return emmit::Operand::of(r);
    }
    static emmit::Operand hacer_operando(const Mem &m) {
        return emmit::Operand::of(m);
    }
    static emmit::Operand hacer_operando(const Lbl &l) {
        return emmit::Operand::of(l);
    }
    static emmit::Operand hacer_operando(const Ann &a) {
        return emmit::Operand::of(a);
    }
    /// Un operando ya construido pasa tal cual.
    static emmit::Operand hacer_operando(emmit::Operand o) { return o; }
    /// Cualquier entero.  El SIGNO se conserva: `ins.imm` es `uint64_t`, y
    /// tratarlo como con signo escribiria 0xFFFFFFFFFFFFFFFF como `-1`.
    template <typename T, typename = typename std::enable_if<
                              std::is_integral<T>::value>::type>
    static emmit::Operand hacer_operando(T v) {
        if (std::is_unsigned<T>::value)
            return emmit::Operand::of_imm(static_cast<uint64_t>(v));
        return emmit::Operand::of_imm(static_cast<int64_t>(v));
    }

    /// Escribe un operando como lo espera el `.vel`.
    static void render_operando(std::ostream &os, const emmit::Operand &o) {
        switch (o.kind) {
        case emmit::OperandKind::Reg: os << o.reg; return;
        case emmit::OperandKind::Mem: os << o.mem; return;
        case emmit::OperandKind::Label: os << o.name; return;
        case emmit::OperandKind::SymRef:
            os << '@' << emmit::text_of(o.sym_kind) << "(\"" << o.name << "\")";
            return;
        case emmit::OperandKind::Imm:
            if (o.imm_digitos_hex > 0) {
                os << "0x" << std::hex << std::setw(o.imm_digitos_hex)
                   << std::setfill('0') << static_cast<uint64_t>(o.imm)
                   << std::dec << std::setfill(' ');
            } else if (o.imm_sin_signo) {
                os << static_cast<uint64_t>(o.imm);
            } else {
                os << o.imm;
            }
            return;
        case emmit::OperandKind::None: return;
        }
    }

    /// Escribe un marcador de depuracion como el comentario que era.
    static void render_marca(std::ostream &os, const Marca &m) {
        if (!m.stackmap.empty()) {
            os << "    // @sm " << m.stackmap << "\n";
            return;
        }
        os << "    // @line " << m.linea;
        if (m.columna > 0) os << " " << m.columna;
        os << "\n";
    }

    /// Escribe una instruccion como la espera el `.vel`.
    static void render_instr(std::ostream &os, const emmit::Instr &in) {
        os << "    " << emmit::text_of(in.mnem);
        for (int i = 0; i < in.n_ops; ++i) {
            os << (i == 0 ? " " : ", ");
            render_operando(os, in.ops[i]);
        }
        os << "\n";
    }
};

} // namespace ir

#endif // IR_VEL_SINK_H
