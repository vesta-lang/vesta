/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file toolchain/aot_build.h
 * @brief Emisor AOT nativo (PE/ELF) reutilizable, extraido de main.cpp.
 */

#ifndef VESTA_TOOLCHAIN_AOT_BUILD_H
#define VESTA_TOOLCHAIN_AOT_BUILD_H

#include <string>

#include "aot/aot_analyze.h" // aot::Tier
#include "vx/compiler.h"     // vx::CompileResult / vx::CompileOptions

// Declaracion adelantada del tipo de cxxopts para no arrastrar el header
// completo a los consumidores que solo declaren la firma.
namespace cxxopts {
class ParseResult;
}

namespace vesta {
namespace tc {

/**
 * @brief Emite el artefacto AOT nativo del modulo ya compilado por el frontend.
 *
 * Reusa el mismo camino que producia el ejecutable AOT en @c main.cpp: parsea
 * el IR embebido en @p cr, auto-bundlea los runtimes necesarios (excepciones,
 * I/O, colecciones), baja a codigo nativo con el back-end vreg (HOST_LEAF), y
 * escribe el .exe/.o/.so/.bin (PE o ELF, x86-64/x86-32) con el emisor y el
 * linker propios.
 *
 * @param result      Flags de la CLI (AOT: --format/--emit/--aot-arch/
 *                    --float-isa/--no-pie/--bin-base/--sysroot).
 * @param cr          Resultado del frontend (IR embebido + simbolos AOT).
 * @param copts       Opciones de compilacion usadas por el frontend.
 * @param out_prefix  Prefijo del artefacto de salida (@c -o).
 * @param aot_tier    Tier nativo (bare|embed|full).
 * @param aot_freestanding  --freestanding (sin libc).
 * @param aot_no_exceptions --no-exceptions (no auto-bundle del runtime de exc).
 * @param aot_no_io   --no-io (no auto-incluir el runtime de I/O).
 * @param aot_no_mem  --no-mem (no auto-incluir el slab allocator).
 * @param argv0       @c argv[0] del proceso (para localizar la stdlib junto al
 *                    ejecutable).
 * @return @c EXIT_SUCCESS / @c EXIT_FAILURE.
 */
int compile_aot(const cxxopts::ParseResult &result,
                const vx::CompileResult &cr, const vx::CompileOptions &copts,
                std::string out_prefix, aot::Tier aot_tier,
                bool aot_freestanding, bool aot_no_exceptions, bool aot_no_io,
                bool aot_no_mem, const char *argv0);

} // namespace tc
} // namespace vesta

#endif // VESTA_TOOLCHAIN_AOT_BUILD_H
