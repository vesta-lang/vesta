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
