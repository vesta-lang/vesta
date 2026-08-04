/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file semantic.h
 * @brief El grafo semantico: lo que alguien sabe del programa, antes de tener
 *        identidad.
 *
 * Un @ref SemanticNode es exactamente una @ref LanguageEntity que todavia
 * nombra a las demas por su CLAVE en lugar de por su identificador -- la misma
 * descripcion, la misma definicion, cambiando solo como se refiere a los otros.
 *
 * No se llama simbolo porque no lo es: un simbolo es algo aislado con un nombre,
 * y esto es un NODO de un grafo, donde las aristas importan tanto como el nodo.
 * El conjunto que produce una fase es el grafo semantico de lo que esa fase
 * sabe.
 *
 * El camino tiene tres trabajos, deliberadamente separados:
 *
 *     SemanticNode --[resolve_graph]--> ResolvedNode --[emit_resolved]--> almacen
 *
 * El primero ordena, detecta ciclos y convierte claves en identificadores; el
 * ultimo solo guarda.  Como se calcula la identidad es el tercero, y por eso se
 * inyecta (@ref IdentityFn) en vez de estar escrito dentro del recorrido.
 *
 * La identidad es del MODELO, no del almacenamiento: sale del contenido
 * semantico y no de que exista una cabecera con la que guardarlo.  Por eso el
 * recorrido produce entidades resueltas, sin cabecera, y quien las guarda es
 * quien se la pone.
 *
 * Resolver y calcular identidades NO pueden ser dos fases seguidas, aunque lo
 * parezca: la identidad de un nodo incluye las de aquellos a los que apunta,
 * asi que para resolver una arista hay que haber calculado ya la identidad del
 * destino.  Van entrelazados por narices.  Lo que si se puede -- y es lo que se
 * hace -- es que el recorrido no SEPA como se calculan: las pide.
 *
 * **Este fichero no menciona ningun lenguaje, ni ninguna estructura de ningun
 * compilador, y no debe empezar a hacerlo.**  Quien traduzca lo que sabe a
 * nodos semanticos vive donde ese conocimiento existe.  Esa es la frontera: si
 * algo de aqui necesitara saber que es un `struct`, el subsistema habria dejado
 * de servir para el segundo lenguaje que lo use.  Y por el mismo motivo no es
 * solo cosa de frontends: una fase intermedia o un backend describen lo que
 * saben con los mismos nodos.
 */

#ifndef VXDBG_SEMANTIC_H
#define VXDBG_SEMANTIC_H

#include "vxdbg/source_meta.h"
#include "vxdbg/store.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace vxdbg {

/**
 * @brief Una arista del grafo semantico: a quien apunta, nombrado por clave.
 *
 * Por clave y no por identificador porque quien declara todavia no tiene
 * ninguno: la identidad de un tipo depende de la de su base, asi que no puede
 * conocerse antes de haber resuelto la base.
 */
using SemanticRelation = RelationT<std::string>;

/**
 * @brief Un nodo del grafo semantico.
 *
 * Su @c key es por la que lo nombran los demas.
 */
using SemanticNode = EntityBase<std::string>;

/**
 * @brief Un nodo con las aristas resueltas y su identidad ya calculada.
 *
 * Su clave es la de la entidad: no se repite aparte porque seria el mismo dato
 * en dos sitios, que es como acaban discrepando.
 *
 * Lleva tambien los bytes con los que se calculo la identidad.  Serializar es
 * lo unico caro de todo esto, y sin guardarlos habria que repetirlo entero al
 * guardar: se serializa una vez y esos mismos bytes son los que van al almacen.
 */
struct ResolvedNode {
    ResolvedEntity entity; ///< ya con identificadores; sin cabecera
    LanguageEntityId id;   ///< la suya
    StoredNode encoded;    ///< con lo que se calculo, y lo que se guardara
};

/**
 * @brief Como se calcula una identidad a partir de unos bytes.
 *
 * Recibe lo serializado y devuelve la huella: no construye nada, no rellena
 * nada y no sabe que es una entidad.  Asi cambiar el algoritmo -- otra funcion
 * de huella, una firma, un identificador que venga de fuera -- no toca ni la
 * logica de ordenar y detectar ciclos ni la de guardar.
 *
 * @param node Lo serializado.
 * @return Su huella.
 */
using IdentityFn = std::function<ContentHash(const StoredNode &node)>;

/// @return La identidad por defecto: la huella de lo que se guardaria.
IdentityFn default_identity();

/**
 * @brief Lo que ocurrio al resolver o emitir.
 */
struct GraphReport {
    size_t resolved = 0;
    size_t emitted = 0;
    /// Aristas cuyo destino no estaba en el grafo.  Se omiten en vez de apuntar
    /// a algo inventado: en un diagnostico, un nombre equivocado es peor que un
    /// dato de menos.  Que esto no sea cero es informacion util, no
    /// necesariamente un fallo.
    size_t unresolved = 0;
    /// Nodos con una clave que ya tenia otro.  NO es un dato repetido: la clave
    /// es la identidad semantica, asi que dos nodos con la misma son una
    /// incoherencia de quien construyo el grafo.  Se queda el primero -- es lo
    /// unico que no hace depender el resultado del orden en que lleguen -- pero
    /// se cuenta, porque callarlo dejaria a alguien preguntandose por que falta
    /// lo que si declaro.
    size_t duplicates = 0;
    /// Identificador de cada nodo, por su clave.  Un almacen direccionado por
    /// contenido no se puede recorrer -- solo se llega a un nodo sabiendo su
    /// huella -- asi que quien acaba de emitir es el unico que las conoce y
    /// tiene que devolverlas.
    std::vector<std::pair<std::string, LanguageEntityId>> ids;
};

/**
 * @brief Ordena un grafo semantico y resuelve las aristas entre sus nodos.
 *
 * El orden lo decide esta funcion, no quien llama: una entidad se identifica
 * por su contenido y su contenido incluye a quien referencia, asi que hay que
 * resolver antes lo referenciado.  Quien declara no tiene por que saber que
 * primero van las clases base y luego las derivadas; si tuviera que saberlo,
 * anadir un genero de declaracion obligaria a revisar ese orden.
 *
 * Si los datos traen un ciclo -- que no se puede representar, porque cada
 * identidad dependeria de la otra -- se corta y la arista se omite, quedando
 * contada.
 *
 * No toca el almacen: calcular una identidad no es guardarla.
 *
 * @param nodes Los nodos, en cualquier orden.
 * @param report Recibe lo que ocurrio.
 * @param identity Como calcular la huella; vacia = la de por defecto.
 * @return Los nodos resueltos, en orden de dependencia.
 */
std::vector<ResolvedNode> resolve_graph(const std::vector<SemanticNode> &nodes,
                                        GraphReport &report,
                                        IdentityFn identity = {});

/**
 * @brief Guarda nodos ya resueltos.
 * @param store Donde guardarlos.
 * @param resolved Lo que devolvio @ref resolve_graph.
 * @param report Recibe cuantos se guardaron y con que identificadores.
 */
void emit_resolved(NodeStore &store, const std::vector<ResolvedNode> &resolved,
                   GraphReport &report);

/**
 * @brief Resuelve y guarda, que es lo que casi siempre se quiere.
 * @param store Donde guardarlos.
 * @param nodes Los nodos, en cualquier orden.
 * @return Lo que ocurrio.
 */
GraphReport emit_semantic_graph(NodeStore &store,
                                const std::vector<SemanticNode> &nodes);

} // namespace vxdbg

#endif // VXDBG_SEMANTIC_H
