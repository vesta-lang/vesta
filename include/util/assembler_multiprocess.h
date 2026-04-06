/**
 * @file assembler_multiprocess.h
 * @brief Infraestructura para ensamblado paralelo usando multiproceso + multihilo.
 *
 * Este módulo implementa el modelo "driver + workers" de VestaVM:
 *
 *  - El **driver** ejecuta múltiples hilos (ThreadPool).
 *  - Cada hilo lanza un **worker** como proceso independiente (`vm.exe --worker`).
 *  - El driver captura la salida de cada worker, registra tiempos y errores,
 *    y finalmente ejecuta el linker global.
 *
 * Ventajas del modelo:
 *  - Aislamiento total entre workers (cada uno es un proceso separado).
 *  - Uso eficiente de todos los núcleos (multihilo en el driver).
 *  - Seguridad: un worker que falla no afecta al driver.
 *  - Escalabilidad: miles de archivos pueden ensamblarse en paralelo.
 *
 * @note Este header solo declara la interfaz; la implementación está en el .cpp.
 */

#ifndef ASSEMBLER_MULTIPROCESS_H
#define ASSEMBLER_MULTIPROCESS_H

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "json.hpp"
#include "emmit/parser_to_bytecode.h"

#include "cli/sync_io.h"
#include "util/ThreadPool.h"

namespace asm_multi_process {
    using namespace Assembly::Bytecode;
    using json = nlohmann::json;

    /**
     * @brief Nombre del archivo que está procesando cada worker.
     *
     * El índice corresponde al ID de tarea en el ThreadPool.
     * Se usa únicamente para mostrar en la barra de progreso qué archivo
     * fue el último en finalizar.
     */
    static std::vector<std::string> worker_current_file;

    /**
     * @brief Número de workers que ya han terminado.
     *
     * Incrementado por cada tarea del ThreadPool al finalizar.
     * Usado por la barra de progreso.
     */
    static std::atomic<int> completed_workers{0};

    /**
     * @brief Número total de archivos a procesar.
     *
     * Se establece al inicio del driver.
     */
    static int total_workers = 0;

    /**
     * @brief Mutex para proteger la salida de la barra de progreso.
     */
    static std::mutex progress_mutex;

    /**
     * @brief Índice del último worker que terminó.
     *
     * Permite mostrar en la barra de progreso el archivo más reciente.
     */
    static std::atomic<int> last_finished{-1};

    /**
     * @brief Indica si el hilo de progreso debe seguir ejecutándose.
     */
    static std::atomic<bool> progress_running{false};

    /**
     * @brief Imprime la barra de progreso del ensamblado paralelo.
     *
     * Muestra:
     *  - porcentaje completado
     *  - número de archivos procesados
     *  - nombre del último archivo finalizado
     *
     * Thread-safe mediante `progress_mutex`.
     */
    void print_progress();

    /**
     * @brief Hilo dedicado a actualizar la barra de progreso.
     *
     * Se ejecuta mientras `progress_running == true`.
     * Actualiza la barra cada 100 ms.
     */
    static void progress_thread_func() {
        while (progress_running.load()) {
            print_progress();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        print_progress();
        std::cout << "\n";
    }

    /**
     * @brief Compila en paralelo todos los archivos .vel dentro de un directorio.
     *
     * Funciona como un "driver" que:
     *  - Busca archivos `.vel` en el directorio.
     *  - Lanza un ThreadPool con N hilos.
     *  - Cada hilo ejecuta un worker como proceso externo.
     *  - Captura la salida de cada worker.
     *  - Registra tiempos, errores y logs.
     *  - Ejecuta el linker final si no hubo errores.
     *
     * @param folder   Directorio donde buscar archivos .vel.
     * @param threads  Número de hilos (0 = automático según hardware).
     * @param output   Nombre del archivo final generado por el linker.
     *
     * @return EXIT_SUCCESS si todo fue correcto, EXIT_FAILURE si hubo errores.
     */
    int run_driver(const std::string &folder, int threads, const std::string &output);

    /**
     * @brief Ejecuta el ensamblado de un único archivo en modo worker.
     *
     * Este modo es invocado por el driver o por scripts externos.
     * Realiza:
     *  - lectura del archivo fuente
     *  - lexing
     *  - parsing
     *  - ensamblado a bytecode
     *  - linking local
     *  - generación de .velb y .velb-map
     *
     * @param file_name      Ruta del archivo fuente .vel.
     * @param output_prefix  Prefijo para los archivos de salida.
     *
     * @return EXIT_SUCCESS si el ensamblado fue correcto, EXIT_FAILURE si hubo errores.
     */
    int run_worker(const std::string &file_name,
                   const std::string &output_prefix);

    /**
     * @brief Ejecuta un comando externo y captura toda su salida.
     *
     * Usado por el driver para ejecutar `vm.exe --worker`.
     *
     * @param cmd Comando completo a ejecutar.
     * @return Cadena con toda la salida estándar del proceso.
     */
    std::string run_and_capture(const std::string &cmd);
} // namespace asm_multi_process

#endif // ASSEMBLER_MULTIPROCESS_H
