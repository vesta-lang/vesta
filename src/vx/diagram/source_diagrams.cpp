/**
 * @file source_diagrams.cpp
 * @brief Los diagramas del AST y de los tipos, en su momento.
 * @see vx/diagram/source_diagrams.h
 */
#include "vx/diagram/source_diagrams.h"

#include "vx/compiler.h"
// Despues de `ast.h`: su declaracion adelantada de `ast::ModuleNode` tiene que
// resolver al tipo ya conocido.
#include "vx/diagram/graphviz_diagrams.h"
#include "vx/diagram/html_diagrams.h"
#include "vx/diagram/mermaid_diagrams.h"

namespace vx {

void fill_source_diagrams(const ast::ModuleNode &mod,
                          const CompileOptions &opts, CompileResult &res) {
    /* El arbol, con los tipos resueltos pero sin lo que el bajado le hace:
     * es la estructura del programa tal como se escribio. */
    if (opts.dump_mermaid_ast) res.mermaid_ast = mermaid_from_ast(mod);
    if (opts.dump_graphviz_ast) res.graphviz_ast = graphviz_from_ast(mod);
    if (opts.dump_html_ast) res.html_ast = html_from_ast(mod);
    /* Los tipos (clases, herencia, interfaces, structs, enums, conceptos):
     * la vista de alto nivel, independiente del detalle del arbol. */
    if (opts.dump_mermaid_types) res.mermaid_types = mermaid_types_from_ast(mod);
    if (opts.dump_graphviz_types)
        res.graphviz_types = graphviz_types_from_ast(mod);
    if (opts.dump_html_types)
        res.html_types =
            html_from_dot(graphviz_types_from_ast(mod), "Types", "types");
}

} // namespace vx
