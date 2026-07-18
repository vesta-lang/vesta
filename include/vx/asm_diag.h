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
 * @file asm_diag.h
 * @brief Diagnosticos estructurales de un bloque de inline asm derivados de su
 *        CFG (ver vx/asm_cfg.h).
 *
 * Sobre el grafo de flujo reconstruido detecta, de forma SoLIDA (sin falsos
 * positivos: solo avisa cuando la propiedad es cierta por alcanzabilidad):
 *   - CoDIGO MUERTO: instrucciones de un bloque que no es alcanzable desde la
 *     entrada del bloque asm.
 *   - SALTO NO RESUELTO: rama a una etiqueta que no se define en el bloque.
 *   - BUCLE SIN SALIDA: una region alcanzable desde la que no se puede llegar a
 *     ninguna salida (ret o caida al final) -> bucle infinito.
 *
 * NO analiza registros/flags todavia (eso es el dataflow, un paso aparte): estos
 * tres diagnosticos salen solo de la topologia del CFG y por eso son seguros.
 * La cadena de codigos es estable (VXA0xx) para el catalogo de diagnosticos.
 */

#ifndef VX_ASM_DIAG_H
#define VX_ASM_DIAG_H

#include <cstdint>
#include <string>
#include <vector>

#include "vx/asm_cfg.h"
#include "vx/instr_db.h"

namespace vx {

/// Severidad de un diagnostico del asm.
enum class AsmDiagSeverity : uint8_t { Info, Warning, Error };

/// Un diagnostico estructural del bloque de asm.
struct AsmDiag {
    AsmDiagSeverity severity = AsmDiagSeverity::Warning;
    uint32_t line_no = 0;    ///< linea fisica dentro del bloque (1-based).
    std::string code;        ///< codigo estable (p.ej. "VXA001").
    std::string message;     ///< mensaje legible.
};

/**
 * @brief Diagnostica un CFG de asm ya construido.
 * @param cfg CFG (de @ref build_asm_cfg).
 * @return Lista de diagnosticos (vacia si el bloque es estructuralmente limpio).
 */
std::vector<AsmDiag> asm_diagnose_cfg(const AsmCfg &cfg);

/**
 * @brief Atajo: construye el CFG de @p body y lo diagnostica.
 */
std::vector<AsmDiag> asm_diagnose(instr_db::Isa isa, const std::string &body);

} // namespace vx

#endif // VX_ASM_DIAG_H
