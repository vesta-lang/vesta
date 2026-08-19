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
#include "emmit/operand.h"
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
