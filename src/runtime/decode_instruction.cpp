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
#include "runtime/dispatch_table.h"
#include "runtime/runtime.h"

namespace runtime {
    using clock = std::chrono::high_resolution_clock;

    void decode_instr_reg(VM *vm, DecodedInstr &instr) {
        // suponiendo que sea un add, mov, sub, div, mul u otro
        // del estilo, se puede usar este modo de descodificacion.

        // el offset del resto de datos empieza apartir del opcode,
        // calculamos el offset al resto de datos.
        uint8_t offset = vm->rip.ptr_vm.raw + ((instr.is_extended != 0) ? 1 : 2);

        // leemos los dos bytes que ocupa la instrucciones de este tipo
        uint16_t data = vm->vm_mem.read_u16(offset);

        uint8_t n1 = static_cast<uint8_t>(data & 0x00FF);
        uint8_t n2 = static_cast<uint8_t>((data & 0xFF00) >> 8);

        // 0b`mode`0d0000 -> modo ocupa el los primeros 2 bits
        instr.mode = (n1 >> 6) & 0b11;

        // los dos registros se codifica en el mismo byte (en el cuarto normalmente), el modo en el tercero
        instr.data_instruction.reg_data.reg1 = static_cast<uint8_t>(n2 & 0xF);
        instr.data_instruction.reg_data.reg2 = static_cast<uint8_t>(n2 >> 4);
    }


    void VM::decode_instruction() {
        vm_hook(this, DebugStage::DecodeBegin);

        PROFILE_START

        uint64_t pc = rip.ptr_vm.raw;
        uint32_t idx = icache_index(pc);

        // -------------------------------------------------------------------------------------------------------

        // HIT en caché, si ya se descodifico alguna vez, se devuelve su resultado.
        // Esto funcionara siempre y cuando las isntrucciones no se modifiquen en run time.
        // si la instruccion se modidica en tiempo de ejecuccion, la cache no se vera actualizada
        // de forma automatica por lo que para la VM puede aparentar que la instruccion nunca cambio
        // aunque a nivel de memoria lo alla hecho, en la cache no lo parecera.
        DecodedInstr *cached = &icache[idx];
        if (cached->pc == pc && decoded_ptr != nullptr) {
            // si ya se descodifico alguna vez una instruccion, se puede usar
            // cache, pero sino hay que realizar una descodificacion por primera vez

            decoded_ptr = cached; // NO COPIA, SOLO APUNTA
            vm_hook(this, DebugStage::DecodeEnd);
            return;
        }
        /**
         * Prefetch de la siguiente instrucción
         * __builtin_prefetch(addr, rw, locality)
         *      - rw = 0 -> lectura
         *      - rw = 1 -> escritura
         *      - locality = 0 -> no lo voy a usar mucho l1
         *      - locality = 1 -> lo usaré pronto l2
         *      - locality = 3 -> mantenlo en caché el máximo tiempo l3
         */
        __builtin_prefetch(&vm_mem[pc + 16], 0, 1);

        DecodedInstr decode_tmp{}; // decodificamos en un temporal
        decode_tmp.pc = pc;
        decode_tmp.is_extended = vm_mem[pc];

        if (decode_tmp.is_extended == 0x00) {
            decode_tmp.opcode_index = vm_mem[pc + 1];
        }

        // -------------------------------------------------------------------------------------------------------
        // MISS -> decodificar desde cero

        InstrFormat *table = decode_table_primary;
        // decoded ya tiene los dos opcodes. leeidos
        uint8_t index = decode_tmp.is_extended;

        // seleccionamos la tabla de opcodes en base a si el primer byte es 0x00 o no,
        // en caso de ser 0x00 se usa la tabla extendida (decode_table_extended).
        if (decode_tmp.is_extended == 0x00) {
            table = decode_table_extended;
            index = decode_tmp.opcode_index;
        }

        // obtenemos los metadatos de la instruccion.
        InstrFormat &metadata = table[index];

        if (
            metadata.mode == Assembly::Bytecode::AddressingMode::COUNT ||
            metadata.exec == nullptr) {
            // realizamos el hook en caso de error
            vm_hook(this, DebugStage::DecodeEnd);
            std::cout << "Error, la instruccion con opcode1(" << vesta::hex64(decode_tmp.is_extended) << "), " <<
                    "opcode2(" << vesta::hex64(decode_tmp.opcode_index) << ") no esta implementada en la VM"
                    << std::endl;
            exit(-1);
        }

        // obtenemos los metadatos de la instruccion.
        decode_tmp.metadata = &metadata;

        // -------------------------------------------------------------------------------------------------------

        // ejecutamos el metodo encarga de descodificar dicha instruccion.
        metadata.decode(this, decode_tmp);

        // Guardar en caché, despues de llamara a decode, muy importante el orden.
        // los metodos de metadata.decode modifican la estructura metadata.decoded
        // que luego copiamos a la cache.

        // guardar en caché
        icache[idx] = decode_tmp;

        // apuntar a la entrada de caché
        decoded_ptr = &icache[idx];

        PROFILE_END("DECODER");

        // realizamos el hook al final de la fase
        vm_hook(this, DebugStage::DecodeEnd);
    }



    vm_event VM::execute_instruction() {
        // realizamos el hook antes de la ejecuccion
        vm_hook(this, DebugStage::ExecuteBegin);

        PROFILE_START

        // ejecutamos la instruccion descodificada. No hacemos aqui
        // validacion del campo "exec" por que se supone que ya hemos comprobado en la fase de decode
        // que la instruccion tiene un metodo ejecutor. Entonces no queremos realizar mas comprobaciones para
        // no consumir mas ciclos de reloj.
        decoded_ptr->metadata->exec(this, *decoded_ptr);
        //execute_instr(this, *decoded_ptr);

        /**
         * Si la instrucción requiere esperar I/O u otra.
         * Retornamos aqui y no despues de incrementar PC para que el flujo
         * de codigo no avance, ya que hasta que esta instruccion no deje de
         * estar bloqueada, no podemos avanzar el registro PC.
         */
        if (decoded_ptr->blocking) {
            vm_hook(this, DebugStage::ExecuteEnd);
            return EVT_IO_WAIT; // EVT_IO_WAIT
        }
        if (should_kill) {
            vm_hook(this, DebugStage::ExecuteEnd);
            return EVT_ERROR; // matar a la VM
        }

        // Si la instrucción NO modificó el PC, lo avanzamos, esto siempre pasara a no ser que sea un jmp o un call.
        // o una instruccion similar que modifique PC por su cuenta.
        if (!decoded_ptr->did_jump)
            // movemos el puntero de instruccion al final de ejecutar la instruccion
            rip.ptr_vm.raw += decoded_ptr->size;
        // antes de retorna hacemos el hook

        PROFILE_END("EXECUTER");

        vm_hook(this, DebugStage::ExecuteEnd);
        // indicamos que la ejecuccion tuvo exito y no se requiere bloquear la VM.
        return EVT_EXEC_DONE; // si se puede seguir ejecutando instrucciones
    }
}
