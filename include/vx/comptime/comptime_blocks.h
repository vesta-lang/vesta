/**
 * @file comptime_blocks.h
 * @brief Los `comptime { }` de nivel de modulo, convertidos en funciones.
 *
 * Un bloque `comptime { ... }` se ejecuta al compilar.  Se convierte en una
 * funcion comptime sintetica y en una constante que la llama, y de ahi en
 * adelante corre como cualquier otra funcion comptime: en la maquina de
 * compilacion, compilado por el JIT.  NO en el evaluador de arbol del
 * comprobador, que es lo que se esta retirando.
 *
 * Estaba escrito solo en el camino de fichero suelto: el de proyecto -- el
 * que usan los programas reales -- seguia mandando esos bloques al evaluador
 * de arbol, asi que el mismo `comptime { }` corria en un motor o en otro
 * segun por donde se compilara.
 */
#ifndef VX_COMPTIME_COMPTIME_BLOCKS_H
#define VX_COMPTIME_COMPTIME_BLOCKS_H

#include "vx/ast.h"

#include <string>

namespace vx {

/**
 * @brief Convierte cada `comptime { }` de nivel de modulo en
 *        `comptime i64 __ctblock_N() { <sentencias>; return 0; }` mas
 *        `const i64 __ctblock_N_r = __ctblock_N();`.
 *
 * Las sinteticas van al FINAL de las declaraciones: el bloque corre despues
 * de todas las globales del modulo, como antes.
 *
 * @param mod   El modulo, con los namespaces ya aplanados.
 * @param owner El modulo que las declara, para que dos modulos no den el
 *              mismo nombre al fusionarse; vacio en el raiz.
 */
void comptime_blocks_to_functions(ast::ModuleNode &mod,
                                  const std::string &owner);

} // namespace vx

#endif // VX_COMPTIME_COMPTIME_BLOCKS_H
