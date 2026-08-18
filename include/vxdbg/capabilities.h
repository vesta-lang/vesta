/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file capabilities.h
 * @brief Que se puede llegar a saber, antes de intentar averiguarlo.
 *
 * Misma arquitectura que el asignador de banco ancho y que el reconocedor de
 * patrones del optimizador: **Facts -> Capabilities -> Constraints ->
 * Objective**.  No es una analogia: es el mismo esquema aplicado a otro
 * dominio.
 *
 *  - **Facts**: los nodos.  Ya son hechos inmutables del programa,
 *    identificados por contenido; no hay que inventar nada.
 *  - **Capabilities**: esto.  Que preguntas se pueden responder.
 *  - **Constraints**: por que no se puede responder el resto (@ref
 * Unavailable): no se guardo, el optimizador lo elimino, lo guardado ya no
 * corresponde.
 *  - **Objective**: que se busca (@ref Query) -- explicar un fallo, colocar un
 *    punto de parada, atribuir una muestra -- porque no todos necesitan lo
 *    mismo y averiguar de mas cuesta.
 *
 * **Por que hace falta.**  La informacion casi nunca esta entera: un binario se
 * distribuye sin metadatos, un modulo viene de un cache antiguo, el optimizador
 * se llevo lo que se pregunta.  Sin declarar que hay, cada consulta lo descubre
 * a base de intentarlo y devolver vacio, y quien pregunta no distingue "no hay"
 * de "no se pudo".  Esa diferencia es la que importa cuando algo falla: una
 * manda a buscar en otro sitio y la otra manda a recompilar.
 *
 * **Nunca globales.**  Se declaran POR MODULO: uno compilado con informacion de
 * linea y otro sin ella conviven en el mismo programa, asi que preguntar "que
 * sabe este sistema" no tiene respuesta -- la tiene "que se sabe de esto".
 */

#ifndef VXDBG_CAPABILITIES_H
#define VXDBG_CAPABILITIES_H

#include "vxdbg/ids.h"
#include "vxdbg/node.h"

#include <array>
#include <cstdint>

namespace vxdbg {

/**
 * @brief Que se puede responder.
 *
 * Banderas independientes porque se pierden por separado: se puede tener la
 * linea sin las variables, o el intermedio sin el fuente.
 */
enum class Capability : uint32_t {
    None = 0,
    /// Traducir una direccion al cuerpo de codigo que la contiene.
    LocateCode = 1u << 0,
    /// Saber de que instruccion intermedia salio.
    MapToIr = 1u << 1,
    /// Subir del intermedio a la sentencia que lo origino.
    MapToStatement = 1u << 2,
    /// Dar la linea.
    SourcePosition = 1u << 3,
    /// Dar el fichero de forma FIABLE.  Aparte de la anterior a proposito: hay
    /// formatos que guardan un fichero por unidad ensamblada, y entonces la
    /// linea es correcta pero el fichero no -- peor que no decirlo, porque
    /// manda a mirar donde no es.
    SourceFile = 1u << 4,
    /// Decir a que entidad pertenece el codigo.
    ResolveEntity = 1u << 5,
    /// Y su jerarquia: de quien deriva, que cumple.
    TypeHierarchy = 1u << 6,
    /// Enumerar las variables con valor en ese punto.
    LiveVariables = 1u << 7,
    /// Decir donde vive cada una.
    VariableLocation = 1u << 8,
    /// Reconstruir los marcos que el optimizador se llevo.
    InlineFrames = 1u << 9,
    /// Recorrer el grafo de transferencias en ambos sentidos.
    ExecutionGraph = 1u << 10,
    /// El camino inverso: de una linea a las direcciones.
    ReverseLookup = 1u << 11,

    /// Ultima capacidad definida.  Anadir una nueva es moverlo, y el contador
    /// se ajusta solo: mantener el numero aparte era pedirle a quien anada la
    /// decimotercera dentro de cinco anos que se acuerde de tocar dos sitios.
    _Last = ReverseLookup,
};

/// Cuantas capacidades hay, derivado del propio enum.
static constexpr size_t CAPABILITY_COUNT = []() {
    size_t n = 0;
    for (uint32_t v = static_cast<uint32_t>(Capability::_Last); v; v >>= 1)
        ++n;
    return n;
}();

/// Conjunto de capacidades.
using CapabilitySet = uint32_t;

/// @name Operaciones de conjunto
/// Completas para que cualquier algoritmo las trate como cualquier otro juego
/// de banderas, sin conversiones a mano en cada uso.
/// @{
inline CapabilitySet operator|(Capability a, Capability b) {
    return static_cast<CapabilitySet>(a) | static_cast<CapabilitySet>(b);
}
inline CapabilitySet operator|(CapabilitySet a, Capability b) {
    return a | static_cast<CapabilitySet>(b);
}
inline CapabilitySet operator&(CapabilitySet a, Capability b) {
    return a & static_cast<CapabilitySet>(b);
}
inline CapabilitySet operator~(Capability a) {
    return ~static_cast<CapabilitySet>(a);
}
inline CapabilitySet &operator|=(CapabilitySet &a, Capability b) {
    a = a | b;
    return a;
}
inline CapabilitySet &operator&=(CapabilitySet &a, Capability b) {
    a = a & b;
    return a;
}
/// @return @c true si @p set incluye @p c.
inline bool has(CapabilitySet set, Capability c) {
    return (set & c) != 0;
}
/// @}

/**
 * @brief Lo que otra capacidad necesita por debajo.
 *
 * Unas se apoyan en otras: saber DONDE vive una variable no significa nada si
 * no se pueden enumerar, y la jerarquia de un tipo exige haber resuelto la
 * entidad.  Declararlo permite completar una consulta automaticamente en vez de
 * que cada quien recuerde pedir las dos.
 *
 * @param c Capacidad.
 * @return Lo que hace falta antes; @c 0 si se sostiene sola.
 */
CapabilitySet prerequisites(Capability c);

/**
 * @brief Por que NO se puede responder algo.
 *
 * Devolver vacio hace que "no hay" y "no se pudo" parezcan lo mismo, y son
 * cosas distintas.  Separar que algo no se guardara de que el optimizador lo
 * eliminara es justo lo que los formatos clasicos suelen perder.
 */
enum class Unavailable : uint8_t {
    Available = 0,     ///< si se puede
    NotRecorded = 1,   ///< no se guardo (se compilo sin metadatos)
    OptimizedAway = 2, ///< existio y el optimizador lo elimino
    Stale = 3,         ///< lo guardado ya no corresponde al fuente actual
    OutOfRange = 4,    ///< no pertenece a nada conocido
    Unsupported = 5,   ///< esta capa no sabe responder eso
};

/**
 * @brief Lo que se necesita saber, declarado antes de preguntar.
 *
 * Explicar un fallo, colocar un punto de parada y atribuir una muestra no
 * necesitan lo mismo, y averiguar de mas cuesta: enumerar las variables vivas
 * de un punto no es gratis y un perfilador no las quiere.
 */
struct Query {
    /// Sin esto la consulta no sirve.
    CapabilitySet needs = 0;
    /// Esto ayuda pero no se exige: si falta, la respuesta sigue siendo util en
    /// vez de fallar entera.
    CapabilitySet wants = 0;

    /**
     * @brief Anade a @c needs todo lo que hace falta por debajo.
     *
     * El cierre es TRANSITIVO: si una capacidad se apoya en otra que a su vez
     * se apoya en una tercera, entran las tres.  Se itera hasta que el
     * conjunto deja de crecer, porque quedarse en un nivel dejaria consultas
     * que dicen poder satisfacerse y luego no.
     */
    void close_prerequisites();
};

/**
 * @brief Que se puede responder DE UN MODULO, y por que no lo demas.
 *
 * Es el RESULTADO de mirar que hay; construirlo es cosa de quien tenga los
 * nodos.  Se consulta antes de preguntar, asi quien pregunta adapta lo que pide
 * o se lo dice al usuario -- "esto se compilo sin informacion de linea" -- en
 * lugar de ensenar una respuesta a medias sin explicar por que.
 */
class CapabilityReport {
  public:
    CapabilityReport() { reasons_.fill(Unavailable::NotRecorded); }

    /// @return Todo lo que se puede responder.
    CapabilitySet available() const { return available_; }

    /**
     * @brief Si una capacidad concreta esta disponible.
     * @param c Capacidad.
     * @return @c true si se puede responder.
     */
    bool can(Capability c) const { return has(available_, c); }

    /**
     * @brief Por que no se puede responder algo.
     * @param c Capacidad.
     * @return El motivo; @ref Unavailable::Available si si se puede.
     */
    Unavailable why_not(Capability c) const;

    /// Declara una capacidad como disponible.
    void offer(Capability c);

    /// Declara por que una capacidad NO lo esta.
    void deny(Capability c, Unavailable reason);

    /**
     * @brief Comprueba si una consulta se puede satisfacer.
     * @param q Lo que se necesita.
     * @param out_missing Recibe lo exigido que falta.
     * @return @c true si todo lo exigido esta disponible.
     */
    bool satisfies(const Query &q, CapabilitySet &out_missing) const;

  private:
    CapabilitySet available_ = 0;
    /// Un motivo por capacidad: es un mapa fijo, no una lista que crece.  Sin
    /// inserciones, sin duplicados, sin busquedas y sin depender del orden.
    std::array<Unavailable, CAPABILITY_COUNT> reasons_{};
};

/**
 * @brief Quien sabe decir de que es capaz cada modulo.
 *
 * Aparte del informe porque son dos papeles: uno describe, el otro averigua.
 * Y por modulo porque las capacidades NO son globales -- en un mismo programa
 * conviven uno compilado con informacion de linea y otro sin ella, y un informe
 * unico tendria que mentir sobre alguno de los dos.
 */
class CapabilityProvider {
  public:
    virtual ~CapabilityProvider() = default;

    /**
     * @brief Describe que se puede responder de un modulo.
     * @param unit Compilacion de la que se pregunta.
     * @return Su informe.
     */
    virtual CapabilityReport describe_unit(UnitId unit) const = 0;

    /**
     * @brief Describe que se puede responder de un nodo concreto.
     *
     * Mas fino todavia: dentro de un mismo modulo, una funcion puede haberse
     * compilado con informacion que otra no tiene.
     *
     * @param node Nodo del que se pregunta.
     * @return Su informe.
     */
    virtual CapabilityReport describe_node(NodeRef node) const = 0;
};

} // namespace vxdbg

#endif // VXDBG_CAPABILITIES_H
