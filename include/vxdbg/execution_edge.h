/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file execution_edge.h
 * @brief Como pasa el control de un sitio a otro, sea o no una llamada.
 *
 * Empezo siendo un nodo de llamada, y esa era la vision estrecha: el codigo
 * generado transfiere el control por muchos motivos que no son llamar a nada --
 * lanzar y capturar, reanudar una corrutina, ceder en un generador, un
 * `defer`, un destructor implicito, la limpieza al desenrollar.  Modelar solo
 * llamadas habria dejado todo eso sin explicar, y habria obligado a cada
 * lenguaje con un modelo de ejecucion distinto a inventarse lo suyo.
 *
 * Aqui se modelan ARISTAS del grafo de ejecucion.  Una llamada es una de ellas.
 *
 * **Dos ejes, no uno.**  Como se decidio el destino (@ref DispatchKind) y como
 * acabo materializandose (@ref TransferForm) son cosas independientes: una
 * llamada virtual que el optimizador desvirtualiza y luego incorpora es virtual
 * en el primer eje e incorporada en el segundo, y con un solo enum habria que
 * elegir cual de las dos verdades contar.
 */

#ifndef VXDBG_EXECUTION_EDGE_H
#define VXDBG_EXECUTION_EDGE_H

#include "vxdbg/ids.h"
#include "vxdbg/node.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace vxdbg {

/// Version del esquema de esta capa.
static constexpr uint32_t EDGE_SCHEMA_VERSION = 1;

/// Identificadores de esta capa.
using EdgeId = NodeId<struct EdgeTag>;
using InlineSiteId = NodeId<struct InlineSiteTag>;

/// Por que pasa el control.
enum class EdgeKind : uint8_t {
    Call = 0,       ///< se invoca algo
    Return = 1,     ///< se vuelve
    Throw = 2,      ///< se lanza un fallo
    Unwind = 3,     ///< se deshace el camino buscando quien lo atienda
    Resume = 4,     ///< se reanuda algo suspendido (corrutina, tarea)
    Yield = 5,      ///< se suspende cediendo un valor
    Cleanup = 6,    ///< limpieza al salir de un ambito (`defer`, destructores)
    Synthetic = 7,  ///< transferencia que genero el compilador por su cuenta
};

/// Que hay al otro lado de una transferencia.
enum class EndpointKind : uint8_t {
    Function = 0, ///< otra funcion del programa
    Runtime = 1,  ///< el runtime (lanzar, reanudar, recolectar)
    External = 2, ///< codigo ajeno: una biblioteca del sistema
    Unknown = 3,  ///< no consta
};

/// Como se decidio a donde ir.
enum class DispatchKind : uint8_t {
    Direct = 0,   ///< se sabia al compilar
    Virtual = 1,  ///< por despacho dinamico
    Indirect = 2, ///< por un puntero o un cierre
    Unknown = 3,  ///< no consta
};

/// Como acabo materializandose la transferencia.
enum class TransferForm : uint8_t {
    Normal = 0,   ///< quedo como transferencia real
    Tail = 1,     ///< reemplazo al marco que la hacia
    Inlined = 2,  ///< no llego a haberla: el cuerpo se incorporo
    Elided = 3,   ///< el optimizador la elimino del todo
};

/**
 * @brief Una transferencia de control.
 *
 * Sirve para reconstruir la pila, pero tambien para perfilar -- una muestra cae
 * en un codigo, ese codigo viene de un intermedio, y esa arista dice desde
 * donde se llego -- y para explicar por que se ejecuto algo que nadie escribio.
 */
struct ExecutionEdge {
    DebugNodeHeader header{NodeKind::ExecutionEdge, EDGE_SCHEMA_VERSION, {}};

    IrInstrId source;  ///< instruccion desde la que se transfiere
    IrFunctionId from; ///< funcion en la que estabamos
    /// A donde va.  No siempre es una funcion: lanzar entra en el runtime,
    /// ceder sale hacia quien reanude, un trampolin salta a codigo ajeno.
    /// Suponer que todo destino es una funcion habria dejado sin representar
    /// justo las transferencias que mas cuesta explicar.
    EndpointKind to_kind = EndpointKind::Function;
    IrFunctionId to;   ///< si @c to_kind es Function
    /// Si el destino es ajeno, como se llama ("runtime", "kernel32!Sleep").
    std::string to_name;
    EdgeKind kind = EdgeKind::Call;
    DispatchKind dispatch = DispatchKind::Direct;
    TransferForm form = TransferForm::Normal;
    /// Sentencias que la originaron.  En plural porque el optimizador funde:
    /// `if (a && b)` puede acabar en una sola transferencia que viene de dos,
    /// y guardar una sola obligaria a elegir cual de las dos contar.
    std::vector<StatementId> statements;

    ContentHash compute_hash() const;
};

/**
 * @brief Un marco que no existe en la pila porque el cuerpo se incorporo.
 *
 * Referencia la arista que lo origino en vez de contenerla: la transferencia
 * existe por si misma -- pudo no acabar incorporada -- y meterla dentro habria
 * atado dos hechos distintos que cambian por motivos distintos.
 *
 * Se anidan: lo incorporado puede a su vez haber incorporado, y la cadena
 * entera cabe entre dos marcos reales.  Reconstruirla es lo que convierte una
 * pila fisica en la que quien programa reconoce.
 */
struct InlineSite {
    DebugNodeHeader header{NodeKind::InlineSite, EDGE_SCHEMA_VERSION, {}};

    IrInstrId at;                 ///< instruccion del cuerpo incorporado
    IrFunctionId inlined_function;///< la funcion que hay que ensenar
    EdgeId edge;                  ///< la transferencia que lo provoco
    InlineSiteId parent;          ///< si esta dentro de otra incorporacion

    ContentHash compute_hash() const;
};

/**
 * @brief El grafo de transferencias, para preguntarle en los dos sentidos.
 *
 * "Quien llega hasta aqui" y "a donde va esto" son las dos preguntas de
 * cualquier navegador de codigo, perfilador o analisis, y recorrer todas las
 * aristas en cada una seria lineal en el tamano del programa.
 *
 * Es un indice: se construye a partir de las aristas y no se guarda, como todos
 * los demas de este subsistema.
 */
class ExecutionGraph {
  public:
    /// Registra una arista por su identificador.
    void add(EdgeId id, IrFunctionId from, IrFunctionId to);

    /**
     * @brief Aristas que SALEN de una funcion.
     * @param fn Funcion.
     * @return Sus aristas salientes.
     */
    const std::vector<EdgeId> &out_of(IrFunctionId fn) const;

    /**
     * @brief Aristas que LLEGAN a una funcion.
     * @param fn Funcion.
     * @return Sus aristas entrantes.
     */
    const std::vector<EdgeId> &into(IrFunctionId fn) const;

  private:
    /// Identificadores, NO copias.  Duplicar las aristas en dos direcciones
    /// habria multiplicado por dos algo que en un programa grande no es
    /// pequeno, y sobre todo habria roto la regla que sigue todo lo demas: el
    /// nodo es unico y el almacen es su dueno; esto es solo un indice.
    std::unordered_map<IrFunctionId, std::vector<EdgeId>> out_;
    std::unordered_map<IrFunctionId, std::vector<EdgeId>> in_;
    std::vector<EdgeId> empty_;
};

} // namespace vxdbg

#endif // VXDBG_EXECUTION_EDGE_H
