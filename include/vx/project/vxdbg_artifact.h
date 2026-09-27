/**
 * @file vxdbg_artifact.h
 * @brief El mapa del ARTEFACTO en el grafo de depuracion: de cada simbolo del
 *        ejecutable a su entidad, y los mapas de los modulos que contiene.
 *
 * Uno solo para todo el programa: el ejecutable contiene todos los modulos y
 * una direccion suya puede caer en cualquiera.  Lo componia el camino de
 * proyecto a mano y el de fichero suelto lo sacaba de su unico modulo por
 * otro lado; aqui lo hacen los dos igual.
 */
#ifndef VX_PROJECT_VXDBG_ARTIFACT_H
#define VX_PROJECT_VXDBG_ARTIFACT_H

#include <vector>

namespace vx {

struct CompileOptions;
struct CompileResult;
struct ProjectModuleWork;

/**
 * @brief Compone y guarda el mapa del artefacto, y publica en @p res su huella
 *        y la de los tramos de fuente del raiz.
 *
 * Los SIMBOLOS son para buscar (dada una direccion, su entidad) y solo los
 * trae el modulo que se bajo en esta compilacion; el mapa de cada modulo es
 * para SOSTENER su grafo, y lo traen todos, tambien los servidos de la cache.
 *
 * Sin artefacto (`ir_only`) no hace nada.
 *
 * @param work Los modulos, con sus huellas ya recogidas
 *             (@ref gather_unit_results).
 * @param opts Opciones de la compilacion (directorio del grafo, `ir_only`).
 * @param res  Recibe `vxdbg_artifact_map` y `vxdbg_span_map`.
 */
void compose_vxdbg_artifact(const std::vector<ProjectModuleWork> &work,
                            const CompileOptions &opts, CompileResult &res);

} // namespace vx

#endif // VX_PROJECT_VXDBG_ARTIFACT_H
