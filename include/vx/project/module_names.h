/**
 * @file module_names.h
 * @brief Los nombres de simbolo que se DERIVAN del nombre de un modulo.
 *
 * Como se forma el nombre de un simbolo de un modulo -- con que separador, con
 * que prefijo -- es una representacion, y tiene que vivir en UN sitio: estaba
 * escrita a mano en cada punto que la necesitaba (`module + "__" + name`,
 * `"__module_init_" + module`), y bastaba con que uno de ellos se separara para
 * que un simbolo se buscara con un nombre y se hubiera emitido con otro -- lo
 * que no da un error al compilar, da "simbolo no resuelto" al enlazar o, peor,
 * el simbolo equivocado.
 */
#ifndef VX_PROJECT_MODULE_NAMES_H
#define VX_PROJECT_MODULE_NAMES_H

#include <string>

namespace vx {

/**
 * @brief El prefijo con que se renombran los simbolos de nivel superior de un
 *        modulo que no es el raiz.
 *
 * Es tambien lo que se le quita al exportar su interfaz: el consumidor importa
 * `sumar`, pero la firma lleva el simbolo renombrado.
 *
 * @param module Nombre del modulo.
 * @return El prefijo.
 */
std::string module_symbol_prefix(const std::string &module);

/**
 * @brief El simbolo de un miembro de nivel superior de un modulo, ya
 *        renombrado.
 * @param module Nombre del modulo.
 * @param member Nombre del miembro tal como se escribe en el fuente.
 * @return El simbolo con que aparece en el intermedio fusionado.
 */
std::string module_member_symbol(const std::string &module,
                                 const std::string &member);

/**
 * @brief El `__module_init` de una dependencia, renombrado para no chocar con
 *        el del raiz ni con el de las demas al fusionar.
 *
 * Sin nombre propio el merge dejaba varias etiquetas `__module_init` y solo se
 * ejecutaba la primera: las clases de las otras dependencias no se registraban.
 *
 * @param module Nombre de la dependencia.
 * @return El simbolo.
 */
std::string module_init_symbol(const std::string &module);

/**
 * @brief Una TANDA del `__module_init` de una dependencia (`__module_init_partN`),
 *        renombrada igual que la principal.
 *
 * Las tandas se llaman igual en todos los modulos, asi que sin esto dos
 * dependencias aportan `__module_init_part0` y la fusion se queda con una.
 *
 * @param part   Nombre de la tanda.
 * @param module Nombre de la dependencia.
 * @return El simbolo.
 */
std::string module_init_part_symbol(const std::string &part,
                                    const std::string &module);

} // namespace vx

#endif // VX_PROJECT_MODULE_NAMES_H
