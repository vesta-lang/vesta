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

#include "runtime/decode_table.h"
#include "runtime/runtime.h"

namespace runtime {
    void decode_instr_reg(VM *vm, DecodedInstr &instr) {
        // suponiendo que sea un add, mov, sub, div, mul u otro
        // del estilo, se puede usar este modo de descodificacion.

        // el offset del resto de datos empieza apartir del opcode,
        // calculamos el offset al resto de datos.
        uint8_t offset = vm->rip.ptr_vm.raw + ((instr.opcode[0] != 0) ? 1 : 2);

        // leemos los dos bytes que ocupa la instrucciones de este tipo
        uint16_t data = vm->vm_mem.read_u16(offset);

        uint8_t n1 = static_cast<uint8_t>(data & 0x00FF);
        uint8_t n2 = static_cast<uint8_t>((data & 0xFF00) >> 8);

        // 0b`mode`0d0000 -> modo ocupa el los primeros 2 bits
        vm->decoded.mode = (n1 >> 6) & 0b11;

        // los dos registros se codifica en el mismo byte (en el cuarto normalmente), el modo en el tercero
        vm->decoded.data_instruction.reg_data.reg1 = static_cast<uint8_t>(n2 & 0xF);
        vm->decoded.data_instruction.reg_data.reg2 = static_cast<uint8_t>((n2 & 0xF0) >> 4);
    }

    void VM::fetch_instruction() {
        vesta::scout() << "fetch_instruction" << std::endl;

        decoded.opcode[0] = vm_mem[rip.ptr_vm.raw];
        //rip.ptr_vm.raw += 1; // avanzar rip 1 posicione despues de lectura de opcode
        if (decoded.opcode[0] != 0) return;

        // si el primer opcode no era 0x00 no hay extension de opcode
        decoded.opcode[1] = vm_mem[rip.ptr_vm.raw + 1];
        // avanzar rip 1 posicione despues de lectura de opcode
        //rip.ptr_vm.raw += 1;
    }


    void VM::decode_instruction() {
        vesta::scout() << "decode_instruction" << std::endl;

        uint64_t pc = rip.ptr_vm.raw;
        uint32_t idx = icache_index(pc);

        // -------------------------------------------------------------------------------------------------------

        // HIT en caché, si ya se descodifico alguna vez, se devuelve su resultado.
        // Esto funcionara siempre y cuando las isntrucciones no se modifiquen en run time.
        // si la instruccion se modidica en tiempo de ejecuccion, la cache no se vera actualizada
        // de forma automatica por lo que para la VM puede aparentar que la instruccion nunca cambio
        // aunque a nivel de memoria lo alla hecho, en la cache no lo parecera.
        if (icache_tag[idx] == pc) {
            decoded = icache[idx];
            return;
        }

        // -------------------------------------------------------------------------------------------------------
        // MISS -> decodificar desde cero

        // decoded ya tiene los dos opcodes. leeidos
        bool extended = decoded.opcode[0] == 0x00;

        // seleccionamos la tabla de opcodes en base a si el primer byte es 0x00 o no,
        // en caso de ser 0x00 se usa la tabla extendida (decode_table_extended).
        uint8_t index = extended
                            ? decoded.opcode[1]
                            : decoded.opcode[0];

        // obtenemos los metadatos de la instruccion.
        InstrFormat &metadata = extended
                                    ? decode_table_extended[index]
                                    : decode_table_primary[index];

        if (
            metadata.mode == Assembly::Bytecode::AddressingMode::COUNT ||
            metadata.exec == nullptr) {
            std::cout << "Error, la instruccion con opcode1(" << vesta::hex64(decoded.opcode[0]) << "), " <<
                    "opcode2(" << vesta::hex64(decoded.opcode[1]) << ") no esta implementada en la VM"
                    << std::endl;
            exit(-1);
        }

        // obtenemos los metadatos de la instruccion.
        decoded.metadata = &metadata;

        // -------------------------------------------------------------------------------------------------------

        // ejecutamos el metodo encarga de descodificar dicha instruccion.
        metadata.decode(this, decoded);

        // Guardar en caché, despues de llamara a decode, muy importante el orden.
        // los metodos de metadata.decode modifican la estructura metadata.decoded
        // que luego copiamos a la cache.
        icache[idx] = decoded;
        icache_tag[idx] = pc;
    }

    vm_event VM::execute_instruction() {
        // ejecutamos la instruccion descodificada. No hacemos aqui
        // validacion del campo "exec" por que se supone que ya hemos comprobado en la fase de decode
        // que la instruccion tiene un metodo ejecutor. Entonces no queremos realizar mas comprobaciones para
        // no consumir mas ciclos de reloj.
        decoded.metadata->exec(this, decoded);

        /**
         * Si la instrucción requiere esperar I/O u otra.
         * Retornamos aqui y no despues de incrementar PC para que el flujo
         * de codigo no avance, ya que hasta que esta instruccion no deje de
         * estar bloqueada, no podemos avanzar el registro PC.
         */
        if (decoded.blocking) {
            return EVT_IO_WAIT; // EVT_IO_WAIT
        }
        if (should_kill) {
            return EVT_ERROR; // matar a la VM
        }

        // Si la instrucción NO modificó el PC, lo avanzamos, esto siempre pasara a no ser que sea un jmp o un call.
        // o una instruccion similar que modifique PC por su cuenta.
        if (!decoded.did_jump)
            // movemos el puntero de instruccion al final de ejecutar la instruccion
            rip.ptr_vm.raw += decoded.size;

        // indicamos que la ejecuccion tuvo exito y no se requiere bloquear la VM.
        return EVT_EXEC_DONE; // si se puede seguir ejecutando instrucciones
    }
}
