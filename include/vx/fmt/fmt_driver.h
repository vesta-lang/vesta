/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file fmt_driver.h
 * @brief Formatear un fichero, resolviendo lo que no cabe en uno solo.
 *
 * `fmt.h` es la libreria pura: texto entra, texto sale.  Esto es la capa que
 * ademas lee del disco, y existe por una razon concreta: saber que funciones
 * capturan el TEXTO de su argumento (`R110`) requiere mirar los OTROS ficheros
 * del proyecto, porque las importadas se declaran en otro modulo.
 *
 * Quien llama decide que ficheros son -- el compilador conoce sus fuentes, el
 * LSP su espacio de trabajo --; aqui no se recorren directorios.
 */

#ifndef VX_FMT_FMT_DRIVER_H
#define VX_FMT_FMT_DRIVER_H

#include "vx/fmt/fmt.h"

namespace vx {
namespace fmt {

/**
 * @brief Nombres de funciones que capturan el texto de su argumento (`R110`).
 *
 * @param paths Ficheros `.vx` donde buscarlos.  Los que no se puedan leer o
 *              trocear se saltan sin ruido: un nombre de menos deja una
 *              llamada sin proteger, pero un error aqui no debe impedir
 *              formatear.
 * @return Los nombres encontrados, sin repetidos.
 */
std::vector<std::string>
capture_names_in_files(const std::vector<std::string> &paths);

/**
 * @brief Formatea un fichero del disco.
 *
 * @param path          Fichero a formatear.
 * @param project_files Los demas ficheros del proyecto, de donde salen los
 *                      nombres de `R110` que este no puede ver.  Vacio para
 *                      formatear un fichero suelto.
 * @param options       Ajustes del estandar.  Sus `raw_capture_names` se
 *                      CONSERVAN y se les anaden los encontrados.
 * @return El texto formateado, o el codigo del motivo por el que no se toco.
 */
FormatResult format_file(const std::string &path,
                         const std::vector<std::string> &project_files,
                         FormatOptions options = FormatOptions{});

} // namespace fmt
} // namespace vx

#endif // VX_FMT_FMT_DRIVER_H
