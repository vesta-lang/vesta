/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file exec_instruction_async.cpp
 * @brief Implementacion de las instrucciones de async/await de VestaVM.
 *
 * Implementa un modelo de concurrencia basado en promesas:
 *
 *  FUTURE  (0x00 0x29): crea un FutureObject en el GC (estado PENDING).
 *                        R0 = GcHandle del future.
 *
 *  AWAIT   (0x00 0x2A): suspende el proceso hasta que el future sea resuelto.
 *                        Si ya esta resuelto, escribe el resultado en R0 y continua.
 *                        Si sigue PENDING, registra el PID y bloquea (blocking=true);
 *                        cuando FULFILL/REJECT lo despierte, re-ejecuta AWAIT que
 *                        encuentra el estado resuelto y retorna el valor.
 *
 *  FULFILL (0x00 0x2B): resuelve el future con un valor (RESOLVED).
 *                        Si hay un proceso esperando (waiter_pid != 0) lo despierta.
 *
 *  REJECT  (0x00 0x2C): rechaza el future con un codigo de error (REJECTED).
 *                        Si hay un proceso esperando (waiter_pid != 0) lo despierta.
 */

#include "runtime/exec_instruction.h"
#include "runtime/proceso_runtime.h"
#include "runtime/scheduler.h"
#include "runtime/runtime.h"
#include "gc/gc_heap.h"
#include "loader/oop_types.h"

namespace runtime {

    // =========================================================================
    //  0x29  FUTURE  (sin operandos)
    // =========================================================================

    /**
     * @brief Ejecuta FUTURE: aloca un FutureObject GC en estado PENDING.
     *
     * El GcHandle devuelto en R0 debe pasarse a AWAIT para esperar el resultado
     * o a FULFILL/REJECT para resolverlo desde otro proceso.
     *
     * @param vm    Proceso virtual que ejecuta FUTURE.
     * @param instr Sin operandos (FIXED_2).
     */
    void exec_instr_future(ProcessVM *vm, const DecodedInstr &instr) {
        (void)instr; // sin operandos

        // alocar el FutureObject en el heap GC
        gc::GcHandle h = vm->gc_heap.alloc(sizeof(loader::FutureObject));
        if (h == gc::GC_NULL_HANDLE) {
            vm->registers.regs[0].qword(static_cast<uint64_t>(gc::GC_NULL_HANDLE));
            return; // sin memoria: el llamante debe comprobar el handle
        }

        uint8_t *payload = vm->gc_heap.deref(h);
        if (payload == nullptr) {
            vm->registers.regs[0].qword(static_cast<uint64_t>(gc::GC_NULL_HANDLE));
            return;
        }

        // inicializar el FutureObject en estado PENDING
        auto *fut             = reinterpret_cast<loader::FutureObject *>(payload);
        fut->header.flags     = loader::OBJ_FLAG_GC_OWNED; // gestionado por GC
        fut->header.hash_code = static_cast<uint32_t>(h);  // identidad = handle
        fut->header.class_ptr = nullptr;                    // sin ClassInfo asignado
        fut->state            = loader::FutureState::PENDING;
        fut->result           = 0;
        fut->waiter_pid       = 0; // ningún proceso esperando aun

        vm->registers.regs[0].qword(static_cast<uint64_t>(h)); // R0 = GcHandle
    }

    // =========================================================================
    //  0x2A  AWAIT  r_fut
    // =========================================================================

    /**
     * @brief Ejecuta AWAIT: suspende el proceso hasta que el future se resuelva.
     *
     * Primera ejecucion (state == PENDING):
     *   Registra el PID del proceso llamante en waiter_pid y activa blocking=true.
     *   El PC no avanza; AWAIT sera re-ejecutado cuando FULFILL/REJECT lo despierte.
     *
     * Re-ejecucion (state != PENDING):
     *   Escribe fut->result en R0 y retorna normalmente.
     *
     * @param vm    Proceso virtual que ejecuta AWAIT.
     * @param instr reg1 = GcHandle del FutureObject.
     */
    void exec_instr_await(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  r_fut = instr.data_instruction.reg_data.reg1;
        const uint32_t h     = static_cast<uint32_t>(vm->registers.regs[r_fut].qword());

        uint8_t *payload = vm->gc_heap.deref(h);
        if (payload == nullptr) {
            vm->registers.regs[0].qword(0); // handle invalido: retornar 0
            return;
        }

        auto *fut = reinterpret_cast<loader::FutureObject *>(payload);

        if (fut->state == loader::FutureState::PENDING) {
            // future aun no resuelto: suspender el proceso
            // codificar el PID del proceso actual: scheduler_id<<32 | local_pid
            uint64_t my_pid = (static_cast<uint64_t>(vm->pid.scheduler_id) << 32)
                            | static_cast<uint64_t>(vm->pid.local_pid);
            fut->waiter_pid = my_pid; // registrar quien espera

            // bloquear el proceso sin avanzar el PC (AWAIT sera re-ejecutado)
            vm->decoded_ptr->flags_info.blocking = true;
            vm->scheduler.on_event(EVT_IO_WAIT); // transicion a WAIT_IO
            return;
        }

        // future ya resuelto: escribir resultado en R0 y continuar
        vm->registers.regs[0].qword(fut->result);
    }

    // =========================================================================
    //  0x2B  FULFILL  r_fut, r_val
    // =========================================================================

    /**
     * @brief Ejecuta FULFILL: resuelve un future con un valor.
     *
     * Escribe state=RESOLVED y result=r_val en el FutureObject.
     * Si habia un proceso esperando (waiter_pid != 0) llama a make_ready() para
     * despertarlo y que re-ejecute su AWAIT.
     *
     * @param vm    Proceso virtual que ejecuta FULFILL.
     * @param instr reg1 = GcHandle del FutureObject, reg2 = valor de resolucion.
     */
    void exec_instr_fulfill(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  r_fut = instr.data_instruction.reg_data.reg1; // handle del future
        const uint8_t  r_val = instr.data_instruction.reg_data.reg2; // valor de resolucion

        const uint32_t h = static_cast<uint32_t>(vm->registers.regs[r_fut].qword());
        uint8_t *payload = vm->gc_heap.deref(h);
        if (payload == nullptr) return; // handle invalido: ignorar

        auto *fut    = reinterpret_cast<loader::FutureObject *>(payload);
        fut->state   = loader::FutureState::RESOLVED;
        fut->result  = vm->registers.regs[r_val].qword(); // almacenar el valor

        if (fut->waiter_pid != 0) {
            // despertar al proceso que esta esperando en AWAIT
            GlobalPID target;
            target.scheduler_id = static_cast<uint32_t>(fut->waiter_pid >> 32);
            target.local_pid    = static_cast<uint64_t>(fut->waiter_pid & 0xFFFFFFFF);
            vm->scheduler.vm_reference.make_ready(target);
            fut->waiter_pid = 0; // limpiar para evitar despertar varias veces
        }
    }

    // =========================================================================
    //  0x2C  REJECT  r_fut, r_err
    // =========================================================================

    /**
     * @brief Ejecuta REJECT: rechaza un future con un codigo de error.
     *
     * Escribe state=REJECTED y result=r_err en el FutureObject.
     * Si habia un proceso esperando (waiter_pid != 0) llama a make_ready() para
     * despertarlo y que obtenga el codigo de error desde su AWAIT.
     *
     * @param vm    Proceso virtual que ejecuta REJECT.
     * @param instr reg1 = GcHandle del FutureObject, reg2 = codigo de error.
     */
    void exec_instr_reject(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  r_fut = instr.data_instruction.reg_data.reg1; // handle del future
        const uint8_t  r_err = instr.data_instruction.reg_data.reg2; // codigo de error

        const uint32_t h = static_cast<uint32_t>(vm->registers.regs[r_fut].qword());
        uint8_t *payload = vm->gc_heap.deref(h);
        if (payload == nullptr) return; // handle invalido: ignorar

        auto *fut    = reinterpret_cast<loader::FutureObject *>(payload);
        fut->state   = loader::FutureState::REJECTED;
        fut->result  = vm->registers.regs[r_err].qword(); // almacenar el codigo de error

        if (fut->waiter_pid != 0) {
            // despertar al proceso que esta esperando en AWAIT
            GlobalPID target;
            target.scheduler_id = static_cast<uint32_t>(fut->waiter_pid >> 32);
            target.local_pid    = static_cast<uint64_t>(fut->waiter_pid & 0xFFFFFFFF);
            vm->scheduler.vm_reference.make_ready(target);
            fut->waiter_pid = 0; // limpiar para evitar despertar varias veces
        }
    }

} // namespace runtime
