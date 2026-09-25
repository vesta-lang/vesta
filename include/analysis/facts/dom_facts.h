/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/facts/dom_facts.h
 * @brief DomFacts: el grafo de bloques y sus DOMINADORES, calculados en UN
 *        sitio.
 *
 * Antes vivian dentro de @c LoopFacts, que se los calculaba para si, y con
 * copias propias en el optimizador.  Lo necesita casi todo lo que razona sobre
 * el flujo de control -- bucles, el motor de rangos, las relaciones, la
 * escalarizacion --, asi que es un hecho de la base y no un detalle de nadie.
 *
 * EL GRAFO: el de @c IrFunction::edges_of con las DOS clases de arista, la
 * normal y la excepcional.  Sin la segunda, el bloque de un `catch` parece
 * inalcanzable y se queda sin dominador.  Sin repetidos: dos aristas iguales
 * son una para quien camina el grafo (las PHI, que si las distinguen, leen
 * @c IrBlock::preds, no esto).
 *
 * EL ALGORITMO: postorden iterativo desde la entrada (bloque 0), dominadores
 * inmediatos por Cooper-Harvey-Kennedy y el arbol de dominadores numerado al
 * entrar y al salir, para contestar "domina" en tiempo CONSTANTE.
 *
 * LAS TABLAS: se dimensionan al numero de bloques y una funcion normal tiene
 * pocos, asi que viven en linea hasta @c kDomInlineBlocks y solo despues tocan
 * el monton.  Medido cuando vivian en @c loop_facts.cpp: ocho tablas por
 * funcion eran el 5 % de todas las reservas de compilar.
 */

#ifndef VESTA_ANALYSIS_FACTS_DOM_FACTS_H
#define VESTA_ANALYSIS_FACTS_DOM_FACTS_H

#include "ir/ssa_ir.h"
#include "util/alloc/small_vector.h"

#include <cstddef>
#include <cstdint>

namespace analysis {

/// Bloques que caben en linea antes de que una tabla toque el monton.
constexpr size_t kDomInlineBlocks = 8;

/// Numero de postorden de un bloque.  Mas alto = mas cerca de la entrada.
enum PostorderNum : uint32_t {};
/// El bloque no se alcanza desde la entrada.
constexpr PostorderNum NO_POSTORDER = PostorderNum(0xFFFFFFFFu);

/// Instante en que el recorrido del arbol de dominadores entra o sale de un
/// bloque.
enum DomTick : uint32_t {};
/// Un bloque inalcanzable: no esta en el arbol.
constexpr DomTick NO_DOM_TICK = DomTick(0xFFFFFFFFu);

/// Posicion dentro de la lista aplanada de aristas.
enum CfgEdgeSlot : uint32_t {};

/// Posicion dentro de la lista aplanada de hijos del arbol de dominadores.
enum DomChildSlot : uint32_t {};

/**
 * @brief Los vecinos de todos los bloques en DOS tablas contiguas.
 *
 * Los vecinos del bloque `b` son `edges[offs[b] .. offs[b+1])`.  Un vector por
 * bloque costaba una reserva por bloque, dos veces -- sucesores y
 * predecesores --; asi, ninguna mientras quepa en linea, y el recorrido va en
 * orden de memoria.
 */
struct BlockGraph {
    /// Donde empieza cada bloque dentro de @c edges.  N+1 entradas.
    util::SmallVector<CfgEdgeSlot, kDomInlineBlocks + 1> offs;
    /// Los vecinos de todos los bloques, seguidos.
    util::SmallVector<ir::IrBlockId, kDomInlineBlocks * 2> edges;

    /// Cuantos bloques hay.
    size_t size() const noexcept { return offs.empty() ? 0 : offs.size() - 1; }

    /// Los vecinos de UN bloque, recorribles e indexables.
    struct Row {
        const ir::IrBlockId *first;
        const ir::IrBlockId *last;
        const ir::IrBlockId *begin() const noexcept { return first; }
        const ir::IrBlockId *end() const noexcept { return last; }
        size_t size() const noexcept {
            return static_cast<size_t>(last - first);
        }
        ir::IrBlockId operator[](size_t i) const noexcept { return first[i]; }
    };

    /// Los vecinos de @p b.
    Row operator[](size_t b) const noexcept {
        const ir::IrBlockId *base = edges.begin();
        return Row{base + offs[b], base + offs[b + 1]};
    }
};

/**
 * @brief Sucesores de cada bloque de @p fn, sin repetidos y en el orden en que
 *        aparecen.
 *
 * Sale de @c IrFunction::edges_of, la unica regla de aristas del intermedio:
 * quien necesite el grafo de bloques lo pide aqui en vez de mirar los saltos
 * por su cuenta.  Mira la funcion TAL COMO ESTA, no `IrBlock::succs`, que un
 * pase pudo dejar sin rehacer.
 *
 * @param fn   Funcion SSA.
 * @param want De que grafo: @c All (con los `catch`) o @c TerminatorOnly.
 * @return Una fila por bloque.
 */
BlockGraph block_successors(const ir::IrFunction &fn, ir::IrEdgeWant want);

/**
 * @brief El grafo al reves: de sucesores a predecesores, o al contrario.
 *
 * Cada fila sale en orden de bloque de origen.  Sin repetidos si @p g no los
 * tiene.
 *
 * @param g Grafo de bloques.
 * @return Su inverso, con el mismo numero de bloques.
 */
BlockGraph invert_block_graph(const BlockGraph &g);

/**
 * @struct DomFacts
 * @brief Grafo de bloques, postorden y dominadores de una funcion.
 *
 * Todo indexado por @c IrBlockId.  La entrada es el bloque 0.
 *
 * DOS GRAFOS, y no es una duplicacion: con @c IrEdgeWant::All salen tambien
 * las aristas a los `catch`, que es lo que necesita quien ANALIZA -- sin ellas
 * un manejador parece inalcanzable --; con @c IrEdgeWant::TerminatorOnly solo
 * las del terminador, que es lo que necesita quien COLOCA PHIs -- una PHI solo
 * tiene argumento por las aristas que guarda @c IrBlock::preds, y ponerle uno
 * por la de excepcion daria un IR invalido --.  Cada consumidor pide el suyo
 * y @c edges dice cual es.
 */
struct DomFacts {
    ir::IrEdgeWant edges = ir::IrEdgeWant::All; ///< de que grafo se calculo.
    BlockGraph succs; ///< sucesores (segun @c edges, sin repetidos).
    BlockGraph preds; ///< predecesores: la inversa exacta de @c succs.

    /// Numero de postorden por bloque; @c NO_POSTORDER si es inalcanzable.
    util::SmallVector<PostorderNum, kDomInlineBlocks> postorder;
    /// Los bloques ALCANZABLES en postorden inverso (la entrada primero).
    util::SmallVector<ir::IrBlockId, kDomInlineBlocks> rpo;
    /// Dominador inmediato; la entrada se tiene a si misma; @c IR_NO_BLOCK si
    /// es inalcanzable.
    util::SmallVector<ir::IrBlockId, kDomInlineBlocks> idom;
    /// Cuando se entra en el bloque y cuando se sale, en el arbol.
    util::SmallVector<DomTick, kDomInlineBlocks> pre, post;
    /// Los hijos de cada bloque en el arbol: los de `b` son
    /// `children[child_offs[b] .. child_offs[b+1])`, en orden de bloque.  Hacen
    /// falta para numerar el arbol, asi que se guardan en vez de tirarlos: los
    /// recorre quien baja por el arbol (el renombrado de mem2reg).
    util::SmallVector<DomChildSlot, kDomInlineBlocks + 1> child_offs;
    util::SmallVector<ir::IrBlockId, kDomInlineBlocks> children;

    /// Los hijos de @p b en el arbol de dominadores.
    BlockGraph::Row children_of(ir::IrBlockId b) const noexcept {
        const ir::IrBlockId *base = children.begin();
        return BlockGraph::Row{base + child_offs[b], base + child_offs[b + 1]};
    }

    /// Cuantos bloques tiene la funcion.
    size_t block_count() const noexcept { return idom.size(); }

    /// @p b se alcanza desde la entrada.
    bool reachable(ir::IrBlockId b) const noexcept {
        return b < idom.size() && idom[b] != ir::IR_NO_BLOCK;
    }

    /// @p a domina a @p b (todo bloque se domina a si mismo).  Falso si alguno
    /// de los dos es inalcanzable: fuera del arbol no hay dominancia que
    /// afirmar.  O(1).
    bool dominates(ir::IrBlockId a, ir::IrBlockId b) const noexcept {
        if (a >= pre.size() || b >= pre.size()) return false;
        if (pre[a] == NO_DOM_TICK || pre[b] == NO_DOM_TICK) return false;
        return pre[a] <= pre[b] && post[b] <= post[a];
    }

    /// @p a domina a @p b y no son el mismo bloque.
    bool strictly_dominates(ir::IrBlockId a, ir::IrBlockId b) const noexcept {
        return a != b && dominates(a, b);
    }
};

/**
 * @brief Marcador del analisis para el gestor.
 *
 * Aqui, con su dominio, como @c LoopsAnalysis: un marcador escondido en otra
 * unidad deja fuera del gestor a quien no la vea.
 */
struct DominatorsAnalysis {
    static char ID;
    /// Como se llama al medirlo.  Lo exige la puerta del gestor.
    static constexpr const char *kName = "dominators";
};

/**
 * @brief Calcula los @c DomFacts de @p fn.
 * @param fn    Funcion SSA.  La entrada es su bloque 0.
 * @param edges De que grafo: @c All para analizar (con los `catch`),
 *              @c TerminatorOnly para colocar PHIs.
 * @return Tablas dimensionadas a @c fn.blocks.size().
 */
DomFacts compute_dom_facts(const ir::IrFunction &fn,
                           ir::IrEdgeWant edges = ir::IrEdgeWant::All);

/**
 * @brief La frontera de dominancia de cada bloque (Cytron et al.).
 *
 * `DF(b)` = los bloques donde el dominio de `b` deja de ser estricto: donde
 * hay que poner una PHI si `b` define algo.  Va APARTE de @c DomFacts porque
 * solo la pide quien construye SSA; calcularla siempre seria cobrarsela a
 * todos los demas.
 *
 * Mismo formato que @c BlockGraph: la frontera de `b` es `df[b]`, en el orden
 * en que se descubre.  Sin repetidos.
 *
 * @param d Los dominadores, del grafo que corresponda (quien coloca PHIs pide
 *          @c TerminatorOnly).
 */
BlockGraph compute_dominance_frontier(const DomFacts &d);

} // namespace analysis

#endif // VESTA_ANALYSIS_FACTS_DOM_FACTS_H
