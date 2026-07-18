/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 */

/**
 * @file jit/sched/machine_effects.h
 * @brief Efectos COMPLETOS de una instruccion MachineIR (para el DAG de
 *        dependencias del scheduler).  Parte del analisis de efectos del ASA.
 *
 * El scheduler solo puede REORDENAR instrucciones que no dependan entre si.
 * Para eso necesita el conjunto EXACTO de lo que cada instruccion lee y
 * escribe -- incluyendo los efectos IMPLICITOS, que son la fuente de casi
 * todos los bugs de un scheduler ingenuo:
 *
 *   - registros implicitos: @c IDIV / @c DIV_U leen y escriben RDX:RAX;
 *     @c CQO lee RAX y escribe RDX; los shifts por CL leen RCX; etc.
 *   - FLAGS (RFLAGS): @c CMP / @c ADD / ... los ESCRIBEN; @c JCC / @c SETCC /
 *     @c CMOVCC / @c ADC / @c SBB los LEEN.  Un flag-consumer no puede subir por
 *     encima de su productor, ni meterse un clobber de flags entre ambos.
 *   - MEMORIA: los accesos a memoria se mantienen ORDENADOS de forma
 *     conservadora (sin alias analysis): un load no adelanta a un store previo,
 *     dos stores no se reordenan.
 *   - BARRERAS: @c CALL / @c RET / @c SAFEPOINT no se cruzan.
 *
 * Esto es exactamente el analisis de efectos del ASA (ver @c vx/asm/asm_effects
 * para el nivel de asm-fuente); aqui se expresa sobre el @c MInstr del back-end.
 */

#ifndef VESTA_JIT_SCHED_MACHINE_EFFECTS_H
#define VESTA_JIT_SCHED_MACHINE_EFFECTS_H

#include "jit/machine_ir.h"
#include "vx/asm/instr_db.h"

#include <cstdint>
#include <vector>

namespace jit {
namespace sched {

/// ISA sobre la que se calculan los efectos (elige la DB: x86 / arm64).
using EffIsa = vx::instr_db::Isa;

/**
 * @brief Efectos de una @c MInstr: que registros lee/escribe (explicitos +
 *        implicitos), si toca flags y memoria, y si es una barrera.
 *
 * Los identificadores de registro usan un espacio UNIFORME:
 *   - fisicos (post-regalloc): el id de @c MReg (0..63).
 *   - virtuales (pre-regalloc): @c VREG_BASE + vreg_id.
 * Asi un mismo dependency-DAG sirve antes o despues del register allocator.
 */
struct MEffects {
    static constexpr uint32_t VREG_BASE = 1u << 20; ///< separa vregs de fisicos.

    std::vector<uint32_t> reads;  ///< registros leidos (explicitos + implicitos)
    std::vector<uint32_t> writes; ///< registros escritos (explicitos+implicitos)
    bool reads_flags = false;     ///< lee RFLAGS (Jcc/SETcc/CMOVcc/ADC/SBB)
    bool writes_flags = false;    ///< escribe RFLAGS (ALU/CMP/TEST/...)
    bool reads_mem = false;       ///< lee memoria
    bool writes_mem = false;      ///< escribe memoria
    bool is_barrier = false;      ///< CALL/RET/SAFEPOINT: no reordenar a traves
};

/**
 * @brief Calcula los efectos completos de @p mi consultando las DBs generadas.
 *
 * Los efectos de las instrucciones REALES de la ISA (add/mov/idiv/addsd/...) se
 * leen de @c vx::instr_db (rmask/wmask/memflags de la forma) + los registros
 * implicitos con nombre de @c vx::asm (implicit_write).  NO se re-derivan a mano:
 * la DB es la fuente de verdad.  Solo los PSEUDOS propios de VestaVM (LOAD_VM,
 * STORE_VM, DIVMOD_V, ALLOCA, SAFEPOINT, ARG, TAILCALL, ...), que no existen en
 * ninguna ISA, se modelan aqui explicitamente.
 *
 * @param mi   instruccion maquina.
 * @param isa  ISA de la DB a consultar (por defecto x86).
 */
MEffects machine_effects(const MInstr &mi, EffIsa isa = EffIsa::X86);

/**
 * @brief Mnemonico de la ISA para un @c MOp REAL (o @c nullptr si es un pseudo
 *        de VestaVM sin instruccion equivalente).  Solo el NOMBRE: los efectos
 *        salen de la DB, no de aqui.  @p isa selecciona x86 vs arm64.
 */
const char *mop_mnemonic(MOp op, EffIsa isa);

} // namespace sched
} // namespace jit

#endif // VESTA_JIT_SCHED_MACHINE_EFFECTS_H
