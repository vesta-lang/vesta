/**
 * @file method_names.h
 * @brief Como se llama el SIMBOLO de un metodo, y los nombres de los metodos
 *        de ciclo de vida que el compilador llama sin que el programa los
 *        nombre en la llamada.
 *
 * El simbolo de un metodo es `Duenyo__metodo`; el destructor y el gancho de
 * copia siguen la misma regla con un nombre fijo.  Esos dos se armaban a mano
 * -- `tipo + "__" + "__dtor"` -- en una decena de sitios del bajado, del
 * comprobador y del backend de C.  Aqui se dice una vez como se llaman y como
 * se forma su simbolo.
 */
#ifndef VX_METHOD_NAMES_H
#define VX_METHOD_NAMES_H

#include <string>

namespace vx {

/**
 * @brief El SIMBOLO con el que se emite un metodo: `Duenyo__metodo`.
 *
 * Estaba escrito en SEIS sitios del bajado, cada uno armando la misma cadena a
 * mano.  Mirandolos de cerca no eran lo mismo -- y por eso no se colapsan en un
 * campo --: cada uno elige un dueno distinto A PROPoSITO.  Uno nombra al que
 * DEFINE el metodo, otro al escrito en la llamada, otro a la clase del aspecto.
 *
 * Lo que si se repetia es la REGLA de formar el nombre, y eso es lo que vive
 * aqui.  Cada llamante sigue diciendo de quien es el metodo; como se llama el
 * simbolo lo dice esta funcion, y se cambia en un sitio.
 *
 * @param owner  De quien es el metodo, que lo decide el llamante.
 * @param method Nombre del metodo.
 * @param tag    Lo que separa una SOBRECARGA de sus hermanas -- el mangleado de
 *               sus parametros --, o vacio si el nombre no esta sobrecargado y
 *               el metodo conserva su simbolo exacto.
 * @return El simbolo.
 */
inline std::string method_symbol(const std::string &owner,
                                 const std::string &method,
                                 const std::string &tag = std::string()) {
    if (tag.empty()) return owner + "__" + method;
    return owner + "__" + method + "_" + tag;
}

/**
 * @brief El nombre interno del destructor.  El programa escribe `~Tipo()`; el
 *        parser lo guarda con este nombre, y el comprobador lo usa tambien
 *        para el que sintetiza.
 */
inline constexpr const char kDestructorMethod[] = "__dtor";

/**
 * @brief El gancho de copia: un metodo que el PROGRAMA declara con este
 *        nombre en un struct, y que el compilador llama en cada sitio donde
 *        el struct se copia.
 */
inline constexpr const char kCopyHookMethod[] = "__clone__";

/**
 * @brief El operador de LECTURA por indice: `x[i]` sobre un struct o clase es
 *        `x.__index__(i)`.
 */
inline constexpr const char kIndexGetMethod[] = "__index__";

/**
 * @brief El operador de ESCRITURA por indice: `x[i] = v` sobre un struct o
 *        clase es `x.__index_set__(i, v)`.
 */
inline constexpr const char kIndexSetMethod[] = "__index_set__";

/**
 * @brief El simbolo del destructor de @p owner: `<owner>____dtor`.
 * @param owner Tipo dueno.
 * @return El simbolo.
 */
inline std::string destructor_symbol(const std::string &owner) {
    return method_symbol(owner, kDestructorMethod);
}

/**
 * @brief El simbolo del gancho de copia de @p owner: `<owner>____clone__`.
 * @param owner Struct dueno.
 * @return El simbolo.
 */
inline std::string copy_hook_symbol(const std::string &owner) {
    return method_symbol(owner, kCopyHookMethod);
}

} // namespace vx

#endif // VX_METHOD_NAMES_H
