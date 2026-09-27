/**
 * @file source_diagrams.h
 * @brief Los diagramas del FUENTE -- el AST y los tipos -- que pide quien
 *        compila, en el momento en que se sacan.
 *
 * Se sacan despues de comprobar los tipos, con cada expresion ya tipada, y
 * antes de bajar, que reescribe el arbol.  Estaban escritos en los dos
 * caminos de compilacion y no decian lo mismo: el de proyecto dibujaba el
 * arbol del raiz DESPUES de bajarlo y se olvidaba de los diagramas de tipos.
 */
#ifndef VX_DIAGRAM_SOURCE_DIAGRAMS_H
#define VX_DIAGRAM_SOURCE_DIAGRAMS_H

#include "vx/ast.h"

namespace vx {

struct CompileOptions;
struct CompileResult;

/**
 * @brief Rellena en @p res los diagramas del AST y de los tipos que pida
 *        @p opts.  Sin ninguno pedido no hace nada.
 * @param mod  El modulo, ya comprobado y sin bajar todavia.
 * @param opts Que diagramas se piden.
 * @param res  Donde se dejan.
 */
void fill_source_diagrams(const ast::ModuleNode &mod,
                          const CompileOptions &opts, CompileResult &res);

} // namespace vx

#endif // VX_DIAGRAM_SOURCE_DIAGRAMS_H
