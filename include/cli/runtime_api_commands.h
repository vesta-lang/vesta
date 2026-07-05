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
 * @file runtime_api_commands.h
 * @brief API de ejecucion de comandos sincrona y asincrona para el runtime de
 * la CLI.
 *
 * Proporciona funciones para ejecutar comandos de texto en el runtime de
 * VestaVM de forma sincrona (bloqueante con timeout opcional) o asincrona (via
 * future), junto con las funciones de inicializacion y cierre del thread pool
 * interno del runtime.
 *
 * Uso tipico:
 * @code
 *   runtime::runtime_init(4);                       // inicializar con 4 hilos
 *   std::string out = runtime::run_command_sync("run program.velb"); //
 * ejecutar sincrono runtime::runtime_shutdown();                     // cerrar
 * al terminar
 * @endcode
 */

#ifndef RUNTIME_API_COMMANDS_H
#define RUNTIME_API_COMMANDS_H

#include "util/ThreadPool.h"

#include <string>
#include <future>
#include <chrono>
#include <memory>
#include <atomic>

namespace runtime {

/**
 * @brief Token de cancelacion compartido entre el emisor y el ejecutor de un
 * comando.
 *
 * Cuando el booleano atomico se pone a true el ejecutor debe abortar la
 * ejecucion tan pronto como sea posible.  Se crea con make_cancel_token().
 */
using CancelToken = std::shared_ptr<std::atomic<bool>>;

/**
 * @brief Crea un nuevo token de cancelacion inicializado a false (sin
 * cancelar).
 *
 * @return Token de cancelacion listo para usar.
 */
CancelToken make_cancel_token();

/**
 * @brief Ejecuta un comando de forma sincrona en el runtime.
 *
 * Bloquea el hilo llamante hasta que el comando termina o se agota el timeout.
 * Si se proporciona un token de cancelacion, la ejecucion puede interrumpirse
 * poniendo el booleano a true desde otro hilo.
 *
 * @param cmd          Cadena de texto con el comando a ejecutar.
 * @param timeout      Tiempo maximo de espera; cero significa sin limite.
 * @param cancel_token Token de cancelacion (puede ser nullptr para ignorar
 * cancelacion).
 * @return Cadena con la salida producida por el comando, o un mensaje de error.
 */
std::string run_command_sync(
    const std::string &cmd,
    std::chrono::milliseconds timeout = std::chrono::milliseconds::zero(),
    CancelToken cancel_token = nullptr);

/**
 * @brief Envia un comando al thread pool interno para ejecucion asincrona.
 *
 * Devuelve un future que contendra la salida del comando cuando termine, o
 * propagara una excepcion si la ejecucion fallo.
 *
 * @param cmd          Cadena de texto con el comando a ejecutar.
 * @param cancel_token Token de cancelacion (puede ser nullptr para ignorar
 * cancelacion).
 * @return std::future<std::string> con el resultado de la ejecucion.
 */
std::future<std::string> run_command_async(const std::string &cmd,
                                           CancelToken cancel_token = nullptr);

/**
 * @brief Inicializa el thread pool interno del runtime con el numero de hilos
 * indicado.
 *
 * Si no se llama antes de usar run_command_async(), se crea automaticamente un
 * pool con std::thread::hardware_concurrency() hilos.  Llamar multiples veces
 * no tiene efecto.
 *
 * @param threads Numero de hilos del pool; 0 usa hardware_concurrency().
 */
void runtime_init(size_t threads = 0);

/**
 * @brief Cierra el runtime y espera a que todas las tareas pendientes terminen.
 *
 * Debe llamarse antes de salir del programa para garantizar que no quedan
 * tareas en vuelo ni hilos del pool activos.
 */
void runtime_shutdown();

} // namespace runtime

#endif // RUNTIME_API_COMMANDS_H
