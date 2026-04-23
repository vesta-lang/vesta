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
#ifndef EXEC_INSTRUCTION_H
#define EXEC_INSTRUCTION_H
#include <cstdint>
#include "decode_table.h"
#include "emmit/emmit_decl.h"

namespace runtime {
    static constexpr uint64_t MASKS[4] = {
        0xFFULL,              // 8 bits
        0xFFFFULL,            // 16 bits
        0xFFFFFFFFULL,        // 32 bits
        0xFFFFFFFFFFFFFFFFULL // 64 bits
    };

    static constexpr int SIGNBIT[4] = {7, 15, 31, 63};

    using ReadRegFn = uint64_t(*)(ProcessVM *, uint8_t);

    static uint64_t read_reg8(ProcessVM *vm, uint8_t r) {
        return vm->registers.regs[r].byte_lo();
    }

    static uint64_t read_reg16(ProcessVM *vm, uint8_t r) {
        return vm->registers.regs[r].word_lo();
    }

    static uint64_t read_reg32(ProcessVM *vm, uint8_t r) {
        return vm->registers.regs[r].dword_lo();
    }

    static uint64_t read_reg64(ProcessVM *vm, uint8_t r) {
        return vm->registers.regs[r].qword();
    }

    static constexpr ReadRegFn read_reg_table[] = {
        read_reg8,
        read_reg16,
        read_reg32,
        read_reg64
    };

    using WriteRegFn = void(*)(ProcessVM *, uint8_t, uint64_t);

    static void write_reg8(ProcessVM *vm, uint8_t r, uint64_t v) {
        vm->registers.regs[r].byte_lo(v);
    }

    static void write_reg16(ProcessVM *vm, uint8_t r, uint64_t v) {
        vm->registers.regs[r].word_lo(v);
    }

    static void write_reg32(ProcessVM *vm, uint8_t r, uint64_t v) {
        vm->registers.regs[r].dword_lo(v);
    }

    static void write_reg64(ProcessVM *vm, uint8_t r, uint64_t v) {
        vm->registers.regs[r].qword(v);
    }

    static constexpr WriteRegFn write_reg_table[] = {
        write_reg8,
        write_reg16,
        write_reg32,
        write_reg64
    };

    using ReadSpecialFn = uint64_t(*)(ProcessVM *);

    static uint64_t read_cur0(ProcessVM *vm) {
        return vm->registers.cur[0].qword();
    }

    static uint64_t read_cur1(ProcessVM *vm) {
        return vm->registers.cur[1].qword();
    }

    static uint64_t read_cur2(ProcessVM *vm) {
        return vm->registers.cur[2].qword();
    }

    static uint64_t read_cur3(ProcessVM *vm) {
        return vm->registers.cur[3].qword();
    }

    static uint64_t read_rip(ProcessVM *vm) {
        return vm->registers.rip.raw();
    }

    static uint64_t read_rbp(ProcessVM *vm) {
        return vm->registers.base_pointer.raw();
    }

    static uint64_t read_rsp(ProcessVM *vm) {
        return vm->registers.stack_pointer.raw();
    }

    static uint64_t read_rflags(ProcessVM *vm) {
        return vm->registers.flags.raw;
    }

    static constexpr ReadSpecialFn read_special_table[12] = {
        read_cur0,  // 0
        read_cur1,  // 1
        read_cur2,  // 2
        read_cur3,  // 3
        nullptr,    // 4
        nullptr,    // 5
        nullptr,    // 6
        nullptr,    // 7
        read_rip,   // 8
        read_rbp,   // 9
        read_rsp,   // 10
        read_rflags // 11
    };

    static const char *regs_special[] = {
        "cur0",
        "cur1",
        "cur2",
        "cur3",
        "nullptr",
        "nullptr",
        "nullptr",
        "nullptr",
        "rip",
        "rbp",
        "rsp",
        "rflags",
    };


    inline uint64_t read_special(ProcessVM *vm, uint8_t code) {
        if (code >= 12 || read_special_table[code] == nullptr) {
            return 0;
        }
        return read_special_table[code](vm);
    }

    using WriteSpecialFn = void(*)(ProcessVM *, uint64_t);

    static void write_cur0(ProcessVM *vm, uint64_t v) {
        vm->registers.cur[0].qword(v);
    }

    static void write_cur1(ProcessVM *vm, uint64_t v) {
        vm->registers.cur[1].qword(v);
    }

    static void write_cur2(ProcessVM *vm, uint64_t v) {
        vm->registers.cur[2].qword(v);
    }

    static void write_cur3(ProcessVM *vm, uint64_t v) {
        vm->registers.cur[3].qword(v);
    }

    static void write_rip(ProcessVM *vm, uint64_t v) {
        vm->registers.rip.raw(v);
        // al modificar rip, se esta haciendo un salto
        vm->decoded_ptr->flags_info.did_jump = true;
    }

    static void write_rbp(ProcessVM *vm, uint64_t v) {
        vm->registers.base_pointer.raw(v);
    }

    static void write_rsp(ProcessVM *vm, uint64_t v) {
        vm->registers.stack_pointer.raw(v);
    }

    static void write_rflags(ProcessVM *vm, uint64_t v) {
        vm->registers.flags.raw = v;
    }

    static constexpr WriteSpecialFn write_special_table[] = {
        write_cur0,  // 0b000000
        write_cur1,  // 0b000001
        write_cur2,  // 0b000010
        write_cur3,  // 0b000011
        nullptr,     // 0b000100
        nullptr,     // 0b000101
        nullptr,     // 0b000110
        nullptr,     // 0b000111
        write_rip,   // 0b001000
        write_rbp,   // 0b001001
        write_rsp,   // 0b001010
        write_rflags // 0b001011
    };

    inline void write_special(ProcessVM *vm, uint8_t code, uint64_t v) {
        if (code >= sizeof(write_special_table) ||
            write_special_table[code % sizeof(write_special_table)] == nullptr
        ) {
            VM_ASSERT(false, "write_special: code=" + std::to_string(code) + " is not (code >= 12)", {});
            return;
        }
        write_special_table[code](vm, v);
    }



    // ---------------------------------------------------------------------
    //                          de tipo registro
    // ---------------------------------------------------------------------
    /**
     * Permite ejecutar la instruccion INC o DEC dependiendo del valor de
     * instr._signed_instruct:
     *      - (instr._signed_instruct == 0) = INC
     *      - (instr._signed_instruct == 1) = DEC
     * @param vm
     * @param instr
     */
    void exec_instr_inc_dec_reg(ProcessVM *vm, const DecodedInstr &instr);

    void exec_instr_mov_reg(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * Permite ejecutar una instruccion de tipo
     * add reg, reg
     */
    void exec_instr_add_reg(ProcessVM *vm, const DecodedInstr &instr);

    void exec_instr_sub_reg(ProcessVM *vm, const DecodedInstr &instr);

    void exec_instr_mul_reg(ProcessVM *vm, const DecodedInstr &instr);

    void exec_instr_cmp_reg(ProcessVM *vm, const DecodedInstr &instr);

    void exec_instr_and_reg(ProcessVM *vm, const DecodedInstr &instr);

    void exec_instr_or_reg(ProcessVM *vm, const DecodedInstr &instr);

    void exec_instr_xor_reg(ProcessVM *vm, const DecodedInstr &instr);

    void exec_instr_div_reg(ProcessVM *vm, const DecodedInstr &instr);

    // ---------------------------------------------------------------------


    // ---------------------------------------------------------------------
    //                          de tipo inmediato
    // ---------------------------------------------------------------------
    void exec_instr_add_imm(ProcessVM *vm, const DecodedInstr &instr);

    // ---------------------------------------------------------------------

    /**
     * Permite ejecutar una instruccion HLT, esto hara que la fase
     * EXECUTE emita un evento de tipo EVT_ERROR
     * @param vm
     * @param instr
     */
    void exec_instr_hlt(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * Permite ejecutar una instruccion de tipo PUSH.
     * PUSH:
     *      - SP -= size
     *      - [SP] = value
     * @param vm
     * @param instr
     */
    void exec_instr_push(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * Permite ejecutar una instruccion de tipo POP.
     * POP:
     *      - value = [SP]
     *      - SP += size
     *
     * @param vm
     * @param instr
     */
    void exec_instr_pop(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * Permite ejecutar una instruccion CALLN que realiza una llamada
     * nativa, puede bloquear la VM si la funcion bloquea.
     * @param vm
     * @param instr
     */
    void exec_instr_calln(ProcessVM *vm, const DecodedInstr &instr);

    void exec_instr_xchg(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * JMP incondicional y Jcc condicionales.
     * El byte de condición en inmmed_data.reg selecciona la condición;
     * 0x0F (o cualquier valor no reconocido) salta siempre.
     *
     * Códigos de condición:
     *   0x00 je/jz   0x01 jne/jnz  0x02 jcs/jae  0x03 jcc/jb
     *   0x04 jmi     0x05 jpl      0x06 jvs       0x07 jvc
     *   0x08 jhi     0x09 jls      0x0A jge       0x0B jlt
     *   0x0C jgt     0x0D jle      0x0F jmp (siempre)
     */
    void exec_instr_jmp(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * CALLVM addr: empuja la dirección de retorno (RIP + tamaño) en la pila
     * y salta a addr. Equivalente al CALL de x64 pero dentro de la VM.
     */
    void exec_instr_callvm(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * RET: extrae la dirección de retorno de la pila y salta a ella.
     */
    void exec_instr_ret(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * ENTER frame_size: crea un stack frame.
     *   push rbp  ->  mov rbp, rsp  ->  sub rsp, frame_size
     */
    void exec_instr_enter(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * LEAVE: destruye el stack frame actual.
     *   mov rsp, rbp  ->  pop rbp
     */
    void exec_instr_leave(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * JMPR reg: salta a la dirección almacenada en un registro.
     * El registro se codifica en instr.data_instruction.reg_data.reg1;
     * si flags_info.reg_ext está activo se interpreta como registro especial.
     * Siempre incondicional.
     */
    void exec_instr_jmpr(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * CALLVMR reg: igual que CALLVM pero la dirección viene de un registro.
     * Empuja la dirección de retorno en la pila y salta al valor del registro.
     */
    void exec_instr_callvmr(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * JREL cond, disp32: salto relativo condicional.
     * Mismos códigos de condición que JMP (0x00-0x0D; 0x0F = siempre).
     * El desplazamiento es un int32 con signo relativo al FIN de la instrucción.
     * Formato extendido (0x00 0x2D): [0x00][0x2D][cond][pad][disp32] = 8 bytes.
     */
    void exec_instr_jrel(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * Permite ejecutar una instruccion MOV del tipo:
     * mov:
     *      - reg, 0x1000 ||
     *      -  [reg], 0x1000 ||
     *      - reg_ext, 0x1000
     * @param vm
     * @param instr
     */
    void exec_instr_inmed_mov(ProcessVM *vm, const DecodedInstr &instr);

    // -------------------------------------------------------------------------
    //                   Cursor - acceso a memoria real (0x00 0xC0 .. 0xC2)
    // -------------------------------------------------------------------------

    /** READCUR dest_reg, curN
     *  Lee N bytes de la dirección host almacenada en curN y los escribe en dest_reg.
     *  El tamaño viene dado por el campo mode (0=byte, 1=word, 2=dword, 3=qword).
     *  reg1 = registro destino general, reg2 = cursor index (0-3). */
    void exec_instr_readcur(ProcessVM *vm, const DecodedInstr &instr);

    /** WRITECUR curN, src_reg
     *  Escribe N bytes de src_reg en la dirección host almacenada en curN.
     *  El tamaño viene dado por el campo mode (0=byte, 1=word, 2=dword, 3=qword).
     *  reg1 = registro fuente general, reg2 = cursor index (0-3). */
    void exec_instr_writecur(ProcessVM *vm, const DecodedInstr &instr);

    /** GCDEREF curN, handle_reg
     *  Desreferencia el GcHandle contenido en handle_reg y almacena el puntero
     *  raw al payload en el cursor curN.
     *  reg1 = registro general con el handle, reg2 = cursor index destino (0-3). */
    void exec_instr_gcderef(ProcessVM *vm, const DecodedInstr &instr);

    // -------------------------------------------------------------------------
    //                   GC generacional (0x00 0xA0 .. 0xA3)
    // -------------------------------------------------------------------------

    /** NEWOBJ reg_size -> R0 = GcHandle
     *  reg1 = registro que contiene el numero de bytes a reservar.
     *  Retorna GC_NULL_HANDLE en R0 si no hay memoria disponible. */
    void exec_instr_newobj(ProcessVM *vm, const DecodedInstr &instr);

    /** GCRUN  (sin operandos)
     *  Ejecuta minor_gc() del proceso actual; si old_used >= threshold
     *  tambien ejecuta major_gc(). */
    void exec_instr_gcrun(ProcessVM *vm, const DecodedInstr &instr);

    /** GCCONFIG reg_threshold
     *  reg1 = registro que contiene el nuevo umbral de OldGen (bytes). */
    void exec_instr_gcconfig(ProcessVM *vm, const DecodedInstr &instr);

    /** DROP reg_handle
     *  reg1 = registro que contiene el GcHandle a liberar. */
    void exec_instr_gc_drop(ProcessVM *vm, const DecodedInstr &instr);

    /** GCWB reg_old_handle
     *  Registra el handle OLD en el remembered set para que el minor GC
     *  lo trate como raiz adicional y escanee sus referencias a YOUNG.
     *  reg1 = registro que contiene el GcHandle del objeto OLD modificado. */
    void exec_instr_gcwb(ProcessVM *vm, const DecodedInstr &instr);

    // -------------------------------------------------------------------------
    //                   Raw allocator (0x00 0xB0 .. 0xB2)
    // -------------------------------------------------------------------------

    /** ALLOC reg_size -> R0 = ptr host real (uint64_t)
     *  reg1 = registro que contiene el numero de bytes a reservar.
     *  Retorna 0 en R0 si la asignacion falla. */
    void exec_instr_raw_alloc(ProcessVM *vm, const DecodedInstr &instr);

    /** FREE reg_ptr
     *  reg1 = registro que contiene el puntero host a liberar.
     *  No hace nada si el puntero no pertenece a este proceso. */
    void exec_instr_raw_free(ProcessVM *vm, const DecodedInstr &instr);

    /** REALLOC reg_ptr, reg_size -> R0 = nuevo ptr host real
     *  reg1 = registro con el puntero original.
     *  reg2 = registro que contiene el nuevo tamano en bytes.
     *  Retorna 0 en R0 si la asignacion falla. */
    void exec_instr_raw_realloc(ProcessVM *vm, const DecodedInstr &instr);

    // -------------------------------------------------------------------------
    //                   OOP - sistema de objetos (0x00 0xD0 .. 0xDC)
    // -------------------------------------------------------------------------

    /** NEWOBJRAW reg_classinfo, reg_size -> R0 = host_ptr
     *  reg1 = registro con puntero host a ClassInfo.
     *  reg2 = registro con bytes a reservar (0 = usar classinfo->instance_size).
     *  Escribe ObjectHeader con OBJ_FLAG_RAW_OWNED al inicio del bloque. */
    void exec_instr_newobjraw(ProcessVM *vm, const DecodedInstr &instr);

    /** CALLVIRT reg_obj, vtable_idx
     *  reg1 = host_ptr al ObjectHeader del objeto receptor.
     *  reg2 = indice en la vtable (0-255).
     *  Empuja FrameHeader y salta a MethodInfo->code_vaddr. */
    void exec_instr_callvirt(ProcessVM *vm, const DecodedInstr &instr);

    /** CALLSUPER reg_classinfo, vtable_idx
     *  reg1 = host_ptr al ClassInfo de la superclase.
     *  reg2 = indice en la vtable de la superclase (0-255). */
    void exec_instr_callsuper(ProcessVM *vm, const DecodedInstr &instr);

    /** THROW reg_obj
     *  reg1 = host_ptr al ObjectHeader de la excepcion.
     *  Recorre la cadena de FrameHeaders buscando un handler compatible.
     *  Deposita el puntero de excepcion en R00 y salta al handler_pc. */
    void exec_instr_throw(ProcessVM *vm, const DecodedInstr &instr);

    /** RETHROW  (sin operandos)
     *  Relanza current_exception sin modificar el ObjectHeader. */
    void exec_instr_rethrow(ProcessVM *vm, const DecodedInstr &instr);

    /** GETCLASS reg_obj -> R0 = ClassInfo*
     *  reg1 = host_ptr al ObjectHeader del objeto. */
    void exec_instr_getclass(ProcessVM *vm, const DecodedInstr &instr);

    /** INSTANCEOF reg_obj, reg_classinfo -> R0 = bool (0/1)
     *  reg1 = host_ptr al ObjectHeader.
     *  reg2 = host_ptr al ClassInfo objetivo. */
    void exec_instr_instanceof(ProcessVM *vm, const DecodedInstr &instr);

    /** CHECKCAST reg_obj, reg_classinfo -> R0 = reg_obj o THROW ClassCastException
     *  reg1 = host_ptr al ObjectHeader.
     *  reg2 = host_ptr al ClassInfo objetivo. */
    void exec_instr_checkcast(ProcessVM *vm, const DecodedInstr &instr);

    /** GETFIELD reg_classinfo, field_idx -> R0 = FieldInfo*
     *  reg1 = host_ptr al ClassInfo.
     *  reg2 = indice del campo (0-255). */
    void exec_instr_getfield(ProcessVM *vm, const DecodedInstr &instr);

    /** GETMETHOD reg_classinfo, method_idx -> R0 = MethodInfo*
     *  reg1 = host_ptr al ClassInfo.
     *  reg2 = indice del metodo (0-255). */
    void exec_instr_getmethod(ProcessVM *vm, const DecodedInstr &instr);

    /** FIELDCOUNT reg_classinfo -> R0 = uint64 */
    void exec_instr_fieldcount(ProcessVM *vm, const DecodedInstr &instr);

    /** METHODCOUNT reg_classinfo -> R0 = uint64 */
    void exec_instr_methodcount(ProcessVM *vm, const DecodedInstr &instr);

    /** CLASSNAME reg_classinfo -> R0 = char* (puntero host a ClassInfo.name.data) */
    void exec_instr_classname(ProcessVM *vm, const DecodedInstr &instr);
}
#endif //EXEC_INSTRUCTION_H
