/**
 * @file scc.cpp
 * @brief Implementacion de las componentes fuertemente conexas (Tarjan).
 */
#include "util/scc.h"

#include "util/named_alloc.h"

#include <algorithm>

namespace util {

namespace {

/// En que orden se descubrio un nodo, que es con lo que Tarjan compara.
enum SccOrder : uint32_t {};
/// Un nodo que el recorrido todavia no ha alcanzado.  Un orden de verdad nunca
/// llega aqui: habria que tener mas nodos de los que caben en el indice.
constexpr SccOrder SCC_UNVISITED = SccOrder(0xFFFFFFFFu);

/// Si el nodo esta en la pila de la componente que sigue abierta.
enum SccStackMark : uint8_t {
    SCC_OFF_STACK = 0, ///< Fuera: o sin visitar, o ya en una componente.
    SCC_ON_STACK = 1,  ///< Dentro: su componente aun no se ha cerrado.
};

/// Un paso pendiente del recorrido: el nodo y por que arista seguir.
struct SccFrame {
    SccNode node;
    SccEdge next;
};

/* Lo que el recorrido guarda por nodo, cada cosa con su nombre para que el
 * perfil de reservas sepa de quien es. */
struct SccIndexTag;   ///< Orden en que se descubrio cada nodo.
struct SccLowTag;     ///< El descubrimiento mas antiguo que alcanza.
struct SccOnStackTag; ///< El nodo esta en la pila de la componente abierta.
struct SccStackTag;   ///< Esa pila.
struct SccWorkTag;    ///< La pila del recorrido.

} // namespace

size_t tarjan_scc(const SccEdge *offsets, const SccNode *targets, size_t n,
                  SccComp *out_comp) {
    for (size_t i = 0; i < n; ++i)
        out_comp[i] = SCC_NO_COMP;
    NamedVector<SccOrder, SccIndexTag> order(n, SCC_UNVISITED);
    NamedVector<SccOrder, SccLowTag> low(n, SCC_UNVISITED);
    NamedVector<SccStackMark, SccOnStackTag> mark(n, SCC_OFF_STACK);
    NamedVector<SccNode, SccStackTag> stack;
    stack.reserve(n);
    uint32_t next_order = 0;
    uint32_t next_comp = 0;

    /* DFS iterativo (evita desbordar la pila del host con modulos grandes). */
    NamedVector<SccFrame, SccWorkTag> work;
    for (uint32_t s = 0; s < n; ++s) {
        if (order[s] != SCC_UNVISITED) continue;
        work.push_back({SccNode(s), offsets[s]});
        while (!work.empty()) {
            SccFrame &f = work.back();
            const SccNode v = f.node;
            /* La primera vez que se mira su marco: descubrirlo.  En UN sitio,
             * para la raiz y para cualquier otro -- un nodo solo se apila sin
             * descubrir, y es lo siguiente que se mira, asi que no puede
             * apilarse dos veces. */
            if (order[v] == SCC_UNVISITED) {
                order[v] = low[v] = SccOrder(next_order++);
                stack.push_back(v);
                mark[v] = SCC_ON_STACK;
            }
            if (f.next < offsets[v + 1]) {
                const SccNode w = targets[f.next];
                f.next = SccEdge(f.next + 1); // la proxima, la arista siguiente
                /* `f` no se vuelve a usar: apilar puede mover el vector. */
                if (order[w] == SCC_UNVISITED)
                    work.push_back({w, offsets[w]});
                else if (mark[w] == SCC_ON_STACK)
                    low[v] = std::min(low[v], order[w]);
                continue;
            }
            // Todas sus aristas vistas: cerrar la componente si v es raiz.
            if (low[v] == order[v]) {
                for (;;) {
                    const SccNode w = stack.back();
                    stack.pop_back();
                    mark[w] = SCC_OFF_STACK;
                    out_comp[w] = SccComp(next_comp);
                    if (w == v) break;
                }
                ++next_comp;
            }
            work.pop_back();
            if (!work.empty()) {
                const SccNode parent = work.back().node;
                low[parent] = std::min(low[parent], low[v]);
            }
        }
    }
    return next_comp;
}

} // namespace util
