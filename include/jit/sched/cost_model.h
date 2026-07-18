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
 * @file jit/sched/cost_model.h
 * @brief Modelo de COSTE (latencia + throughput + puertos) por instruccion
 *        maquina, para el scheduler machine-level (C2.15).
 *
 * MODULAR por diseno: el scheduler consulta esta interfaz y NO sabe de donde
 * salen los numeros.  Dos implementaciones:
 *
 *   - @c GenericCostModel : un core OoO superescalar MODERNO y balanceado (el
 *     punto comun de Skylake/Zen/Cortex-A76/Neoverse).  Es el DEFAULT: siempre
 *     es mejor que ignorar las latencias, y beneficia a la mayoria del software
 *     moderno sin atarse a una microarquitectura concreta.
 *   - @c UarchCostModel : los numeros EXACTOS de una microarquitectura concreta
 *     (@c --cpu cortex-a76 / skylake / ...), leidos de la DB de coste
 *     (@c cost_x86() / @c cost_arm64(), generada desde arch-data).
 *
 * El coste se expresa por @c MOp (familia de operacion) + ancho, que es lo que
 * el scheduler ve en el MachineIR; la implementacion @c UarchCostModel mapea
 * @c MOp -> forma de la ISA -> @c AsmCost de la DB.
 */

#ifndef VESTA_JIT_SCHED_COST_MODEL_H
#define VESTA_JIT_SCHED_COST_MODEL_H

#include "jit/machine_ir.h"

#include <cstdint>
#include <memory>
#include <string>

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
    ALU = 0,    ///< aritmetica/logica entera simple (add/sub/and/mov/cmp...).
    MUL,        ///< multiplicacion entera.
    DIV,        ///< division entera (no totalmente pipelined).
    LOAD,       ///< lectura de memoria.
    STORE,      ///< escritura de memoria.
    BRANCH,     ///< salto/condicional.
    FP_ADD,     ///< suma/mov/convert de coma flotante.
    FP_MUL,     ///< multiplicacion de coma flotante.
    FP_DIV,     ///< division/raiz de coma flotante.
    OTHER,      ///< pseudo-ops / barreras (call/ret) / sin coste.
    COUNT
};

/**
 * @brief Coste de una instruccion para el scheduler.
 */
struct InstrCost {
    float latency = 1.0f;   ///< ciclos desde emision hasta que el resultado
                            ///< esta disponible para un dependiente.
    float recip_tp = 1.0f;  ///< throughput reciproco (ciclos entre dos emisiones
                            ///< back-to-back de la misma clase; 1/IPC).
    ExecKind kind = ExecKind::ALU; ///< familia de ejecucion (puertos).
    bool is_barrier = false; ///< CALL/RET/SAFEPOINT: no se reordena a traves.
};

/**
 * @brief Interfaz del modelo de coste (consumida por el scheduler).
 */
class SchedCostModel {
  public:
    virtual ~SchedCostModel() = default;
    /// Coste de la instruccion @p mi (usa su MOp + anchos + operandos MEM).
    virtual InstrCost cost(const MInstr &mi) const = 0;
    /// Ancho de emision del core (uops/ciclo) -- limite superescalar.
    virtual int issue_width() const = 0;
    /// Nombre legible del modelo (para diagnostico / --cpu).
    virtual const char *name() const = 0;
};

/**
 * @brief Modelo GENERICO: core OoO moderno balanceado (default siempre activo).
 *
 * Latencias/throughputs tipicos comunes a los cores modernos: alu 1, mul 3,
 * div ~20 (no pipelined), load 4, store 1, branch 1, fp add/mul 4, fp div ~12.
 * Emision de 4 uops/ciclo.  No modela una uarch concreta pero captura el orden
 * de magnitud correcto -> el scheduler oculta las latencias largas (load/mul/
 * div/fp) y expone ILP, beneficiando a la mayoria del software moderno.
 */
class GenericCostModel final : public SchedCostModel {
  public:
    InstrCost cost(const MInstr &mi) const override;
    int issue_width() const override { return 4; }
    const char *name() const override { return "generic"; }
};

/// ISA objetivo del modelo de coste (para elegir la tabla de la DB).
enum class SchedIsa : uint8_t { X86_64, X86_32, ARM64 };

/**
 * @brief Crea el modelo de coste para @p isa y la CPU @p cpu.
 *
 * @param isa  ISA del MachineIR que se va a schedular.
 * @param cpu  nombre de microarquitectura (@c --cpu, p.ej. "cortex-a76",
 *             "skylake").  Vacio o desconocido -> @c GenericCostModel (default).
 * @return Un modelo de coste (nunca null; cae al generico si la uarch no existe).
 */
std::unique_ptr<SchedCostModel> make_cost_model(SchedIsa isa,
                                                const std::string &cpu);

/**
 * @brief Clasifica un @c MOp en su familia de ejecucion + coste generico.
 *        Expuesto para que @c UarchCostModel reuse la familia (los puertos) y
 *        solo sobreescriba latencia/tp con los datos exactos de la DB.
 */
InstrCost generic_cost_for(const MInstr &mi);

} // namespace sched
} // namespace jit

#endif // VESTA_JIT_SCHED_COST_MODEL_H
