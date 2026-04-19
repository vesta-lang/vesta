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

#include "ffi/native_ffi.h"
#include "runtime/decode_table.h"
#include "runtime/dispatch_table.h"
#include "runtime/runtime.h"

/**
 * version inline para obtener el tiempo
 * @return devuelve el tiempo actual, se usa para mediciones.
 */
inline uint64_t now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

namespace runtime {
    using clock = std::chrono::high_resolution_clock;

    void decode_instr_two_op_reg(ProcessVM *vm, DecodedInstr &instr) {
        // las instrucciones de registro usan tamaño constante.
        instr.flags_info.size_instr = Assembly::Bytecode::instr_size(instr.metadata->size);

        // suponiendo que sea un add, mov, sub, div, mul u otro
        // del estilo, se puede usar este modo de descodificacion.

        // el offset del resto de datos empieza apartir del opcode,
        // calculamos el offset al resto de datos.
        uint8_t offset = vm->registers.rip.raw() + ((instr.flags_info.is_not_extended != 0) ? 1 : 2);

        // leemos los dos bytes que ocupa la instrucciones de este tipo
        uint16_t data = vm->vm_mem.read_u16(offset);

        uint8_t n1 = static_cast<uint8_t>(data & 0x00FF);
        uint8_t n2 = static_cast<uint8_t>((data & 0xFF00) >> 8);

        // 0b`mode`0d0000 -> modo ocupa el los primeros 2 bits
        instr.flags_info.mode = (n1 >> 6) & 0b11;

        // los dos registros se codifica en el mismo byte (en el cuarto normalmente), el modo en el tercero
        instr.data_instruction.reg_data.reg1 = static_cast<uint8_t>(n2 & 0xF);
        instr.data_instruction.reg_data.reg2 = static_cast<uint8_t>(n2 >> 4);
    }

    void decode_instr_simple_mov(ProcessVM *vm, DecodedInstr &instr) {
        // los MOV simples ocupan espacio constantes
        instr.flags_info.size_instr = Assembly::Bytecode::instr_size(instr.metadata->size);

        // el offset del resto de datos empieza apartir del opcode,
        // calculamos el offset al resto de datos.
        uint8_t offset = vm->registers.rip.raw() + ((instr.flags_info.is_not_extended != 0) ? 1 : 2);

        // leemos los dos bytes que ocupa la instrucciones de este tipo
        uint16_t data = vm->vm_mem.read_u16(offset);

        uint8_t n1 = static_cast<uint8_t>(data & 0x00FF);
        uint8_t n2 = static_cast<uint8_t>((data & 0xFF00) >> 8);

        // 0b`mode`0d0000 -> modo ocupa el los primeros 2 bits
        instr.flags_info.mode = (n1 >> 6) & 0b11;

        // los dos registros se codifica en el mismo byte (en el cuarto normalmente), el modo en el tercero
        instr.data_instruction.reg_data.reg1 = static_cast<uint8_t>(n2 & 0xF);
        instr.data_instruction.reg_data.reg2 = static_cast<uint8_t>(n2 >> 4);
    }


    void decode_instr_inmed_mov(ProcessVM *vm, DecodedInstr &instr) {
        //instr.flags_info.size_instr =

        // datos del mov que indican la variante.
        uint8_t data = vm->vm_mem[vm->registers.rip.raw() + 2];

        // leemos solo 8 byte de datos apartir del segundo opcode y el campo data.
        // las instrucciones de inmediatos usan longitud variable, para asgurarnos de hacer las menos
        // lecturas posibles leemos 64 bits primeramente de golpe, luego usamos 1, 2, 4, u 8 bytes de los leeidos.
        uint64_t inmed = vm->vm_mem.read_u64(vm->registers.rip.raw() + 3);

        // 0b`mode`0d0000 -> modo ocupa el los primeros 2 bits
        instr.flags_info.mode                  = (data >> 6) & 0b11; // bits 7-6 (mm)
        instr.flags_info._signed_instruct      = (data >> 5) & 0b1;  // bit 5 (s)
        instr.flags_info.direction             = (data >> 4) & 0b1;  // bit 4 (d)
        instr.data_instruction.inmmed_data.reg = data & 0b1111;      // bits 3-0 (rrrr)

        // si la direccion es 1 y el signo 1, el mov usa un registro especial
        // y por tanto siempre se usa 64 bits de inmediato
        if (instr.flags_info.direction == 1 && instr.flags_info._signed_instruct == 1) {
            // guardamos el inmediato de 64 bits:
            instr.data_instruction.inmmed_data.inmmed = inmed;

            // indicamos el tamaño de la instruccion que es 2 bytes de opcode + 1 de datos + 8 de inmediato
            instr.flags_info.size_instr = 2 + 1 + 8;
            instr.flags_info.reg_ext    = true; // indicar que usa un registro extendido

            // un registro especial usa el modo como bits adicional para codificar el registro espcial.
            instr.data_instruction.inmmed_data.reg = instr.flags_info.mode << 4 |
                    instr.data_instruction.inmmed_data.reg;

            return; // debemos salir ya que lo de abajo solo aplica con descodificacion convencional
        }

        /**
         * El tamaño de este tipo de instrucciones es variable ya que depende del modo usado, la cantidad
         * de bytes para el inmediato varia.
         * solo 3 bytes son constantes, los cuales 2 son opcodes y 1 es metadatos de la instruccion,
         * los otrs bytes corresponden a la longitud del inmediato que depende del modo codificado.
         */
        instr.flags_info.size_instr = 3 + Assembly::Bytecode::mode_to_bytes(instr.flags_info.mode);

        switch (instr.flags_info.mode) {
            case 0b00: instr.data_instruction.inmmed_data.inmmed = (uint8_t) inmed; //  8 bits
                break;
            case 0b01: instr.data_instruction.inmmed_data.inmmed = (uint16_t) inmed; // 16 bits
                break;
            case 0b10: instr.data_instruction.inmmed_data.inmmed = (uint32_t) inmed; // 32 bits
                break;
            default: instr.data_instruction.inmmed_data.inmmed = inmed; // 64 bits
                break;
        }
    }

    void decode_instr_simple(ProcessVM *vm, DecodedInstr &instr) {}

    void decode_instr_one_op_reg(ProcessVM *vm, DecodedInstr &instr) {
        VM_ASSERT(
            instr_size(instr.metadata->size) == 2,
            std::string("VM::decode_instr_one_op_reg() Instruccion invalida en RIP[") +
            vesta::hex64(vm->registers.rip.raw()) +
            "] opcode1(" + vesta::hex64(instr.flags_info.is_not_extended) +
            ") opcode2(" + vesta::hex64(instr.flags_info.opcode_index) + ")\n" <<
            "decode_instr_one_op_reg() Error la instruccion encontrada no tiene size 2 sino un size: "
            << instr_size(instr.metadata->size) << "\n"
            //<< vm->to_string(),
            , {
            vesta::scout() << vesta::dump(vm->vm_mem, vm->registers.rip.raw(), 64) << std::endl;
            }
        );
        instr.flags_info.size_instr = Assembly::Bytecode::instr_size(instr.metadata->size);

        // leemos solo 1 byte de datos pues se supone que la instruccion a
        // descodificar es de longitud 2.
        uint8_t data = vm->vm_mem[vm->registers.rip.raw() + 1];

        // 0b00`mode`0000 -> modo ocupa el los primeros 2 bits
        instr.flags_info.mode = (data >> 4) & 0b11;

        // el septimo bit indica si es INC o DIC
        // 0b0000 0000 -> INC
        // 0b0100 0000 -> DEC
        instr.flags_info._signed_instruct = (data >> 6) & 0b1;

        // 0b00`mode`reg -> registro ocupa los ultimos 4 bits
        instr.data_instruction.reg_data.reg1 = static_cast<uint8_t>(data & 0xF);
    }

    void decode_instr_push_pop(ProcessVM *vm, DecodedInstr &instr) {
        VM_ASSERT(
            instr_size(instr.metadata->size) == 2,
            std::string("VM::decode_instr_push_pop() Instruccion invalida en RIP[") +
            vesta::hex64(vm->registers.rip.raw()) +
            "] opcode1(" + vesta::hex64(instr.flags_info.is_not_extended) +
            ") opcode2(" + vesta::hex64(instr.flags_info.opcode_index) + ")\n" <<
            "decode_instr_push_pop() Error la instruccion encontrada no tiene size 2 sino un size: "
            << instr_size(instr.metadata->size) << "\n"
            //<< vm->to_string(),
            , {
            vesta::scout() << vesta::dump(vm->vm_mem, vm->registers.rip.raw(), 64) << std::endl;
            }
        );
        instr.flags_info.size_instr = Assembly::Bytecode::instr_size(instr.metadata->size);

        // leemos solo 1 byte de datos pues se supone que la instruccion a
        // descodificar es de longitud 2.
        uint8_t data = vm->vm_mem[vm->registers.rip.raw() + 1];

        // bit 7 -> registro extendido (especial)
        uint8_t reg_ext          = (data >> 6) & 0b1;
        instr.flags_info.reg_ext = reg_ext;

        if (reg_ext == 1) {
            //   REGISTRO ESPECIAL (6 bits)
            uint8_t reg_code                     = data & 0b00111111; // bits 0..5
            instr.data_instruction.reg_data.reg1 = reg_code;

            // modo no aplica
            instr.flags_info.mode = 0;
        } else {
            //   REGISTRO GENERAL (mode + reg) (6 bits)
            uint8_t mode     = (data >> 4) & 0b11; // bits 4..5
            uint8_t reg_code = data & 0b1111;      // bits 0..3

            instr.flags_info.mode                = mode;
            instr.data_instruction.reg_data.reg1 = reg_code;
        }

        instr.flags_info.size_instr = 2;
    }

    void decode_instr_inmed_reg(ProcessVM *vm, DecodedInstr &instr) {
        VM_ASSERT(
            instr.flags_info.is_not_extended == false,
            std::string("VM::decode_instr_inmed_reg() Instruccion invalida en RIP[") +
            vesta::hex64(vm->registers.rip.raw()) +
            "] opcode1(" + vesta::hex64(instr.flags_info.is_not_extended) +
            ") opcode2(" + vesta::hex64(instr.flags_info.opcode_index) + ")\n" <<
            "decode_instr_inmed_reg() instr.is_not_extended == false " <<
            "Error la instruccion encontrada deberia usar extension de signo pero no lo hace por algun motivo: "
            << std::to_string(instr.flags_info.is_not_extended) << "\n"
            //<< vm->to_string(),
            , {
            vesta::scout() << vesta::dump(vm->vm_mem, vm->registers.rip.raw(), 64) << std::endl;
            }
        );

        /**
         * data contiene los campos encodeados:
         * 0b mm s d rrrr
         *    │  │ │ └─── reg (4 bits)
         *    │  │ └───── d (1 bit)
         *    │  └─────── s (1 bit)
         *    └────────── mode (2 bits)
         */
        uint8_t data = vm->vm_mem[vm->registers.rip.raw() + 2];

        // leemos solo 8 byte de datos apartir del segundo opcode y el campo data.
        // las instrucciones de inmediatos usan longitud variable, para asgurarnos de hacer las menos
        // lecturas posibles leemos 64 bits primeramente de golpe, luego usamos 1, 2, 4, u 8 bytes de los leeidos.
        uint64_t inmed = vm->vm_mem.read_u64(vm->registers.rip.raw() + 3);

        /**
         * Aunque las instrucciones de inmediatos no tiene direccionalidad, el campo
         * direccion permite indicar en este caso si el valor inmediato debe operar a un registro:
         *      adds reg, 0x1000    -> direccion = 0
         * o si por el contrario el valor inmediato debe operar el valor contenido en la memoria señala por
         * el registro:
         *      adds [reg], 0x1000  -> direccion = 1
         */
        instr.flags_info.mode                  = (data >> 6) & 0b11; // bits 7-6 (mm)
        instr.flags_info._signed_instruct      = (data >> 5) & 0b1;  // bit 5 (s)
        instr.flags_info.direction             = (data >> 4) & 0b1;  // bit 4 (d)
        instr.data_instruction.inmmed_data.reg = data & 0b1111;      // bits 3-0 (rrrr)

        /**
         * El tamaño de este tipo de instrucciones es variable ya que depende del modo usado, la cantidad
         * de bytes para el inmediato varia.
         * solo 3 bytes son constantes, los cuales 2 son opcodes y 1 es metadatos de la instruccion,
         * los otrs bytes corresponden a la longitud del inmediato que depende del modo codificado.
         */
        instr.flags_info.size_instr = 3 + Assembly::Bytecode::mode_to_bytes(instr.flags_info.mode);

        switch (instr.flags_info.mode) {
            case 0b00: instr.data_instruction.inmmed_data.inmmed = (uint8_t) inmed; //  8 bits
                break;
            case 0b01: instr.data_instruction.inmmed_data.inmmed = (uint16_t) inmed; // 16 bits
                break;
            case 0b10: instr.data_instruction.inmmed_data.inmmed = (uint32_t) inmed; // 32 bits
                break;
            default: instr.data_instruction.inmmed_data.inmmed = inmed; // 64 bits
                break;
        }
    }

    void decode_instr_xchg(ProcessVM *vm, DecodedInstr &instr) {
        instr.flags_info.size_instr = Assembly::Bytecode::instr_size(instr.metadata->size);

        // sumamos 2 por que saltamos el opcode y el byte de flags que aun no
        // tiene ningun uso
        uint16_t data = vm->vm_mem.read_u16(vm->registers.rip.raw() + 2);

        auto byte1 = static_cast<uint8_t>(data & 0xFF);
        auto byte2 = static_cast<uint8_t>(data >> 8);

        instr.data_instruction.regs_data_extent.reg1       = byte1 & 0b11'1111;
        instr.data_instruction.regs_data_extent.reg1_flags = byte1 >> 6 & 0b1;
        instr.data_instruction.regs_data_extent.reg2       = byte2 & 0b11'1111;
        instr.data_instruction.regs_data_extent.reg2_flags = byte2 >> 6 & 0b1;
    }

    void decode_instr_calln(ProcessVM *vm, DecodedInstr &instr) {
        instr.data_instruction.inmmed_data.inmmed = vm->vm_mem.read_u64(vm->registers.rip.raw() + 2);

        // tamaño de la instruccion para luego incrementar rip
        instr.flags_info.size_instr = Assembly::Bytecode::instr_size(instr.metadata->size);

        // argc se cachea en decode para que en exec ambos (fn y argc) vengan del
        // ICACHE - constantes por PC de instruccion - lo que permite al IBP de la CPU
        // predecir correctamente el switch(argc) tras la primera ejecucion.
        instr.data_instruction.inmmed_data.reg = static_cast<uint8_t>(vm->registers.regs[R15].qword());
    }



    void decode_instruction(ProcessVM *process) {
        vm_hook(process, DebugStage::DecodeBegin);
        PROFILE_START
        const uint64_t t1 = now_ns();

        uint64_t pc  = process->registers.rip.raw();
        uint32_t idx = icache_index(pc);

        // -------------------------------------------------------------------------------------------------------

        // HIT en caché, si ya se descodifico alguna vez, se devuelve su resultado.
        // Esto funcionara siempre y cuando las isntrucciones no se modifiquen en run time.
        // si la instruccion se modidica en tiempo de ejecuccion, la cache no se vera actualizada
        // de forma automatica por lo que para la VM puede aparentar que la instruccion nunca cambio
        // aunque a nivel de memoria lo alla hecho, en la cache no lo parecera.
        DecodedInstr *cached = &process->icache[idx];
        if (cached->pc == pc && process->decoded_ptr != nullptr) {
            // si ya se descodifico alguna vez una instruccion, se puede usar
            // cache, pero sino hay que realizar una descodificacion por primera vez

            process->decoded_ptr = cached; // NO COPIA, SOLO APUNTA

            const uint64_t t2 = now_ns();
            process->scheduler.time_decode += (t2 - t1);

            PROFILE_END("DECODER");
            // realizamos el hook al final de la fase
            vm_hook(process, DebugStage::DecodeEnd);
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
        __builtin_prefetch(&process->vm_mem[pc + 16], 0, 1);

        DecodedInstr decode_tmp{}; // decodificamos en un temporal
        decode_tmp.pc                         = pc;
        decode_tmp.flags_info.is_not_extended = process->vm_mem[pc];

        if (decode_tmp.flags_info.is_not_extended == 0x00) {
            decode_tmp.flags_info.opcode_index = process->vm_mem[pc + 1];
        }

        // -------------------------------------------------------------------------------------------------------
        // MISS -> decodificar desde cero

        InstrFormat *table = decode_table_primary;
        // decoded ya tiene los dos opcodes. leeidos
        uint8_t index = decode_tmp.flags_info.is_not_extended;

        // seleccionamos la tabla de opcodes en base a si el primer byte es 0x00 o no,
        // en caso de ser 0x00 se usa la tabla extendida (decode_table_extended).
        if (decode_tmp.flags_info.is_not_extended == 0x00) {
            table = decode_table_extended;
            index = decode_tmp.flags_info.opcode_index;
        }

        // obtenemos los metadatos de la instruccion.
        InstrFormat &metadata = table[index];

        // Validación de instrucción (solo en modo debug)
        VM_ASSERT(
            metadata.mode < Assembly::Bytecode::AddressingMode::COUNT &&
            metadata.exec != nullptr && metadata.decode != nullptr,

            std::string("VM::decode_instruction() Instruccion invalida en RIP[") +
            vesta::hex64(process->registers.rip.raw()) +
            "] opcode1(" + vesta::hex64(decode_tmp.flags_info.is_not_extended) +
            ") opcode2(" + vesta::hex64(decode_tmp.flags_info.opcode_index) + ")" <<
            "Bytes64: " <<
            "\n"
            //<< process->to_string(),
            ,
            vesta::scout() << vesta::dump(process->vm_mem, process->registers.rip.raw(), 64) << std::endl;
            vm_hook(process, DebugStage::DecodeEnd);
        );

        // obtenemos los metadatos de la instruccion.
        decode_tmp.metadata = &metadata;

        // -------------------------------------------------------------------------------------------------------

        // ejecutamos el metodo encarga de descodificar dicha instruccion.
        metadata.decode(process, decode_tmp);

        // Guardar en caché, despues de llamara a decode, muy importante el orden.
        // los metodos de metadata.decode modifican la estructura metadata.decoded
        // que luego copiamos a la cache.

        // guardar en caché
        process->icache[idx] = decode_tmp;

        // apuntar a la entrada de caché
        process->decoded_ptr = &process->icache[idx];

        const uint64_t t2 = now_ns();
        process->scheduler.time_decode += (t2 - t1);

        PROFILE_END("DECODER");
        // realizamos el hook al final de la fase
        vm_hook(process, DebugStage::DecodeEnd);
    }


    vm_event execute_instruction(ProcessVM *process) {
        // realizamos el hook antes de la ejecuccion
        vm_hook(process, DebugStage::ExecuteBegin);
        PROFILE_START

        // --- PROFILER: inicio ---
        // debemos ponerlo despues de la hook para no contabilizar el tiempo de las hook
        const uint64_t t1 = now_ns();
        // ------------------------

        // ejecutamos la instruccion descodificada. No hacemos aqui
        // validacion del campo "exec" por que se supone que ya hemos comprobado en la fase de decode
        // que la instruccion tiene un metodo ejecutor. Entonces no queremos realizar mas comprobaciones para
        // no consumir mas ciclos de reloj.
        process->decoded_ptr->metadata->exec(process, *process->decoded_ptr);
        //execute_instr(this, *decoded_ptr);

        /**
         * Si la instrucción requiere esperar I/O u otra.
         * Retornamos aqui y no despues de incrementar PC para que el flujo
         * de codigo no avance, ya que hasta que esta instruccion no deje de
         * estar bloqueada, no podemos avanzar el registro PC.
         */
        if (process->decoded_ptr->flags_info.blocking) {
            vm_hook(process, DebugStage::ExecuteEnd);
            return EVT_IO_WAIT; // EVT_IO_WAIT
        }
        //if (process->should_kill) {
        //    vm_hook(process, DebugStage::ExecuteEnd);
        //    return EVT_ERROR; // matar a la VM
        //}

        // Si la instrucción NO modificó el PC, lo avanzamos, esto siempre pasara a no ser que sea un jmp o un call.
        // o una instruccion similar que modifique PC por su cuenta.
        if (!process->decoded_ptr->flags_info.did_jump)
            // movemos el puntero de instruccion al final de ejecutar la instruccion
            process->registers.rip.qword(process->registers.rip.raw() + process->decoded_ptr->flags_info.size_instr);
        else process->decoded_ptr->flags_info.did_jump = false; // ejecuta una vez la isntruccion, desmarcamos el salto

        // --- PROFILER: fin ---
        const uint64_t t2 = now_ns();

        process->scheduler.profiler_sample++;

        process->scheduler.profiler_instr_counter++; // IPS sampling
        process->scheduler.time_exec += (t2 - t1);   // tiempo ocupado

        // antes de retorna hacemos el hook
        PROFILE_END("EXECUTER");
        vm_hook(process, DebugStage::ExecuteEnd);

        // indicamos que la ejecuccion tuvo exito y no se requiere bloquear la VM.
        return EVT_EXEC_DONE; // si se puede seguir ejecutando instrucciones
    }
}
