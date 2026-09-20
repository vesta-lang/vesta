/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vx/builtin_params.h
 * @brief Como se llaman las ranuras de cada builtin.  UN solo sitio.
 *
 * El nombre de un parametro no es cosmetica: desde que una llamada puede
 * nombrar la ranura (`f(.a = 3)`), **es parte del contrato** -- viaja en el
 * `.vxi` y renombrarlo rompe a quien llama --.  Y es lo que separa a dos
 * funciones que toman lo mismo, asi que sin el un builtin no puede
 * SOBRECARGARSE: `overload.h` dice que una lista de nombres vacia "no separa",
 * de modo que colisionaba con cualquier funcion del usuario de sus mismos
 * tipos.
 *
 * Estaban escritos en el servidor de lenguaje, para ensenarlos en el editor, y
 * el comprobador de tipos no los conocia: dos sitios que decian del mismo
 * builtin cosas que podian separarse sin que nadie se enterara.  Aqui se
 * producen una vez y los leen los dos, que es lo que hace que el editor y el
 * compilador no puedan discrepar.
 */
#ifndef VX_BUILTIN_PARAMS_H
#define VX_BUILTIN_PARAMS_H

#include <cstdint>
#include <string_view>

namespace vx {

/**
 * @brief Las ranuras de un builtin: sus nombres, en orden.
 *
 * @c names apunta a literales estaticos; @c count es cuantos hay.  Un builtin
 * sin parametros -- o al que todavia no se le han decidido -- devuelve
 * @c count 0, y eso NO separa: ver la cabecera del fichero.
 */
struct BuiltinParams {
    const char *const *names = nullptr;
    uint8_t count = 0;
};

/**
 * @brief Las ranuras de @p name, o una lista vacia si no se le conocen.
 *
 * @param name Nombre del builtin, tal cual se escribe en el fuente.
 * @return Sus ranuras; @c count 0 si no toma parametros o no constan.
 */
BuiltinParams builtin_params_of(std::string_view name) noexcept;

} // namespace vx

#endif // VX_BUILTIN_PARAMS_H
