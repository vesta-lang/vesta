/**
 * @file unit_results.h
 * @brief Juntar en el resultado lo que dejo la compilacion de cada modulo.
 *
 * Cada modulo se compila escribiendo SOLO en su `ProjectModuleWork` -- varios
 * a la vez en hilos distintos --, y al terminar todos se suma en el resultado
 * de la compilacion, en orden de indice.  Lo hacian a mano el camino de
 * proyecto y el de fichero suelto, cada uno con sus campos; aqui una vez.
 */
#ifndef VX_UNIT_UNIT_RESULTS_H
#define VX_UNIT_UNIT_RESULTS_H

#include <vector>

namespace vx {

struct CompileResult;
struct ProjectModuleWork;

/**
 * @brief Suma en @p res lo de cada modulo de @p work: diagnosticos, conjunto
 *        comptime, lo que se sabe de cada `@Macro`, y las huellas de su grafo
 *        de depuracion (que se guardan aparte del `.vxi`, que se suelta
 *        despues).
 *
 * Tiene que llamarse cuando TODOS los modulos han terminado y antes de soltar
 * sus interfaces.
 *
 * @param work Los modulos, en orden topologico.
 * @param res  El resultado; se pone @c ok a false si alguno fallo.
 * @return false si algun modulo fallo.
 */
bool gather_unit_results(std::vector<ProjectModuleWork> &work,
                         CompileResult &res);

} // namespace vx

#endif // VX_UNIT_UNIT_RESULTS_H
