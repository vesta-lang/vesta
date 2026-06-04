/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/regalloc_rewrite.h
 * @brief Rewrite de MachineIR vreg -> MachineIR fisica (Phase D.7, commit 4a).
 *        Ver doc/REGALLOC.md.
 *
 * Toma una @c MFunction en forma de registros virtuales (3-operandos,
 * pre-legalization) y la asignacion del linear-scan, y produce una NUEVA
 * @c MFunction donde todos los operandos son fisicos (REG/MEM/IMM), lista
 * para el @c X86Encoder existente.  Hace:
 *
 *   1. Sustitucion VREG -> registro fisico (si esta en reg) o acceso a su
 *      spill slot @c [rbp+off] (si esta spilled).
 *   2. Two-address legalization: el encoder es de DOS operandos
 *      (@c "add dst, src" -> dst = dst OP src); la forma vreg es de TRES
 *      (@c "add dst, src1, src2" -> dst = src1 OP src2).  Se inserta el
 *      @c "mov dst, src1" y se resuelve la anti-dependencia cuando dst y
 *      src2 caen en el mismo fisico (conmutativo: reordenar; no conmutativo:
 *      usar un scratch).
 *   3. Insercion de load/store de spills (el encoder soporta ALU reg/mem, asi
 *      que un src spilled se lee directo; un dst spilled se materializa en un
 *      scratch y se almacena).
 *   4. Prologue/epilogue: push/pop de los callee-saved usados + reserva del
 *      frame para los spill slots.
 *
 * Cobertura commit 4a: MOV, ALU binario (ADD/SUB/AND/OR/XOR/IMUL), CMP/TEST,
 * RET, y passthrough de control (JMP/JCC/LABEL_DEF/NOP).  Resto: se anyade al
 * migrar el selector (commit 4b/5).  ABI del prologue: funcion HOJA estilo
 * host (la integracion con la VM_ABI -- RBX=ProcessVM*, safepoints -- es el
 * commit 4b).
 */

#ifndef VESTA_JIT_REGALLOC_REWRITE_H
#define VESTA_JIT_REGALLOC_REWRITE_H

#include "jit/linear_scan.h"
#include "jit/machine_ir.h"
#include "jit/target_reginfo.h"

namespace jit {

    /**
     * @brief Reescribe @p vf (forma vreg) a una MFunction fisica.
     *
     * @param vf   Funcion en MachineIR vreg (3-operandos).
     * @param ra   Asignacion del linear-scan (vreg -> reg/slot).
     * @param tri  Descriptor del target (scratch regs, ancho de puntero).
     * @param abi  Convencion del prologue/epilogue (HOST_LEAF o VM).  En VM se
     *             salva/establece RBX = @c ProcessVM* (push rbx + mov rbx,arg0).
     * @param ivs  Intervalos (opcional).  Si != nullptr, el rewrite construye
     *             un @c Stackmap en cada CALL describiendo los slots de los GC
     *             roots vivos a traves (Phase D.7 commit 6).  El resultado va
     *             en @c MFunction::stackmaps y se asocia al CALL via su
     *             @c flags (idx); el encoder rellena el @c pc_offset.
     * @return     Nueva MFunction fisica lista para el encoder.
     */
    MFunction rewrite_to_physical(const MFunction &vf,
                                  const RegAlloc &ra,
                                  const TargetRegInfo &tri,
                                  AbiKind abi = AbiKind::HOST_LEAF,
                                  const IntervalResult *ivs = nullptr);

} // namespace jit

#endif // VESTA_JIT_REGALLOC_REWRITE_H
