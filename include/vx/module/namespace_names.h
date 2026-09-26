/**
 * @file namespace_names.h
 * @brief El nombre FISICO de lo que se declara dentro de un namespace.
 *
 * `std.collections.Vector` se emite como `std__collections__Vector`: la ruta
 * punteada pasa a llevar `__` entre segmentos, y el miembro va detras con el
 * mismo separador.  Es una representacion, y estaba escrita a mano en cada
 * sitio que la necesitaba -- el aplanado, la inyeccion de interfaces, la
 * inyeccion de plantillas, los ganchos del raiz --, cinco veces el recorrido
 * que cambia los puntos y una docena la union con el miembro.  Bastaba con que
 * una se separara para que un simbolo se buscara con un nombre y se hubiera
 * emitido con otro.
 *
 * El nombre HUMANO (el que se resuelve, `a.b.c`) conserva los puntos; esto es
 * solo el que acaba en el intermedio.
 */
#ifndef VX_MODULE_NAMESPACE_NAMES_H
#define VX_MODULE_NAMESPACE_NAMES_H

#include <string>

namespace vx {

/// Separador entre los segmentos de un nombre fisico: entre los de la ruta de
/// un namespace, y entre la ruta (o el modulo) y el miembro.
inline constexpr const char kSymbolPathSeparator[] = "__";

/**
 * @brief La ruta de un namespace en su forma fisica: `a.b.c` -> `a__b__c`.
 *
 * Un nombre de un solo segmento sale tal cual.
 *
 * @param dotted Ruta con puntos.
 * @return La ruta con el separador fisico.
 */
std::string namespace_symbol_path(const std::string &dotted);

/**
 * @brief Une una ruta YA fisica (de namespace o de modulo) con un miembro.
 * @param path   Ruta fisica, sin separador final.
 * @param member Nombre del miembro tal como se escribio.
 * @return `path__member`.
 */
std::string qualified_symbol(const std::string &path,
                             const std::string &member);

/**
 * @brief El simbolo de un miembro de un namespace escrito con puntos:
 *        `a.b` y `f` -> `a__b__f`.
 * @param dotted Ruta del namespace, con puntos.
 * @param member Nombre del miembro.
 * @return El simbolo fisico.
 */
std::string namespace_member_symbol(const std::string &dotted,
                                    const std::string &member);

/**
 * @brief Si @p name es un miembro de la ruta fisica @p path, su nombre corto.
 *
 * Un nombre que no lleva ese prefijo -- `main`, que no se renombra, o un
 * identificador reservado -- se devuelve tal cual.
 *
 * @param name Simbolo fisico.
 * @param path Ruta fisica; vacia devuelve @p name.
 * @return El nombre sin el prefijo `path__`, o @p name si no lo lleva.
 */
std::string strip_symbol_path(const std::string &name, const std::string &path);

} // namespace vx

#endif // VX_MODULE_NAMESPACE_NAMES_H
