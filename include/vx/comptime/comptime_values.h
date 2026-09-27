/**
 * @file comptime_values.h
 * @brief Los valores que el comprobador calculo al compilar, para ensenarlos
 *        en el editor (`vesta/comptimeValues`).
 *
 * Solo lo hacia el camino de fichero suelto, asi que un documento con
 * `namespace` o con `import` -- que se compila como proyecto -- no ensenaba
 * ninguno.  Lo usan los dos caminos desde aqui.
 */
#ifndef VX_COMPTIME_COMPTIME_VALUES_H
#define VX_COMPTIME_COMPTIME_VALUES_H

#include "vx/compiler.h"

#include <vector>

namespace vx {

class TypeChecker;

/**
 * @brief Anade a @p out los valores comptime que @p tc resolvio: las
 *        constantes de nivel superior, las locales de los bloques
 *        `comptime { }` y lo que contestaron los builtins de introspeccion.
 *
 * Las locales de los bloques solo estan si se le pidio al comprobador que las
 * capturase ANTES de ejecutarlo (`set_capture_comptime_block_locals`).
 *
 * @param tc  El comprobador, ya ejecutado.
 * @param out Donde se anaden.
 */
void collect_comptime_values(
    const TypeChecker &tc,
    std::vector<CompileResult::ComptimeValueSnapshot> &out);

} // namespace vx

#endif // VX_COMPTIME_COMPTIME_VALUES_H
