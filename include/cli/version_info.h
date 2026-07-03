/**
 * @file version_info.h
 * @brief Banner de version de VestaVM para `vesta --version` / `vesta -v`.
 *
 * Muestra nombre + version, fecha/hora del build, hash de git, plataforma,
 * modos de ejecucion disponibles (interprete / JIT / AOT) y las variantes
 * SIMD que soporta la CPU actual (detectadas en runtime via cpuid).
 */
#ifndef VESTAVM_CLI_VERSION_INFO_H
#define VESTAVM_CLI_VERSION_INFO_H

#include <ostream>

namespace cli {

/**
 * @brief Imprime el banner de version en @p os.
 *
 * Usa color ANSI solo si la terminal lo soporta (respeta NO_COLOR y
 * consolas sin VT).  Coste nulo fuera del path de --version.
 *
 * @param os Stream de salida (normalmente stdout).
 */
void print_version_banner(std::ostream &os);

} // namespace cli

#endif // VESTAVM_CLI_VERSION_INFO_H
