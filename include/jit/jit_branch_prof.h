/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file jit_branch_prof.h
 * @brief Contadores de branch del CODIGO JIT-NATIVO, indexados por linea
 * fuente.
 *
 * El auto-PGO del JIT necesita datos de branches para re-decidir la
 * if-conversion.  El profiler del interprete (runtime/profile.h) solo ve el
 * codigo interpretado; cuando el JIT compila EAGER, el codigo corre nativo sin
 * pasar por el interp -> el profiler del interp queda ciego.  La solucion que
 * usan los JIT de produccion: el propio codigo JIT-eado incrementa contadores
 * en cada branch.  Aqui esos contadores viven en una tabla FIJA indexada por
 * @c source_line (que el IR ya lleva en cada @c BR_COND), de modo que:
 *
 *   - Es VM-INDEPENDIENTE: no hay que mapear PC->linea con @c debug_info; la
 *     clave ES la linea, conocida en el IR.
 *   - El codigo JIT emite `inc [tabla + (line & mask)*8 + off]` al ENTRAR al
 *     bloque destino (taken/not_taken), donde los flags estan muertos -> sin
 *     hazard con el `jcc` del branch.
 *   - El auto-PGO lee esta tabla directamente por linea (sin round-trip).
 *
 * Es APROXIMADO por diseno: colisiones de @c (line & mask) agregan lineas
 * distintas (raro; el modelo de coste solo necesita el orden de magnitud de
 * P(mispredict)).  Contadores @c uint32 no atomicos: una carrera pierde algun
 * incremento, irrelevante para una heuristica.
 */

#ifndef JIT_BRANCH_PROF_H
#define JIT_BRANCH_PROF_H

#include <cstdint>

namespace jit {

/// @brief Ranura de contadores de un branch (una linea fuente).
struct JitLineCtr {
    uint32_t taken;     ///< veces que la condicion fue verdadera
    uint32_t not_taken; ///< veces que fue falsa
};

/// Numero de ranuras (potencia de 2).  16384 * 8B = 128 KB estaticos.
constexpr uint32_t kJitLineSlots = 1u << 14;
constexpr uint32_t kJitLineMask = kJitLineSlots - 1;

/// Tabla global de contadores por linea (incrementada por el codigo JIT).
extern JitLineCtr g_jit_line_ctrs[kJitLineSlots];

/// @brief Direccion absoluta del campo @c taken (off=0) o @c not_taken (off=4)
///        de la ranura de @p source_line.  La usa el codegen para emitir el
///        `mov reg, imm64(addr)` del incremento.
inline uint64_t jit_line_ctr_addr(uint32_t source_line, bool taken) {
    const uint32_t idx = source_line & kJitLineMask;
    const uint8_t *base =
        reinterpret_cast<const uint8_t *>(&g_jit_line_ctrs[idx]);
    return reinterpret_cast<uint64_t>(base) + (taken ? 0u : 4u);
}

/// @brief True si el codegen debe emitir contadores de branch (auto-PGO on).
///        Decision de COMPILE-time del JIT (no un flag en el codigo emitido).
bool jit_branch_prof_emit_enabled();

/// @brief Flag runtime barato (plano) para el guard inline del fast-path de
///        CALLVIRT: true si el auto-PGO tier-2 esta activo (JIT on + no
///        VESTA_NO_JIT_PGO).  Lo pone @c main al configurar el JIT.  Evita una
///        llamada a jit_branch_prof_emit_enabled() por cada callvirt.
extern bool g_jit_tier2_on;

} // namespace jit

#endif // JIT_BRANCH_PROF_H
