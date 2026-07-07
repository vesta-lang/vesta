/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file toolchain/toolchain.h
 * @brief Driver reutilizable de compilacion Vesta (.vx -> .velb).
 *
 * Extrae el nucleo del flujo de compilacion que hasta ahora vivia inline en
 * @c main.cpp para que lo compartan varios consumidores sin duplicar la
 * orquestacion ni divergir:
 *   - el ejecutable @c vm (CLI),
 *   - el servidor @c vesta_lsp (que embebe el compilador: metodos
 *     @c vesta/compile y @c vesta/compileProject),
 *   - la libreria cliente Python (a traves del LSP).
 *
 * El modulo vive en su propia libreria estatica (@c vesta_toolchain) porque
 * necesita a la vez el frontend (@c vx_lib: @c compile_vx_source /
 * @c compile_vx_project) y el back-end de ensamblado/linkado (@c vmcore:
 * @c run_worker).  @c vx_lib es deliberadamente standalone (no depende de
 * @c vmcore), de ahi que el pegamento entre ambos sea una tercera libreria.
 *
 * @note Solo COMPILA (produce artefactos en disco).  La EJECUCION de un
 *       programa se hace en un PROCESO aparte (el binario @c vm), nunca en el
 *       proceso del LSP: un @c print del programa escribiria en el stdout del
 *       servidor y corromperia el canal JSON-RPC.
 */

#ifndef VESTA_TOOLCHAIN_TOOLCHAIN_H
#define VESTA_TOOLCHAIN_TOOLCHAIN_H

#include <cstdint>
#include <string>
#include <vector>

namespace vesta {
namespace tc {

/**
 * @enum ExecMode
 * @brief Modo de ejecucion/compilacion objetivo (equivalente a @c -m).
 *
 * @c VM y @c JIT producen el mismo @c .velb (el JIT decide en runtime segun el
 * umbral de invocaciones); se distinguen para futura seleccion de opciones.
 * @c AOT produce un artefacto nativo standalone (POO nativa).
 */
enum class ExecMode { VM, JIT, AOT };

/**
 * @enum DiagLevel
 * @brief Severidad de un diagnostico (espejo de @c vx::DiagLevel, sin acoplar
 *        el header publico del toolchain al del frontend).
 */
enum class DiagLevel { Error, Warning, Note };

/**
 * @struct Diag
 * @brief Un diagnostico del compilador con su ubicacion.
 */
struct Diag {
    DiagLevel level = DiagLevel::Error; ///< Severidad.
    uint32_t line = 0;                  ///< Linea 1-based (0 = sin ubicacion).
    uint32_t column = 0;                ///< Columna 1-based.
    std::string message;                ///< Texto del problema.
    std::string file;                   ///< Fichero fuente (si se conoce).
};

/**
 * @struct CompileRequest
 * @brief Parametros de una compilacion (equivalente a los flags de @c main.cpp).
 */
struct CompileRequest {
    /// Fuente: un fichero @c .vx (single-file) o el @c .vx raiz de un proyecto
    /// con @c import (multi-fichero).  Ver @c is_project.
    std::string input;
    /// Contenido del fichero raiz.  Si esta vacio se lee de @c input.  Permite
    /// al LSP compilar el buffer en memoria sin escribirlo antes.
    std::string source_overlay;
    /// Prefijo de salida (@c -o).  El artefacto es @c <output>.velb.  Vacio =
    /// derivar del nombre base de @c input.
    std::string output;
    /// Nombre logico del modulo (por defecto "main").
    std::string module_name = "main";
    /// Modo objetivo.
    ExecMode mode = ExecMode::VM;
    /// Compilar como PROYECTO (resuelve @c import del disco).  Si es false, se
    /// compila solo @c input como fichero suelto.
    bool is_project = false;
    /// Emitir informacion de depuracion (mapeo linea<->bytecode en el .velb).
    bool debug = false;
    /// Modo de instrumentacion (vacio o "none" = sin instrumentar).  Se pasa
    /// tal cual al lowering (@c CompileOptions::instrument_mode).
    std::string instrument;
    /// Mantener los nombres de label en el .velb (util para depurar el linker).
    bool keep_labels = false;
    /// Emitir un @c .velb-map con info de simbolos.
    bool emit_map = false;
    /// Desactivar el preprocesador VPP.
    bool no_preprocessor = false;
    /// Rutas extra de busqueda para resolver @c import (proyectos anidados).
    std::vector<std::string> search_paths;
};

/**
 * @struct CompileResponse
 * @brief Resultado de una compilacion.
 */
struct CompileResponse {
    bool ok = false;              ///< true si se produjo el artefacto sin errores.
    std::string output_path;      ///< Ruta del @c .velb producido (si @c ok).
    std::vector<Diag> diagnostics;///< Errores + warnings + notas del frontend.
    uint64_t frontend_us = 0;     ///< Microsegundos del frontend (.vx -> .vel).
    std::string message;          ///< Nota adicional (p.ej. razon de un fallo).
};

/**
 * @brief Compila un fuente Vesta a @c .velb usando el compilador embebido.
 *
 * Reutiliza el mismo camino canonico que @c vm: @c compile_vx_source /
 * @c compile_vx_project para el frontend y @c run_worker para ensamblar +
 * linkar el @c .vel intermedio al @c .velb final (con el IR embebido).
 *
 * @param req Parametros de la compilacion.
 * @return El resultado; si @c ok es false, @c diagnostics explica por que.
 *
 * @note El modo @c AOT (artefacto nativo @c .exe / @c .o, formatos PE/ELF)
 *       todavia no esta portado a esta funcion; se anyadira sin cambiar la
 *       firma.  Mientras tanto @c compile() con @c ExecMode::AOT devuelve
 *       @c ok=false con un @c message claro.
 */
CompileResponse compile(const CompileRequest &req);

} // namespace tc
} // namespace vesta

#endif // VESTA_TOOLCHAIN_TOOLCHAIN_H
