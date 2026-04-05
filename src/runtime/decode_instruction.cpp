/*
 * VestaVM - Máquina Virtual Distribuida
 *
 * Copyright © 2026 David López.T (DesmonHak) (Castilla y León, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribución obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

#include "runtime/runtime.h"

namespace runtime {
    void VM::fetch_instruction() {
        vesta::scout() << "fetch_instruction" << std::endl;

        decoded.opcode[0] = vm_mem[rip.ptr_vm.raw];
        //rip.ptr_vm.raw += 1; // avanzar rip 1 posicione despues de lectura de opcode
        if (decoded.opcode[0] != 0) return;

        // si el primer opcode no era 0x00 no hay extension de opcode
        decoded.opcode[1] = vm_mem[rip.ptr_vm.raw+1];
        // avanzar rip 1 posicione despues de lectura de opcode
        //rip.ptr_vm.raw += 1;
    }


    void VM::decode_instruction() {
        vesta::scout() << "decode_instruction" << std::endl;


        uint64_t pc = rip.ptr_vm.raw;
        uint32_t idx = icache_index(pc);

        // HIT en caché, si ya se descodifico alguna vez, se devuelve su resultado.
        // Esto funcionara siempre y cuando las isntrucciones no se modifiquen en run time.
        // si la instruccion se modidica en tiempo de ejecuccion, la cache no se vera actualizada
        // de forma automatica por lo que para la VM puede aparentar que la instruccion nunca cambio
        // aunque a nivel de memoria lo alla hecho, en la cache no lo parecera.
        if (icache_tag[idx] == pc) {
            decoded = icache[idx];
            return;
        }

        // MISS -> decodificar desde cero

        // decoded ya tiene los dos opcodes.d

        DecodedInstr d{};
       /* d.opcode = opcode;
        d.size = decode_table[opcode].size;
        d.exec = decode_table[opcode].exec;

        switch (decode_table[opcode].type) {
            case TYPE_R:
                d.rd = (raw >> 7) & 0x1F;
                d.rs1 = (raw >> 15) & 0x1F;
                d.rs2 = (raw >> 20) & 0x1F;
                break;

            case TYPE_I:
                d.rd = (raw >> 7) & 0x1F;
                d.rs1 = (raw >> 15) & 0x1F;
                d.imm = (int32_t) raw >> 20;
                break;
        }

        // Guardar en caché
        icache[idx] = d;
        icache_tag[idx] = pc;
*/
        // Copiar al decodificado actual
        decoded = d;

        _sleep(1000);
    }
}
