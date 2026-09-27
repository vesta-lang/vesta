/**
 * @file single_unit.h
 * @brief Compilar un fichero suelto con las MISMAS piezas que un proyecto:
 *        es un proyecto de un modulo.
 *
 * El camino de fichero suelto tenia su propia copia de la mitad delantera
 * -- comprobar, bajar, lo que el raiz teje --, y cada vez que una de las dos
 * aprendia algo la otra se quedaba atras sin que nada fallara.  Ahora las
 * dos pasan por `compile_unit`.
 */
#ifndef VX_UNIT_SINGLE_UNIT_H
#define VX_UNIT_SINGLE_UNIT_H

#include <vector>

namespace vx {

struct CompileOptions;
struct CompileResult;
struct ProjectModuleWork;

/**
 * @brief Compila el unico modulo de @p work como el raiz de un proyecto de un
 *        modulo, sin cache, y suma en @p res lo que deje.
 *
 * El modulo tiene que llegar parseado (`ast`), con su fuente, su ruta y su
 * nombre.  Al volver, su `ProjectModuleWork` conserva el AST, el comprobador y
 * el IR bajado para quien siga compilando.
 *
 * @param work El modulo, uno solo.
 * @param opts Opciones de la compilacion.
 * @param res  El resultado: diagnosticos, lo que teje el raiz y lo que
 *             `gather_unit_results` suma.
 * @return false si fallo; el motivo ya esta en @p res.
 */
bool compile_single_unit(std::vector<ProjectModuleWork> &work,
                         const CompileOptions &opts, CompileResult &res);

} // namespace vx

#endif // VX_UNIT_SINGLE_UNIT_H
