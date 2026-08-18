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
 * @file jit/sched/instr_cost.h
 * @brief InstrCost + ExecKind: el DATO abstracto de coste de una operacion
 *        (latencia + throughput + puertos + uops), SIN dependencia del backend.
 *
 * Separado de @c cost_model.h a proposito: @c cost_model.h declara la INTERFAZ
 * @c SchedCostModel::cost(const MInstr&), que conoce el MachineIR (ISA).  El
 * DATO que devuelve (@c InstrCost) es en cambio ABSTRACTO -- solo numeros y una
 * familia de ejecucion -- y no menciona ninguna instruccion concreta.
 *
 *      SchedCostModel::cost(MInstr)  --produce-->  InstrCost   (este header)
 *          (conoce la ISA)                          (abstracto, ISA-neutral)
 *
 * Gracias a esta separacion, un consumidor ISA-NEUTRAL (el modelo de asignacion
 * de recursos en @c codegen/rbank, los Facts de hardware) puede leer @c
 * InstrCost sin arrastrar @c machine_ir.h.  El UNICO que traduce MInstr ->
 * InstrCost es el backend (la implementacion del @c SchedCostModel).
 */

#ifndef VESTA_JIT_SCHED_INSTR_COST_H
#define VESTA_JIT_SCHED_INSTR_COST_H

#include <cstdint>

namespace jit {
namespace sched {

/**
 * @brief Familia de ejecucion de una operacion (grupo de puertos idealizado).
 *
 * El modelo de recursos del scheduler reparte las instrucciones "listas" entre
 * estas familias respetando el ancho de emision y los puertos disponibles del
 * core, para modelar la ejecucion superescalar/paralela.
 */
enum class ExecKind : uint8_t {
    ALU = 0, ///< aritmetica/logica entera simple (add/sub/and/mov/cmp...).
    MUL,     ///< multiplicacion entera.
    DIV,     ///< division entera (no totalmente pipelined).
    LOAD,    ///< lectura de memoria.
    STORE,   ///< escritura de memoria.
    BRANCH,  ///< salto/condicional.
    FP_ADD,  ///< suma/mov/convert de coma flotante.
    FP_MUL,  ///< multiplicacion de coma flotante.
    FP_DIV,  ///< division/raiz de coma flotante.
    OTHER,   ///< pseudo-ops / barreras (call/ret) / sin coste.
    COUNT
};

/// Numero maximo de grupos de puertos que una instruccion puede usar.
constexpr int kMaxSchedPorts = 8;

/// Uso de UN grupo de puertos por una instruccion (para el modelo de recursos).
/// @c port indexa el legado de puertos de la microarquitectura
/// (0..port_count-1);
/// @c uops = uops que la instruccion despacha a ese grupo.
struct SchedPortUse {
    uint8_t port = 0;
    float uops = 1.0f;
};

/**
 * @brief Coste de una instruccion para el scheduler (latencia + throughput +
 *        PUERTOS de ejecucion).  DATO abstracto: no menciona ninguna ISA.
 *
 * CONTRATO kind <-> ports (coherencia obligatoria).  @c kind y @c ports pueden
 * expresar parcialmente la misma idea (un LOAD suele ir a los puertos de
 * memoria).  Para que nadie los rellene de forma inconsistente (p.ej.
 * @c kind=LOAD con @c ports={ALU}), la regla es:
 *
 *   - @c kind es SIEMPRE clasificatorio (la familia; nunca miente).
 *   - Si @c nports>0, @c ports describe el comportamiento EXACTO de puertos y
 *     DEBE ser coherente con @c kind (los puertos de un LOAD son de memoria).
 *   - Si @c nports==0, no hay detalle de puertos: el consumidor los DERIVA de
 *     @c kind (el modelo generico siempre rellena @c ports; una uarch parcial
 *     puede dejar @c nports==0 y confiar en @c kind).
 *
 * EXTENSION FUTURA (sin romper la interfaz): este es el sitio correcto para
 * anadir dimensiones de coste del hardware -- @c energy, @c register_pressure,
 * @c cache_pressure, @c memory_level, @c fusion_class, @c speculation_penalty
 * -- segun aparezcan consumidores.  Ninguna rompe a los actuales (campos nuevos
 * con default neutro).
 */
struct InstrCost {
    float latency = 1.0f;  ///< ciclos desde emision hasta que el resultado
                           ///< esta disponible para un dependiente.
    float recip_tp = 1.0f; ///< throughput reciproco (ciclos entre dos emisiones
                           ///< back-to-back de la misma clase; 1/IPC).
    ExecKind kind = ExecKind::ALU; ///< familia de ejecucion (grupo de puertos).
    bool is_barrier = false; ///< CALL/RET/SAFEPOINT: no se reordena a traves.
    float uops = 1.0f;       ///< uops totales (limite de emision del core).
    /// Grupos de puertos que la instruccion ocupa (del modelo de la microarq o
    /// sintetizados de @c kind).  El scheduler lleva ocupacion por grupo y
    /// evita programar en el mismo ciclo dos instrucciones que compiten por el
    /// mismo puerto -> modela la contencion superescalar real.  @c nports==0 =
    /// derivar de @c kind (el modelo generico siempre rellena esto).
    SchedPortUse ports[kMaxSchedPorts];
    uint8_t nports = 0;
};

} // namespace sched
} // namespace jit

#endif // VESTA_JIT_SCHED_INSTR_COST_H
