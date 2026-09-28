/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file import_origin.h
 * @brief La identidad de lo que llega de otro modulo: con que se califica su
 *        nombre, en UN sitio.
 *
 * Un simbolo importado se llama por el namespace donde se DECLARA, o por su
 * modulo si no declara ninguno.  Esa decision estaba escrita a mano en cada
 * camino de importacion, y los caminos ya no coincidian: uno calificaba una
 * sola vez y otro volvia a calificar lo que ya lo estaba (en la MISMA funcion,
 * el esqueleto de un tipo y el tipo completo iban a dos claves distintas); las
 * plantillas se nombraban por el alias que eligio quien importa, o por nada; y
 * con un namespace vacio salia `__T`, un nombre del compilador.
 *
 * Aqui esta la regla; quien importa solo dice de DONDE viene lo que importa.
 * Lo que pone el consumidor (`only`, `as L`) sirve para BUSCAR, no para
 * nombrar.
 */

#ifndef VX_MODULE_IMPORT_ORIGIN_H
#define VX_MODULE_IMPORT_ORIGIN_H

#include <string>

#include "util/name_pool.h" // util::InternedName

namespace vx {

/**
 * @struct ImportOrigin
 * @brief De donde viene lo que se importa: el modulo que lo exporta.
 */
struct ImportOrigin {
    /// El calificador de lo que ese modulo declara SIN namespace: el nombre
    /// del modulo.  Nunca vacio.
    util::InternedName module;
};

/**
 * @brief La identidad de un modulo importado.
 * @param module_name El nombre del modulo.
 * @return Su origen.
 */
ImportOrigin import_origin_of(const std::string &module_name);

/**
 * @brief El simbolo de algo que llega de @p origin.
 *
 * Por el namespace donde se DECLARA (@p ns_path) o, si no declara ninguno, por
 * su modulo.  Una sola vez: un nombre que ya viene calificado -- por una cadena
 * de re-exports -- se respeta, porque el exportador parte siempre un nombre
 * publico en (namespace, nombre corto) y el corto no lleva `__`.
 *
 * @param origin  De donde viene.
 * @param ns_path Namespace declarado, con puntos; vacio si no declara.
 * @param name    Su nombre.
 * @return El simbolo.
 */
std::string imported_symbol(const ImportOrigin &origin,
                            const std::string &ns_path,
                            const std::string &name);

} // namespace vx

#endif // VX_MODULE_IMPORT_ORIGIN_H
