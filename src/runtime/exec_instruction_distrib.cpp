/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 */

/**
 * @file exec_instruction_distrib.cpp
 * @brief Ejecutores de instrucciones de programacion distribuida.
 *
 * Implementa las 4 instrucciones distribuidas del conjunto extendido:
 *
 *   0x3B  rspawn   r_fn, r_node   -> R0 = GcHandle de FutureObject
 *   0x3C  msgsend  r_pid, r_addr, r_len
 *   0x3D  msgrecv  r_buf, r_max   -> R0 = bytes recibidos
 *   0x3E  memsync  r_params
 *
 * Todas las operaciones remotas se delegan al DistRuntime del VM propietario,
 * accesible via vm->scheduler.vm_reference.dist_runtime.
 *
 * Encoding FIXED_4 de las instrucciones:
 *   rspawn:  [0x00][0x3B][ctrl: (mode<<6)][regs: (r_node<<4)|r_fn]
 *   msgsend: [0x00][0x3C][(r_pid<<4)|r_addr][(r_len<<4)]
 *   msgrecv: [0x00][0x3D][ctrl: (mode<<6)][regs: (r_max<<4)|r_buf]
 *   memsync: [0x00][0x3E][ctrl: (mode<<6)][r_params]
 */

#include "runtime/exec_instruction.h"
#include "runtime/proceso_runtime.h"
#include "runtime/scheduler.h"
#include "runtime/runtime.h"
#include "distrib/dist_runtime.h"
#include "distrib/mailbox.h"

namespace runtime {

// =========================================================================
//  0x3B  RSPAWN  r_fn, r_node  ->  R0 = GcHandle del FutureObject
// =========================================================================

/**
 * @brief Ejecuta la instruccion RSPAWN.
 *
 * Crea un proceso en el nodo remoto indicado por r_node y retorna
 * un FutureObject GC cuyo resultado sera el PID remoto del proceso.
 *
 * El proceso se bloquea implicitamente si usa AWAIT sobre el future.
 * Si el nodo no esta conectado o la sesion no esta activa, R0 = 0xFFFFFFFF.
 *
 * @param vm    Proceso que ejecuta la instruccion.
 * @param instr reg1 = r_fn (dir virtual del bytecode), reg2 = r_node (indice en NodeRegistry).
 */
void exec_instr_rspawn(ProcessVM *vm, const DecodedInstr &instr) {
    const uint8_t r_fn   = instr.data_instruction.reg_data.reg1; // registro con fn_addr
    const uint8_t r_node = instr.data_instruction.reg_data.reg2; // registro con node_idx

    const uint64_t fn_addr  = vm->registers.regs[r_fn].qword();
    const uint32_t node_idx = static_cast<uint32_t>(vm->registers.regs[r_node].qword());

    distrib::DistRuntime *dr = vm->scheduler.vm_reference.dist_runtime.get();
    if (!dr) {
        vm->registers.regs[0].qword(0xFFFFFFFF); // R0 = handle invalido
        return;
    }

    uint32_t handle = dr->rspawn(vm, fn_addr, node_idx);
    vm->registers.regs[0].qword(static_cast<uint64_t>(handle)); // R0 = GcHandle del future
}

// =========================================================================
//  0x3C  MSGSEND  r_pid, r_addr, r_len
// =========================================================================

/**
 * @brief Ejecuta la instruccion MSGSEND.
 *
 * Envia un mensaje al proceso indicado por r_pid.
 * El PID puede ser local (bit 63 = 0) o remoto (bit 63 = 1).
 * El mensaje se copia desde la memoria VM del proceso emisor.
 *
 * Para PIDs remotos, el envio es at-least-once: se almacena hasta recibir ACK.
 *
 * Encoding de r_pid:
 *   - Local:  bits 31-0 = local_pid (usa scheduler_id=0 para busqueda global)
 *   - Remoto: bit63=1, bits 62-32 = node_idx, bits 31-0 = remote_local_pid
 *             Construir con vdp_make_remote_pid(node_idx, local_pid) desde bytecode.
 *
 * @param vm    Proceso emisor.
 * @param instr mem_data.reg_base=r_pid, mem_data.reg_index=r_addr, mem_data.reg_final=r_len.
 */
void exec_instr_msgsend(ProcessVM *vm, const DecodedInstr &instr) {
    const uint8_t r_pid  = instr.data_instruction.mem_data.reg_base;  // PID del destino
    const uint8_t r_addr = instr.data_instruction.mem_data.reg_index; // dir del buffer
    const uint8_t r_len  = instr.data_instruction.mem_data.reg_final; // longitud en bytes

    const uint64_t target_pid = vm->registers.regs[r_pid].qword();
    const uint64_t vm_addr    = vm->registers.regs[r_addr].qword();
    const uint64_t len        = vm->registers.regs[r_len].qword();

    distrib::DistRuntime *dr = vm->scheduler.vm_reference.dist_runtime.get();
    if (!dr) {
        vm->registers.regs[0].qword(0); // R0 = 0 si no hay runtime distribuido
        return;
    }

    bool ok = dr->msgsend(vm, target_pid, vm_addr, len);
    vm->registers.regs[0].qword(ok ? 1 : 0); // R0 = 1 si enviado/encolado, 0 si error
}

// =========================================================================
//  0x3D  MSGRECV  r_buf, r_max_len  ->  R0 = bytes recibidos
// =========================================================================

/**
 * @brief Ejecuta la instruccion MSGRECV.
 *
 * Extrae el primer mensaje del buzon del proceso y lo copia al buffer
 * VM apuntado por r_buf (hasta r_max_len bytes).
 *
 * Si el buzon esta vacio: bloquea el proceso (EVT_IO_WAIT) sin avanzar el PC.
 * El proceso se re-ejecutara desde MSGRECV cuando llegue un mensaje.
 * R0 = bytes copiados (0 si el proceso quedo bloqueado).
 *
 * @param vm    Proceso receptor.
 * @param instr reg1 = r_buf (dir del buffer destino), reg2 = r_max_len.
 */
void exec_instr_msgrecv(ProcessVM *vm, const DecodedInstr &instr) {
    const uint8_t r_buf = instr.data_instruction.reg_data.reg1;     // buffer destino
    const uint8_t r_max = instr.data_instruction.reg_data.reg2;     // tamano maximo

    const uint64_t buf_addr = vm->registers.regs[r_buf].qword();
    const uint64_t max_len  = vm->registers.regs[r_max].qword();

    distrib::DistRuntime *dr = vm->scheduler.vm_reference.dist_runtime.get();
    if (!dr) {
        vm->registers.regs[0].qword(0);
        return;
    }

    // intentar obtener el mailbox sin crear uno si no existe
    distrib::Mailbox *mb = dr->get_or_create_mailbox(vm);

    if (mb->empty()) {
        // buzon vacio: bloquear el proceso en WAIT_IO para que sea despertado
        // por msgsend cuando llegue un mensaje. El PC no avanza (blocking = true)
        // hace que MSGRECV se re-ejecute en el proximo quantum de este proceso.
        vm->decoded_ptr->flags_info.blocking = true;
        vm->scheduler.on_event(EVT_IO_WAIT); // transicion a WAIT_IO
        vm->registers.regs[0].qword(0);      // R0 = 0 mientras espera
        return;
    }

    uint64_t bytes = dr->msgrecv(vm, buf_addr, max_len);
    vm->registers.regs[0].qword(bytes); // R0 = bytes copiados al buffer
}

// =========================================================================
//  0x3E  MEMSYNC  r_params
// =========================================================================

/**
 * @brief Ejecuta la instruccion MEMSYNC.
 *
 * Lee la estructura MemsyncParams desde la memoria VM apuntada por r_params,
 * detecta las paginas modificadas de la region indicada y las sincroniza con
 * el nodo remoto usando el protocolo MEMSYNC_HASH/DIFF/DATA/ACK.
 *
 * Algoritmo de deteccion de cambios (3 niveles):
 *   1. Adler-32: filtro rapido por pagina (~1 ns/pagina intacta)
 *   2. BLAKE2s-256: verificacion fuerte (solo para paginas con Adler-32 diferente)
 *   3. Transmision: solo las paginas donde BLAKE2s difiere del baseline
 *
 * En esta implementacion v1 la sincronizacion es siempre asincrona.
 * El future_handle en MemsyncParams (si != 0) se resuelve al completar.
 *
 * Layout de MemsyncParams (leido desde VM memory via r_params):
 *   +0   uint64_t local_addr     dir virtual local de inicio
 *   +8   uint64_t len            tamano de la region en bytes
 *   +16  uint32_t node_idx       indice del nodo destino en NodeRegistry
 *   +20  uint32_t _pad
 *   +24  uint64_t remote_addr    dir en el nodo destino
 *   +32  uint32_t future_handle  GcHandle del future a resolver (0=ignorar)
 *   +36  uint32_t _pad2
 *
 * @param vm    Proceso que ejecuta la instruccion.
 * @param instr reg1 = r_params (dir de MemsyncParams en VM memory).
 */
void exec_instr_memsync(ProcessVM *vm, const DecodedInstr &instr) {
    const uint8_t r_params = instr.data_instruction.reg_data.reg1;
    const uint64_t params_addr = vm->registers.regs[r_params].qword();

    distrib::DistRuntime *dr = vm->scheduler.vm_reference.dist_runtime.get();
    if (!dr) return;

    dr->memsync(vm, params_addr);
    // la instruccion no produce un valor de retorno directo;
    // el future_handle dentro de MemsyncParams se resuelve de forma asincrona
}

} // namespace runtime
