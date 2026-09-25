/**
 * @file module_artifact.h
 * @brief El artefacto en cache de un modulo -- su interfaz `.vxi` y su
 *        intermedio `.vxir` -- y como se adopta al servirlo de la cache.
 *
 * Hay dos caches de donde puede salir un modulo ya compilado: el almacen comun
 * por contenido (CAS), que sirve a toda la maquina, y la de cada proyecto, por
 * ruta.  Las dos entregan lo mismo, y lo que se hace con ello tiene que ser lo
 * mismo: servido por una u otra, el modulo tiene que quedar IGUAL.  Estaba
 * escrito dos veces, y las dos copias ya no coincidian -- una verificaba el
 * intermedio restaurado y la otra no --.
 */
#ifndef VX_PROJECT_MODULE_ARTIFACT_H
#define VX_PROJECT_MODULE_ARTIFACT_H

#include "ir/ssa_ir.h"
#include "vx/module/vxi_format.h"

#include <cstdint>
#include <vector>

namespace vx {

struct ProjectModuleWork;

/**
 * @brief Empaqueta interfaz e intermedio en un blob del CAS.
 *
 * Formato: `[u32 len][vxi][u32 len][vxir]`, little-endian.  Los bytes van en
 * `std::vector<uint8_t>` porque es lo que pide y entrega `CasStore`.
 *
 * @param vxi  Bytes de la interfaz.
 * @param vxir Bytes del intermedio.
 * @return El blob.
 */
std::vector<uint8_t> pack_module_artifact(const std::vector<uint8_t> &vxi,
                                          const std::vector<uint8_t> &vxir);

/**
 * @brief Separa un blob del CAS en interfaz e intermedio.
 * @param blob Lo que devolvio el almacen.
 * @param vxi  Recibe la interfaz.
 * @param vxir Recibe el intermedio.
 * @return false si el blob esta truncado o mal formado.
 */
bool unpack_module_artifact(const std::vector<uint8_t> &blob,
                            std::vector<uint8_t> &vxi,
                            std::vector<uint8_t> &vxir);

/**
 * @brief Adopta en @p pm un modulo servido de una cache, venga de cual venga.
 *
 * Deja el modulo exactamente como lo dejaria compilarlo: su interfaz, su
 * intermedio (ENTERO, con `ir::adopt_cached_module`: campo a campo se olvido
 * tres veces), si declara clases -- lo pregunta el tree-shake y aqui no hay AST
 * --, y su conjunto comptime, que un modulo servido de la cache no puede sacar
 * de un AST que no tiene.  Y verifica el intermedio si se pidio: lo que sale
 * de una cache ya no tiene fuente al que volver.
 *
 * @param pm  El modulo.
 * @param vxi Su interfaz, ya leida y validada.
 * @param ir  Su intermedio, ya leido.
 */
void adopt_cached_artifact(ProjectModuleWork &pm, VxiModule &&vxi,
                           ir::IrModule &&ir);

} // namespace vx

#endif // VX_PROJECT_MODULE_ARTIFACT_H
