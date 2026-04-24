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
 * @file exec_instruction.cpp
 * @brief Implementacion del executor principal de instrucciones de VestaVM.
 *
 * Implementa exec_instruction(): el bucle principal de ejecucion con despacho
 * por tabla de punteros a funcion.  Incluye instrucciones de pila, control
 * de flujo (JMP, JCC, JREL, CALL, RET, ENTER, LEAVE), llamadas nativas
 * (CALLN, CALLVM), syscall, interrupcion (INT) y OOP (CALLVIRT, etc.).
 */#include "runtime/exec_instruction.h"
#include "runtime/decode_instruction.h"
#include "loader/oop_types.h"

#include "ffi/native_ffi.h"

namespace runtime {

    /**
     * @brief Ejecuta la instruccion HLT: detiene el proceso de forma cooperativa.
     *
     * Marca la instruccion como bloqueante antes de notificar al scheduler
     * mediante EVT_HALT.  Sin el marcado previo, el scheduler podria volver
     * a la fase execute y acceder a memoria ya liberada.
     *
     * @param vm    Proceso virtual que ejecuta HLT.
     * @param instr Instruccion descodificada (no se usan sus campos).
     */
    void exec_instr_hlt(ProcessVM *vm, const DecodedInstr &instr) {
        // bloquear la instruccion ANTES de notificar al scheduler para evitar re-entradas
        vm->scheduler.on_event(EVT_HALT);
        vm->decoded_ptr->flags_info.blocking = true; // impedir el avance del PC tras exec
    }

    /**
     * @brief Ejecuta la instruccion PUSH: apila un valor en la pila del proceso.
     *
     * Soporta registros especiales (siempre 8 bytes) y registros generales
     * (tamano determinado por el modo codificado).  Decrementa RSP en el numero
     * de bytes del valor y escribe el contenido en la memoria virtual.
     *
     * @param vm    Proceso virtual que ejecuta PUSH.
     * @param instr Instruccion descodificada con reg_ext, reg_data.reg1 y mode.
     */
    void exec_instr_push(ProcessVM *vm, const DecodedInstr &instr) {
        uint8_t reg_ext  = instr.flags_info.reg_ext;    // indica si es registro especial
        uint8_t reg_code = instr.data_instruction.reg_data.reg1; // codigo de registro

        uint64_t value; // valor a apilar
        size_t   size;  // numero de bytes del valor

        if (reg_ext) {
            // registro especial (RIP, RSP, RBP, CURn...): siempre 8 bytes
            value = read_special(vm, reg_code);
            size  = 8;
        } else {
            // registro general: tamano determinado por el modo
            uint8_t mode = instr.flags_info.mode;
            size         = Assembly::Bytecode::mode_to_bytes(mode);
            value        = read_reg_table[mode](vm, reg_code); // leer segun tamano
        }

        // decrementar la pila y escribir el valor
        vm->registers.stack_pointer.qword(vm->registers.stack_pointer.qword() - size);
        vm->vm_mem.write_bytes(vm->registers.stack_pointer.raw(), &value, size);
    }

    /**
     * @brief Ejecuta la instruccion MOV con inmediato.
     *
     * Tres variantes segun los campos direction y _signed_instruct:
     *   1. direction=1, signed=1 -> escribir en registro especial (RIP/RSP/RBP/CURn)
     *   2. direction=0, signed=0 -> mov reg, imm (registro general)
     *   3. direction=1, signed=0 -> mov [reg], imm (escritura en memoria)
     *
     * @param vm    Proceso virtual que ejecuta MOV imm.
     * @param instr Instruccion descodificada con inmmed_data.reg, inmmed y mode.
     */
    void exec_instr_inmed_mov(ProcessVM *vm, const DecodedInstr &instr) {
        const int rdst      = instr.data_instruction.inmmed_data.reg;     // registro destino
        uint64_t  imm       = instr.data_instruction.inmmed_data.inmmed;  // valor inmediato
        uint8_t   direction = instr.flags_info.direction;                 // modo de acceso
        uint8_t   is_signed = instr.flags_info._signed_instruct;          // indicador de registro especial

        // variante 1: registro especial (RIP provoca un salto implicito)
        if (direction == 1 && is_signed == 1) {
            write_special(vm, rdst, imm);
            return;
        }

        // variante 2: mov reg, imm -> escritura directa en registro general
        if (direction == 0 && is_signed == 0) {
            write_reg_table[instr.flags_info.mode](vm, rdst, imm);
            return;
        }

        // variante 3: mov [reg], imm -> escritura del inmediato en la direccion apuntada por reg
        uint64_t addr = vm->registers.regs[rdst].raw();                          // direccion destino
        size_t   size = Assembly::Bytecode::mode_to_bytes(instr.flags_info.mode); // bytes a escribir
        vm->vm_mem.write_bytes(addr, &imm, size);
    }

    /**
     * @brief Ejecuta la instruccion POP: desapila un valor de la pila del proceso.
     *
     * Lee el numero de bytes indicado por el modo desde la direccion RSP actual,
     * incrementa RSP y escribe el valor en el registro destino.  Soporta tanto
     * registros especiales como generales.
     *
     * @param vm    Proceso virtual que ejecuta POP.
     * @param instr Instruccion descodificada con reg_ext, reg_data.reg1 y mode.
     */
    void exec_instr_pop(ProcessVM *vm, const DecodedInstr &instr) {
        uint8_t reg_ext  = instr.flags_info.reg_ext;                      // indicador de registro especial
        uint8_t reg_code = instr.data_instruction.reg_data.reg1;           // codigo de registro destino

        // tamano: siempre 8 bytes para especiales, o segun el modo para generales
        size_t size = reg_ext ? 8 : Assembly::Bytecode::mode_to_bytes(instr.flags_info.mode);

        uint64_t value = 0;
        vm->vm_mem.read_bytes(vm->registers.stack_pointer.raw(), &value, size); // leer de la pila
        vm->registers.stack_pointer.qword(vm->registers.stack_pointer.qword() + size); // avanzar RSP

        if (reg_ext) {
            write_special(vm, reg_code, value); // restaurar en registro especial
        } else {
            uint8_t mode = instr.flags_info.mode;
            write_reg_table[mode](vm, reg_code, value); // restaurar en registro general
        }
    }

    /**
     * @brief Ejecuta la instruccion CALLN: llama a una funcion nativa del host.
     *
     * El puntero de funcion y el numero de argumentos (argc) provienen de la
     * icache para evitar accesos extra a registros en cada llamada.  Los
     * argumentos se leen de R01..R12 segun argc (hasta 12 parametros).
     * El valor de retorno se escribe en R00.
     *
     * El switch compila a tabla de saltos con el mismo costo de indireccion
     * que dispatch[], pero cada case llama directamente a fn sin frame
     * intermedio, leyendo solo los registros necesarios.
     *
     * @param vm    Proceso virtual que ejecuta CALLN.
     * @param instr Instruccion descodificada con inmmed_data.inmmed (fn) y reg (argc).
     */
    void exec_instr_calln(ProcessVM *vm, const DecodedInstr &instr) {
        void *   fn   = reinterpret_cast<void *>(instr.data_instruction.inmmed_data.inmmed); // puntero a la funcion nativa
        uint64_t argc = instr.data_instruction.inmmed_data.reg; // argc cacheado en decode
        uint64_t r    = 0; // valor de retorno

        typedef uint64_t u64;

        // macro auxiliar para leer R01..R12 de forma concisa
#define A(n) vm->registers.regs[R##n].qword()
        switch (argc) {
            case 0: r = reinterpret_cast<u64(*)()>(fn)();
                break;
            case 1: r = reinterpret_cast<u64(*)(u64)>(fn)(
                    A(01));
                break;
            case 2: r = reinterpret_cast<u64(*)(
                    u64, u64)>(fn)(
                    A(01),A(02));
                break;
            case 3: r = reinterpret_cast<u64(*)(
                    u64, u64, u64)>(fn)(
                    A(01),A(02),A(03));
                break;
            case 4: r = reinterpret_cast<u64(*)(
                    u64, u64, u64, u64)>(fn)(
                    A(01),A(02),A(03),A(04));
                break;
            case 5: r = reinterpret_cast<u64(*)(
                    u64, u64, u64, u64, u64)>(fn)(
                    A(01),A(02),A(03),A(04),A(05));
                break;
            case 6: r = reinterpret_cast<u64(*)(
                    u64, u64, u64, u64, u64, u64)>(fn)(
                    A(01),A(02),A(03),A(04),A(05),A(06));
                break;
            case 7: r = reinterpret_cast<u64(*)(
                    u64, u64, u64, u64, u64, u64, u64)>(fn)(
                    A(01),A(02),A(03),A(04),A(05),A(06),A(07));
                break;
            case 8: r = reinterpret_cast<u64(*)(
                    u64, u64, u64, u64, u64, u64, u64, u64)>(fn)(
                    A(01),A(02),A(03),A(04),A(05),A(06),A(07),A(08));
                break;
            case 9: r = reinterpret_cast<u64(*)(
                    u64, u64, u64, u64, u64, u64, u64, u64, u64)>(fn)(
                    A(01),A(02),A(03),A(04),A(05),A(06),A(07),A(08),A(09));
                break;
            case 10: r = reinterpret_cast<u64(*)(
                    u64, u64, u64, u64, u64, u64, u64, u64, u64, u64)>(fn)(
                    A(01),A(02),A(03),A(04),A(05),A(06),A(07),A(08),A(09),A(10));
                break;
            case 11: r = reinterpret_cast<u64(*)(
                    u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64)>(fn)(
                    A(01),A(02),A(03),A(04),A(05),A(06),A(07),A(08),A(09),A(10),A(11));
                break;
            case 12: r = reinterpret_cast<u64(*)(
                    u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64, u64)>(fn)(
                    A(01),A(02),A(03),A(04),A(05),A(06),A(07),A(08),A(09),A(10),A(11),A(12));
                break;
            default:
                VM_ASSERT(false, "exec_instr_calln: argc=" + std::to_string(argc) + " excede el maximo de 12", {});
                break; // en modo release evita que el switch continue
        }
#undef A
        vm->registers.regs[R00].qword(r); // devolver el resultado en R00
    }

    /**
     * @brief Ejecuta la instruccion XCHG: intercambia dos valores entre registros.
     *
     * Cada operando puede ser un registro general o especial segun su flag.
     * Lee ambos valores, escribe el del registro 1 en el registro 2 y viceversa.
     * El modo del registro general se codifica en los bits 7-4 de su campo.
     *
     * @param vm    Proceso virtual que ejecuta XCHG.
     * @param instr Instruccion descodificada con regs_data_extent.
     */
    void exec_instr_xchg(ProcessVM *vm, const DecodedInstr &instr) {
        uint64_t val1 = 0; // valor leido del registro 1
        uint64_t val2 = 0; // valor leido del registro 2

        bool    reg1_is_general = false; // true si reg1 es un registro general
        uint8_t reg1_mode       = 0;     // modo de acceso del registro general 1
        uint8_t reg1_general    = 0;     // codigo del registro general 1

        if (instr.data_instruction.regs_data_extent.reg1_flags == 1) {
            // reg1 es un registro especial: leer directamente
            val1            = read_special(vm, instr.data_instruction.regs_data_extent.reg1);
            reg1_is_general = false;
        } else {
            // reg1 es un registro general: extraer modo y numero del codigo de 6 bits
            reg1_mode       = instr.data_instruction.regs_data_extent.reg1 >> 4;     // nibble alto = modo
            reg1_general    = instr.data_instruction.regs_data_extent.reg1 & 0b1111; // nibble bajo = registro
            val1            = read_reg_table[reg1_mode](vm, reg1_general);
            reg1_is_general = true;
        }

        if (instr.data_instruction.regs_data_extent.reg2_flags == 1) {
            // reg2 es un registro especial
            val2 = read_special(vm, instr.data_instruction.regs_data_extent.reg2);
            // escribir el valor del registro 1 en el registro 2
            write_special(vm, instr.data_instruction.regs_data_extent.reg2, val1);
        } else {
            // reg2 es un registro general
            uint8_t mode        = instr.data_instruction.regs_data_extent.reg2 >> 4;
            uint8_t reg_general = instr.data_instruction.regs_data_extent.reg2 & 0b1111;

            val2 = read_reg_table[mode](vm, reg_general);
            // escribir el valor del registro 1 en el registro 2
            write_reg_table[mode](vm, reg_general, val1);
        }

        // completar el intercambio: escribir el valor del registro 2 en el registro 1
        if (reg1_is_general == 1) {
            write_reg_table[reg1_mode](vm, reg1_general, val2); // registro general
        } else {
            write_special(vm, instr.data_instruction.regs_data_extent.reg1, val2); // registro especial
        }
    }

    /**
     * @brief Ejecuta la instruccion JMP: salto absoluto condicional o incondicional.
     *
     * Evalua la condicion codificada en el campo cond usando los flags del proceso.
     * Si la condicion es verdadera, actualiza RIP a la direccion absoluta destino.
     * El codigo 0x0F (y cualquier otro no listado) equivale a salto incondicional.
     *
     * @param vm    Proceso virtual que ejecuta JMP.
     * @param instr Instruccion descodificada con inmmed_data.reg (cond) e inmmed (addr).
     */
    void exec_instr_jmp(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  cond = instr.data_instruction.inmmed_data.reg;     // codigo de condicion
        const uint64_t addr = instr.data_instruction.inmmed_data.inmmed;  // direccion absoluta destino
        auto          &fl   = vm->registers.flags.bits;                   // referencia a los flags

        bool taken; // resultado de la evaluacion de la condicion
        switch (cond) {
            case 0x00: taken = COND_EQ(fl); break; // ZF==1
            case 0x01: taken = COND_NE(fl); break; // ZF==0
            case 0x02: taken = COND_CS(fl); break; // CF==1
            case 0x03: taken = COND_CC(fl); break; // CF==0
            case 0x04: taken = COND_MI(fl); break; // SF==1
            case 0x05: taken = COND_PL(fl); break; // SF==0
            case 0x06: taken = COND_VS(fl); break; // OF==1
            case 0x07: taken = COND_VC(fl); break; // OF==0
            case 0x08: taken = COND_HI(fl); break; // CF==0 && ZF==0
            case 0x09: taken = COND_LS(fl); break; // CF==1 || ZF==1
            case 0x0A: taken = COND_GE(fl); break; // SF==OF
            case 0x0B: taken = COND_LT(fl); break; // SF!=OF
            case 0x0C: taken = (fl.ZF == 0 && fl.SF == fl.OF); break; // GT
            case 0x0D: taken = (fl.ZF == 1 || fl.SF != fl.OF); break; // LE
            default:   taken = true; break; // incondicional (0x0F y otros)
        }

        if (taken)
            write_rip(vm, addr); // actualizar RIP al destino del salto
    }

    /**
     * @brief Ejecuta la instruccion CALLVM: llamada a subrutina de bytecode.
     *
     * Empuja la direccion de retorno (PC + size_instr) en la pila del proceso
     * y salta a la direccion indicada en el campo inmediato de la instruccion.
     *
     * @param vm    Proceso virtual que ejecuta CALLVM.
     * @param instr Instruccion descodificada con inmmed_data.inmmed (addr) y size_instr.
     */
    void exec_instr_callvm(ProcessVM *vm, const DecodedInstr &instr) {
        const uint64_t addr     = instr.data_instruction.inmmed_data.inmmed;       // direccion del callee
        const uint64_t ret_addr = vm->registers.rip.raw() + instr.flags_info.size_instr; // siguiente PC

        // apilar la direccion de retorno para que RET pueda recuperarla
        vm->registers.stack_pointer.qword(vm->registers.stack_pointer.qword() - 8);
        vm->vm_mem.write_bytes(vm->registers.stack_pointer.raw(), &ret_addr, 8);
        write_rip(vm, addr); // saltar al callee
    }

    /**
     * @brief Ejecuta la instruccion RET: retorno de subrutina.
     *
     * Si existe un frame OOP activo en frame_stack lo elimina antes de
     * desapilar la direccion de retorno y saltar a ella.
     *
     * @param vm    Proceso virtual que ejecuta RET.
     * @param instr Instruccion descodificada (no se usan sus campos).
     */
    void exec_instr_ret(ProcessVM *vm, const DecodedInstr &instr) {
        // descartar el frame OOP si existe (instrucciones CALLVIRT/CALLSUPER)
        if (vm->frame_stack != nullptr) {
            loader::FrameHeader *frame = vm->frame_stack;
            vm->frame_stack = frame->prev; // subir al frame anterior
            delete frame;                  // liberar el frame actual
        }

        uint64_t ret_addr = 0;
        vm->vm_mem.read_bytes(vm->registers.stack_pointer.raw(), &ret_addr, 8); // leer la direccion de retorno
        vm->registers.stack_pointer.qword(vm->registers.stack_pointer.qword() + 8); // liberar el slot de la pila
        write_rip(vm, ret_addr); // saltar a la direccion de retorno
    }

    /**
     * @brief Ejecuta la instruccion ENTER: crea un nuevo marco de pila.
     *
     * Secuencia clasica de prologo: push RBP, mov RBP RSP, sub RSP frame_size.
     * El campo inmediato de la instruccion indica el tamano del nuevo frame.
     *
     * @param vm    Proceso virtual que ejecuta ENTER.
     * @param instr Instruccion descodificada con inmmed_data.inmmed (frame_size).
     */
    void exec_instr_enter(ProcessVM *vm, const DecodedInstr &instr) {
        const uint64_t frame_size = instr.data_instruction.inmmed_data.inmmed; // tamano del frame en bytes
        const uint64_t rbp_val    = vm->registers.base_pointer.raw();          // valor actual de RBP

        // push rbp: guardar el frame base del llamador
        vm->registers.stack_pointer.qword(vm->registers.stack_pointer.qword() - 8);
        vm->vm_mem.write_bytes(vm->registers.stack_pointer.raw(), &rbp_val, 8);

        // mov rbp, rsp: establecer el nuevo frame base
        vm->registers.base_pointer.raw(vm->registers.stack_pointer.raw());

        // sub rsp, frame_size: reservar espacio para las variables locales
        vm->registers.stack_pointer.qword(vm->registers.stack_pointer.qword() - frame_size);
    }

    /**
     * @brief Ejecuta la instruccion LEAVE: destruye el marco de pila actual.
     *
     * Secuencia clasica de epilogo: mov RSP RBP, pop RBP.
     * Inversa de ENTER: libera el espacio de variables locales y restaura RBP.
     *
     * @param vm    Proceso virtual que ejecuta LEAVE.
     * @param instr Instruccion descodificada (no se usan sus campos).
     */
    void exec_instr_leave(ProcessVM *vm, const DecodedInstr &instr) {
        // mov rsp, rbp: liberar el espacio del frame actual
        vm->registers.stack_pointer.raw(vm->registers.base_pointer.raw());

        // pop rbp: restaurar el frame base del llamador
        uint64_t rbp_val = 0;
        vm->vm_mem.read_bytes(vm->registers.stack_pointer.raw(), &rbp_val, 8);
        vm->registers.stack_pointer.qword(vm->registers.stack_pointer.qword() + 8);
        vm->registers.base_pointer.raw(rbp_val);
    }

    /**
     * @brief Ejecuta la instruccion JMPR: salto absoluto a la direccion contenida en un registro.
     *
     * Si reg_ext esta activo el registro es especial, de lo contrario es un
     * registro general de 64 bits.
     *
     * @param vm    Proceso virtual que ejecuta JMPR.
     * @param instr Instruccion descodificada con reg_ext y reg_data.reg1.
     */
    void exec_instr_jmpr(ProcessVM *vm, const DecodedInstr &instr) {
        // obtener la direccion del registro segun si es especial o general
        const uint64_t addr = instr.flags_info.reg_ext
            ? read_special(vm, instr.data_instruction.reg_data.reg1)  // registro especial
            : read_reg64(vm, instr.data_instruction.reg_data.reg1);   // registro general de 64 bits
        write_rip(vm, addr); // saltar a la direccion obtenida
    }

    /**
     * @brief Ejecuta la instruccion CALLVMR: llamada indirecta a subrutina via registro.
     *
     * Igual que CALLVM pero la direccion del callee se obtiene de un registro
     * (general o especial segun reg_ext).  Empuja ret_addr en la pila antes de saltar.
     *
     * @param vm    Proceso virtual que ejecuta CALLVMR.
     * @param instr Instruccion descodificada con reg_ext, reg_data.reg1 y size_instr.
     */
    void exec_instr_callvmr(ProcessVM *vm, const DecodedInstr &instr) {
        // obtener la direccion del callee desde el registro indicado
        const uint64_t addr     = instr.flags_info.reg_ext
            ? read_special(vm, instr.data_instruction.reg_data.reg1)
            : read_reg64(vm, instr.data_instruction.reg_data.reg1);
        const uint64_t ret_addr = vm->registers.rip.raw() + instr.flags_info.size_instr; // PC de retorno

        // apilar la direccion de retorno
        vm->registers.stack_pointer.qword(vm->registers.stack_pointer.qword() - 8);
        vm->vm_mem.write_bytes(vm->registers.stack_pointer.raw(), &ret_addr, 8);
        write_rip(vm, addr); // saltar al callee
    }

    /**
     * @brief Ejecuta la instruccion JREL: salto relativo condicional o incondicional.
     *
     * El desplazamiento de 32 bits con signo se suma a (PC + size_instr), lo que
     * permite saltos hacia atras y hacia adelante respecto a la instruccion siguiente.
     * La condicion se evalua igual que en JMP.
     *
     * @param vm    Proceso virtual que ejecuta JREL.
     * @param instr Instruccion descodificada con inmmed_data.reg (cond) e inmmed (disp32 extendido).
     */
    void exec_instr_jrel(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t cond = instr.data_instruction.inmmed_data.reg;    // codigo de condicion
        const int64_t disp = static_cast<int64_t>(instr.data_instruction.inmmed_data.inmmed); // desplazamiento con signo
        auto         &fl   = vm->registers.flags.bits;                  // referencia a los flags

        bool taken; // resultado de la evaluacion de la condicion
        switch (cond) {
            case 0x00: taken = COND_EQ(fl); break;
            case 0x01: taken = COND_NE(fl); break;
            case 0x02: taken = COND_CS(fl); break;
            case 0x03: taken = COND_CC(fl); break;
            case 0x04: taken = COND_MI(fl); break;
            case 0x05: taken = COND_PL(fl); break;
            case 0x06: taken = COND_VS(fl); break;
            case 0x07: taken = COND_VC(fl); break;
            case 0x08: taken = COND_HI(fl); break;
            case 0x09: taken = COND_LS(fl); break;
            case 0x0A: taken = COND_GE(fl); break;
            case 0x0B: taken = COND_LT(fl); break;
            case 0x0C: taken = (fl.ZF == 0 && fl.SF == fl.OF); break;
            case 0x0D: taken = (fl.ZF == 1 || fl.SF != fl.OF); break;
            default:   taken = true; break; // incondicional
        }

        if (taken)
            // destino = PC + size_instr + desplazamiento (relativo a la instruccion siguiente)
            write_rip(vm, vm->registers.rip.raw() + instr.flags_info.size_instr + static_cast<uint64_t>(disp));
    }

} // namespace runtime
