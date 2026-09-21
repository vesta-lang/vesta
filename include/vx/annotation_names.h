/**
 * @file annotation_names.h
 * @brief La tabla de las anotaciones que el lenguaje conoce.
 *
 * Existe por un agujero de clase: el parser leia el nombre tras el `@`, lo
 * comparaba con los que sabe manejar, y **lo que no reconocia lo descartaba sin
 * decir nada**.  Una errata (`@Ovrride`, `@Provdes`) compilaba, salia con
 * codigo cero, y la anotacion no hacia NADA -- que con un `@Override` es lo que
 * separa redefinir un metodo de anyadir una sobrecarga, y con un `@Provides` es
 * que el binario use el asignador de la biblioteca en vez del tuyo --.
 *
 * Y no era un sitio: el vocabulario estaba repartido por SIETE lugares del
 * parser (nivel superior, miembro de clase, miembro de struct, campo de struct,
 * typedef, contratos y efectos), cada uno con su lista escrita a mano.  Nadie
 * podia decir que anotaciones tiene el lenguaje sin leerlos todos.
 *
 * Aqui estan todas, UNA vez y en orden, para poder preguntarlo.  Es la misma
 * forma que la tabla de builtins (@c builtin_names.h) y que la de variables de
 * entorno: lo que se puede escribir se enumera en un sitio.
 *
 * ALCANCE DE HOY: la tabla dice si un nombre EXISTE, no donde vale.  Un
 * `@Getter` escrito en el nivel superior sigue sin dar error -- existe --, y
 * eso es un problema aparte y mas pequenyo: son las reglas de COLOCACION, que
 * hay que acordar una por una.  Lo que esto cierra es la errata.
 */
#ifndef VX_ANNOTATION_NAMES_H
#define VX_ANNOTATION_NAMES_H

#include <cstddef>
#include <string>

namespace vx {

/**
 * @brief Dice si @p name es una anotacion del lenguaje.
 *
 * @param name Nombre TAL CUAL se escribio, sin el `@`.
 * @return true si el lenguaje la conoce.
 */
bool annotation_exists(const std::string &name) noexcept;

/**
 * @brief El nombre conocido mas parecido a @p name, para sugerirlo.
 *
 * Quien se equivoca aqui casi siempre esta a una letra o dos de la buena, asi
 * que el mensaje puede decirla en vez de mandar a buscarla.  Se exige un
 * parecido MINIMO: sin eso, un nombre inventado de arriba abajo arrastraba
 * cualquier entrada de la tabla y la sugerencia despistaba mas que ayudar.
 *
 * @param name Nombre escrito, sin el `@`.
 * @return El nombre sugerido, o nullptr si ninguno se parece lo bastante.
 */
const char *annotation_nearest(const std::string &name) noexcept;

/**
 * @brief Recorre la tabla, para listarla o probarla.
 * @param out_count Recibe cuantas hay.
 * @return Array de punteros a los nombres, en orden alfabetico.
 */
const char *const *annotation_all(size_t *out_count) noexcept;

} // namespace vx

#endif // VX_ANNOTATION_NAMES_H
