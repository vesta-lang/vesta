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
 * @file exec_instruction.h
 * @brief Tablas de acceso a registros y declaraciones de todos los ejecutores de instrucciones.
 *
 * Define:
 *   - MASKS[4] y SIGNBIT[4]: constantes para operaciones con signo.
 *   - read_reg_table[4] / write_reg_table[4]: despacho de lectura/escritura segun modo (8/16/32/64 bits).
 *   - read_special_table[12] / write_special_table[12]: acceso a registros especiales
 *     (CUR0-CUR3, RIP, RBP, RSP, RFLAGS).
 *   - read_special() / write_special(): wrappers seguros con comprobacion de rango.
 *   - Declaraciones de todos los ejecutores: ALU reg, ALU imm, ALU SIB, control de flujo,
 *     pila, FFI nativa, GC, raw allocator y sistema OOP.
 */

#ifndef EXEC_INSTRUCTION_H
#define EXEC_INSTRUCTION_H

#include <cstdint>
#include "decode_table.h"
#include "emmit/emmit_decl.h"

namespace runtime {

    // =========================================================================
    //  Constantes para operaciones con signo
    // =========================================================================

    /**
     * @brief Mascaras de bits para los cuatro modos de tamano de operando.
     *
     * Indice 0 = 8 bits (byte), 1 = 16 bits (word), 2 = 32 bits (dword), 3 = 64 bits (qword).
     */
    static constexpr uint64_t MASKS[4] = {
        0xFFULL,              ///< Mascara para operando de  8 bits
        0xFFFFULL,            ///< Mascara para operando de 16 bits
        0xFFFFFFFFULL,        ///< Mascara para operando de 32 bits
        0xFFFFFFFFFFFFFFFFULL ///< Mascara para operando de 64 bits
    };

    /**
     * @brief Indice del bit de signo para cada modo de tamano de operando.
     *
     * Indice 0 = bit 7 (8b), 1 = bit 15 (16b), 2 = bit 31 (32b), 3 = bit 63 (64b).
     */
    static constexpr int SIGNBIT[4] = {7, 15, 31, 63};

    // =========================================================================
    //  Tablas de lectura de registros generales (modo 0-3 -> 8/16/32/64 bits)
    // =========================================================================

    using ReadRegFn = uint64_t(*)(ProcessVM *, uint8_t); ///< Tipo de funcion de lectura de registro

    /** @brief Lee el byte bajo del registro @p r. */
    static uint64_t read_reg8(ProcessVM *vm, uint8_t r) {
        return vm->registers.regs[r].byte_lo();
    }

    /** @brief Lee la word baja del registro @p r. */
    static uint64_t read_reg16(ProcessVM *vm, uint8_t r) {
        return vm->registers.regs[r].word_lo();
    }

    /** @brief Lee la dword baja del registro @p r. */
    static uint64_t read_reg32(ProcessVM *vm, uint8_t r) {
        return vm->registers.regs[r].dword_lo();
    }

    /** @brief Lee la qword completa del registro @p r. */
    static uint64_t read_reg64(ProcessVM *vm, uint8_t r) {
        return vm->registers.regs[r].qword();
    }

    /**
     * @brief Tabla de funciones de lectura indexada por modo (0=8b, 1=16b, 2=32b, 3=64b).
     *
     * Permite seleccionar el tamano de lectura con un simple indice en lugar de un switch.
     */
    static constexpr ReadRegFn read_reg_table[] = {
        read_reg8,  ///< modo 0 ->  8 bits
        read_reg16, ///< modo 1 -> 16 bits
        read_reg32, ///< modo 2 -> 32 bits
        read_reg64  ///< modo 3 -> 64 bits
    };

    // =========================================================================
    //  Tablas de escritura de registros generales (modo 0-3 -> 8/16/32/64 bits)
    // =========================================================================

    using WriteRegFn = void(*)(ProcessVM *, uint8_t, uint64_t); ///< Tipo de funcion de escritura de registro

    /** @brief Escribe el byte bajo del registro @p r con el valor @p v. */
    static void write_reg8(ProcessVM *vm, uint8_t r, uint64_t v) {
        vm->registers.regs[r].byte_lo(v);
    }

    /** @brief Escribe la word baja del registro @p r con el valor @p v. */
    static void write_reg16(ProcessVM *vm, uint8_t r, uint64_t v) {
        vm->registers.regs[r].word_lo(v);
    }

    /** @brief Escribe la dword baja del registro @p r con el valor @p v. */
    static void write_reg32(ProcessVM *vm, uint8_t r, uint64_t v) {
        vm->registers.regs[r].dword_lo(v);
    }

    /** @brief Escribe la qword completa del registro @p r con el valor @p v. */
    static void write_reg64(ProcessVM *vm, uint8_t r, uint64_t v) {
        vm->registers.regs[r].qword(v);
    }

    /**
     * @brief Tabla de funciones de escritura indexada por modo (0=8b, 1=16b, 2=32b, 3=64b).
     */
    static constexpr WriteRegFn write_reg_table[] = {
        write_reg8,  ///< modo 0 ->  8 bits
        write_reg16, ///< modo 1 -> 16 bits
        write_reg32, ///< modo 2 -> 32 bits
        write_reg64  ///< modo 3 -> 64 bits
    };

    // =========================================================================
    //  Registros especiales: CUR0-CUR3, RIP, RBP, RSP, RFLAGS
    // =========================================================================

    using ReadSpecialFn = uint64_t(*)(ProcessVM *); ///< Tipo de funcion de lectura de registro especial

    static uint64_t read_cur0(ProcessVM *vm)    { return vm->registers.cur[0].qword(); }     ///< Lee CUR0
    static uint64_t read_cur1(ProcessVM *vm)    { return vm->registers.cur[1].qword(); }     ///< Lee CUR1
    static uint64_t read_cur2(ProcessVM *vm)    { return vm->registers.cur[2].qword(); }     ///< Lee CUR2
    static uint64_t read_cur3(ProcessVM *vm)    { return vm->registers.cur[3].qword(); }     ///< Lee CUR3
    static uint64_t read_rip(ProcessVM *vm)     { return vm->registers.rip.raw(); }          ///< Lee RIP
    static uint64_t read_rbp(ProcessVM *vm)     { return vm->registers.base_pointer.raw(); } ///< Lee RBP
    static uint64_t read_rsp(ProcessVM *vm)     { return vm->registers.stack_pointer.raw(); }///< Lee RSP
    static uint64_t read_rflags(ProcessVM *vm)  { return vm->registers.flags.raw; }          ///< Lee RFLAGS

    /**
     * @brief Tabla de funciones de lectura de registros especiales.
     *
     * Indexada por el codigo de registro especial (0-11).
     * Entradas 4-7 son nullptr (reservadas para uso futuro).
     *
     * Mapa de indices:
     *   0=CUR0, 1=CUR1, 2=CUR2, 3=CUR3, 4-7=nullptr, 8=RIP, 9=RBP, 10=RSP, 11=RFLAGS
     */
    static constexpr ReadSpecialFn read_special_table[12] = {
        read_cur0,  ///< 0  -> CUR0
        read_cur1,  ///< 1  -> CUR1
        read_cur2,  ///< 2  -> CUR2
        read_cur3,  ///< 3  -> CUR3
        nullptr,    ///< 4  -> reservado
        nullptr,    ///< 5  -> reservado
        nullptr,    ///< 6  -> reservado
        nullptr,    ///< 7  -> reservado
        read_rip,   ///< 8  -> RIP
        read_rbp,   ///< 9  -> RBP
        read_rsp,   ///< 10 -> RSP
        read_rflags ///< 11 -> RFLAGS
    };

    /**
     * @brief Nombres de los registros especiales para mensajes de diagnostico.
     *
     * Indexado igual que read_special_table (0-11).
     */
    static const char *regs_special[] = {
        "cur0", "cur1", "cur2", "cur3",
        "nullptr", "nullptr", "nullptr", "nullptr",
        "rip", "rbp", "rsp", "rflags",
    };

    /**
     * @brief Lee el valor de un registro especial de forma segura.
     *
     * Comprueba que el codigo de registro es valido y que la funcion
     * de lectura no es nullptr antes de llamarla.
     *
     * @param vm   Proceso virtual propietario de los registros.
     * @param code Codigo del registro especial (0-11).
     * @return Valor del registro, o 0 si el codigo es invalido.
     */
    inline uint64_t read_special(ProcessVM *vm, uint8_t code) {
        if (code >= 12 || read_special_table[code] == nullptr) {
            return 0; // codigo invalido o entrada reservada
        }
        return read_special_table[code](vm);
    }

    // =========================================================================
    //  Tablas de escritura de registros especiales
    // =========================================================================

    using WriteSpecialFn = void(*)(ProcessVM *, uint64_t); ///< Tipo de funcion de escritura especial

    static void write_cur0(ProcessVM *vm, uint64_t v)   { vm->registers.cur[0].qword(v); }         ///< Escribe CUR0
    static void write_cur1(ProcessVM *vm, uint64_t v)   { vm->registers.cur[1].qword(v); }         ///< Escribe CUR1
    static void write_cur2(ProcessVM *vm, uint64_t v)   { vm->registers.cur[2].qword(v); }         ///< Escribe CUR2
    static void write_cur3(ProcessVM *vm, uint64_t v)   { vm->registers.cur[3].qword(v); }         ///< Escribe CUR3

    /**
     * @brief Escribe RIP y marca did_jump para que el pipeline no avance el PC automaticamente.
     *
     * Cualquier escritura en RIP constituye un salto; la instruccion que la provoca
     * debe asegurarse de no avanzar el PC de forma adicional.
     */
    static void write_rip(ProcessVM *vm, uint64_t v) {
        vm->registers.rip.raw(v);
        vm->decoded_ptr->flags_info.did_jump = true; // marcar salto para evitar doble avance del PC
    }

    static void write_rbp(ProcessVM *vm, uint64_t v)    { vm->registers.base_pointer.raw(v); }     ///< Escribe RBP
    static void write_rsp(ProcessVM *vm, uint64_t v)    { vm->registers.stack_pointer.raw(v); }    ///< Escribe RSP
    static void write_rflags(ProcessVM *vm, uint64_t v) { vm->registers.flags.raw = v; }           ///< Escribe RFLAGS

    /**
     * @brief Tabla de funciones de escritura de registros especiales.
     *
     * Mismo mapa de indices que read_special_table.
     * Entradas 4-7 son nullptr (reservadas).
     */
    static constexpr WriteSpecialFn write_special_table[] = {
        write_cur0,   ///< 0  -> CUR0
        write_cur1,   ///< 1  -> CUR1
        write_cur2,   ///< 2  -> CUR2
        write_cur3,   ///< 3  -> CUR3
        nullptr,      ///< 4  -> reservado
        nullptr,      ///< 5  -> reservado
        nullptr,      ///< 6  -> reservado
        nullptr,      ///< 7  -> reservado
        write_rip,    ///< 8  -> RIP (marca did_jump)
        write_rbp,    ///< 9  -> RBP
        write_rsp,    ///< 10 -> RSP
        write_rflags  ///< 11 -> RFLAGS
    };

    /**
     * @brief Escribe un valor en un registro especial de forma segura.
     *
     * Valida el codigo de registro y que la funcion de escritura no sea nullptr.
     * En modo VM_DEBUG_CHECKS aborta si el codigo es invalido.
     *
     * @param vm   Proceso virtual propietario de los registros.
     * @param code Codigo del registro especial (0-11).
     * @param v    Valor a escribir en el registro.
     */
    inline void write_special(ProcessVM *vm, uint8_t code, uint64_t v) {
        if (code >= sizeof(write_special_table) ||
            write_special_table[code % sizeof(write_special_table)] == nullptr
        ) {
            VM_ASSERT(false, "write_special: code=" + std::to_string(code) + " is not (code >= 12)", {});
            return; // codigo invalido: no hacer nada en release
        }
        write_special_table[code](vm, v);
    }


    // =========================================================================
    //  Ejecutores: tipo registro
    // =========================================================================

    /**
     * @brief Ejecuta INC o DEC segun el campo _signed_instruct de la instruccion.
     *
     *   - _signed_instruct == 0 -> INC: incrementa el registro en 1.
     *   - _signed_instruct == 1 -> DEC: decrementa el registro en 1.
     *
     * @param vm    Proceso virtual que ejecuta la instruccion.
     * @param instr Instruccion descodificada.
     */
    void exec_instr_inc_dec_reg(ProcessVM *vm, const DecodedInstr &instr);

    /** @brief Ejecuta MOV reg, reg: copia el valor de un registro en otro. */
    void exec_instr_mov_reg(ProcessVM *vm, const DecodedInstr &instr);

    /** @brief Ejecuta ADD reg, reg: suma dos registros y actualiza flags. */
    void exec_instr_add_reg(ProcessVM *vm, const DecodedInstr &instr);

    /** @brief Ejecuta SUB reg, reg: resta dos registros y actualiza flags. */
    void exec_instr_sub_reg(ProcessVM *vm, const DecodedInstr &instr);

    /** @brief Ejecuta MUL reg, reg: multiplica dos registros y actualiza flags. */
    void exec_instr_mul_reg(ProcessVM *vm, const DecodedInstr &instr);

    /** @brief Ejecuta CMP reg, reg: resta sin guardar resultado; solo actualiza flags. */
    void exec_instr_cmp_reg(ProcessVM *vm, const DecodedInstr &instr);

    /** @brief Ejecuta AND reg, reg: AND bit a bit y actualiza flags. */
    void exec_instr_and_reg(ProcessVM *vm, const DecodedInstr &instr);

    /** @brief Ejecuta OR reg, reg: OR bit a bit y actualiza flags. */
    void exec_instr_or_reg(ProcessVM *vm, const DecodedInstr &instr);

    /** @brief Ejecuta XOR reg, reg: XOR bit a bit y actualiza flags. */
    void exec_instr_xor_reg(ProcessVM *vm, const DecodedInstr &instr);

    /** @brief Ejecuta DIV reg, reg: division entera y actualiza flags. */
    void exec_instr_div_reg(ProcessVM *vm, const DecodedInstr &instr);

    /** @brief Ejecuta NOT reg: complemento a uno del registro. */
    void exec_instr_not_reg(ProcessVM *vm, const DecodedInstr &instr);

    /** @brief Ejecuta SHL reg, reg: desplazamiento logico a la izquierda. */
    void exec_instr_shl_reg(ProcessVM *vm, const DecodedInstr &instr);

    /** @brief Ejecuta SHR reg, reg: desplazamiento logico a la derecha. */
    void exec_instr_shr_reg(ProcessVM *vm, const DecodedInstr &instr);

    /** @brief Ejecuta SAR reg, reg: desplazamiento aritmetico a la derecha (preserva signo). */
    void exec_instr_sar_reg(ProcessVM *vm, const DecodedInstr &instr);

    // =========================================================================
    //  Ejecutores: modo SIB (Scale-Index-Base)
    // =========================================================================

    /** @brief Ejecuta ADD con acceso a memoria SIB. */
    void exec_instr_add_sib(ProcessVM *vm, const DecodedInstr &instr);
    /** @brief Ejecuta SUB con acceso a memoria SIB. */
    void exec_instr_sub_sib(ProcessVM *vm, const DecodedInstr &instr);
    /** @brief Ejecuta MUL con acceso a memoria SIB. */
    void exec_instr_mul_sib(ProcessVM *vm, const DecodedInstr &instr);
    /** @brief Ejecuta DIV con acceso a memoria SIB. */
    void exec_instr_div_sib(ProcessVM *vm, const DecodedInstr &instr);
    /** @brief Ejecuta CMP con acceso a memoria SIB. */
    void exec_instr_cmp_sib(ProcessVM *vm, const DecodedInstr &instr);
    /** @brief Ejecuta MOV con acceso a memoria SIB. */
    void exec_instr_mov_sib(ProcessVM *vm, const DecodedInstr &instr);
    /** @brief Ejecuta MOVC: movimiento entre registro y memoria virtual o del host. */
    void exec_instr_movc_mem(ProcessVM *vm, const DecodedInstr &instr);
    /** @brief Ejecuta MOVC reg,reg: movimiento de cursor de registro a registro. */
    void exec_instr_movc_reg(ProcessVM *vm, const DecodedInstr &instr);

    // =========================================================================
    //  Ejecutores: tipo inmediato
    // =========================================================================

    /** @brief Ejecuta ADD reg, imm: suma un inmediato al registro y actualiza flags. */
    void exec_instr_add_imm(ProcessVM *vm, const DecodedInstr &instr);
    /** @brief Ejecuta SUB reg, imm: resta un inmediato del registro y actualiza flags. */
    void exec_instr_sub_imm(ProcessVM *vm, const DecodedInstr &instr);
    /** @brief Ejecuta MUL reg, imm: multiplica el registro por un inmediato. */
    void exec_instr_mul_imm(ProcessVM *vm, const DecodedInstr &instr);
    /** @brief Ejecuta DIV reg, imm: divide el registro entre un inmediato. */
    void exec_instr_div_imm(ProcessVM *vm, const DecodedInstr &instr);
    /** @brief Ejecuta CMP reg, imm: compara registro con inmediato; solo flags. */
    void exec_instr_cmp_imm(ProcessVM *vm, const DecodedInstr &instr);

    // =========================================================================
    //  Ejecutores: control de flujo y pila
    // =========================================================================

    /**
     * @brief Ejecuta HLT: detiene el proceso de forma cooperativa.
     *
     * Emite EVT_HALT al scheduler y marca la instruccion como bloqueante para
     * que execute_instruction() no avance el PC.
     */
    void exec_instr_hlt(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * @brief Ejecuta PUSH: apila un valor decrementando RSP.
     *
     * PUSH: RSP -= size; [RSP] = value.
     */
    void exec_instr_push(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * @brief Ejecuta POP: desapila un valor incrementando RSP.
     *
     * POP: value = [RSP]; RSP += size.
     */
    void exec_instr_pop(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * @brief Ejecuta CALLN: llamada a funcion nativa del host.
     *
     * Puede bloquear la VM si la funcion nativa bloquea el hilo.
     * Soporta hasta 12 argumentos en R01-R12; devuelve resultado en R00.
     */
    void exec_instr_calln(ProcessVM *vm, const DecodedInstr &instr);

    /** @brief Ejecuta XCHG: intercambia dos valores entre registros (generales y/o especiales). */
    void exec_instr_xchg(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * @brief Ejecuta JMP/Jcc: salto absoluto condicional o incondicional.
     *
     * Codigos de condicion (campo cond):
     *   0x00=je/jz, 0x01=jne/jnz, 0x02=jcs/jae, 0x03=jcc/jb,
     *   0x04=jmi,   0x05=jpl,      0x06=jvs,     0x07=jvc,
     *   0x08=jhi,   0x09=jls,      0x0A=jge,     0x0B=jlt,
     *   0x0C=jgt,   0x0D=jle,      0x0F=jmp (siempre).
     */
    void exec_instr_jmp(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * @brief Ejecuta CALLVM: llamada a subrutina de bytecode.
     *
     * Empuja ret_addr (RIP + size_instr) en la pila y salta a addr.
     */
    void exec_instr_callvm(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * @brief Ejecuta RET: retorno de subrutina.
     *
     * Desapila la direccion de retorno, descarta el FrameHeader OOP activo si existe
     * y salta a la direccion de retorno.
     */
    void exec_instr_ret(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * @brief Ejecuta ENTER: crea un marco de pila (prologo de funcion).
     *
     * Secuencia: push RBP -> mov RBP, RSP -> sub RSP, frame_size.
     */
    void exec_instr_enter(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * @brief Ejecuta LEAVE: destruye el marco de pila actual (epilogo de funcion).
     *
     * Secuencia: mov RSP, RBP -> pop RBP.
     */
    void exec_instr_leave(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * @brief Ejecuta JMPR: salto incondicional a la direccion contenida en un registro.
     *
     * Soporta registros generales y especiales segun el campo reg_ext.
     */
    void exec_instr_jmpr(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * @brief Ejecuta CALLVMR: llamada indirecta a subrutina via registro.
     *
     * Empuja ret_addr en la pila y salta al valor del registro indicado.
     */
    void exec_instr_callvmr(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * @brief Ejecuta JREL: salto relativo condicional o incondicional.
     *
     * Mismos codigos de condicion que JMP.
     * Destino = PC + size_instr + disp32 (relativo a la instruccion siguiente).
     * Formato extendido: [0x00][0x2D][cond][pad][disp32] = 8 bytes.
     */
    void exec_instr_jrel(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * @brief Ejecuta MOV inmediato: asignacion de inmediato a registro o memoria.
     *
     * Tres variantes segun direction y _signed_instruct:
     *   1. d=1, s=1 -> escribir en registro especial.
     *   2. d=0, s=0 -> mov reg, imm.
     *   3. d=1, s=0 -> mov [reg], imm.
     */
    void exec_instr_inmed_mov(ProcessVM *vm, const DecodedInstr &instr);

    // =========================================================================
    //  Ejecutores: cursores y acceso a memoria host (0x00 0xC0 .. 0xC2)
    // =========================================================================

    /**
     * @brief Ejecuta READCUR: lee N bytes de la direccion host en curN -> dest_reg.
     *
     * reg1 = registro destino; reg2 = indice del cursor (0-3).
     * Tamano segun mode: 0=byte, 1=word, 2=dword, 3=qword.
     */
    void exec_instr_readcur(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * @brief Ejecuta WRITECUR: escribe N bytes de src_reg en la direccion host en curN.
     *
     * reg1 = registro fuente; reg2 = indice del cursor (0-3).
     * Tamano segun mode: 0=byte, 1=word, 2=dword, 3=qword.
     */
    void exec_instr_writecur(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * @brief Ejecuta GCDEREF: convierte un GcHandle en puntero host y lo almacena en curN.
     *
     * reg1 = registro con el GcHandle; reg2 = indice del cursor destino (0-3).
     */
    void exec_instr_gcderef(ProcessVM *vm, const DecodedInstr &instr);

    // =========================================================================
    //  Ejecutores: GC generacional (0x00 0xA0 .. 0xA5)
    // =========================================================================

    /**
     * @brief Ejecuta NEWOBJ: aloca un objeto gestionado por el GC.
     *
     * reg1 = registro con puntero a ClassInfo.  Inicializa ObjectHeader con GC_OWNED.
     * Devuelve GcHandle en R00; GC_NULL_HANDLE si no hay memoria.
     */
    void exec_instr_newobj(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * @brief Ejecuta GCRUN: fuerza un ciclo de GC menor.
     *
     * Llama a minor_gc(); si old_used >= threshold tambien ejecuta major_gc().
     */
    void exec_instr_gcrun(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * @brief Ejecuta GCCONFIG: ajusta el umbral de promocion a la generacion antigua.
     *
     * reg1 = registro con el nuevo umbral en bytes.
     */
    void exec_instr_gcconfig(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * @brief Ejecuta GCDROP: libera un objeto del heap GC por handle.
     *
     * reg1 = registro con el GcHandle a liberar.
     */
    void exec_instr_gc_drop(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * @brief Ejecuta GCWB (write barrier): notifica una escritura sobre un objeto antiguo.
     *
     * Registra el handle en el remembered set para que el minor GC lo trate como raiz.
     * reg1 = registro con el GcHandle del objeto antiguo modificado.
     */
    void exec_instr_gcwb(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * @brief Ejecuta GCALLOC: aloca un bloque bruto en el heap GC sin ObjectHeader.
     *
     * reg1 = registro con el tamano en bytes.  Devuelve GcHandle en R00.
     */
    void exec_instr_gcalloc(ProcessVM *vm, const DecodedInstr &instr);

    // =========================================================================
    //  Ejecutores: raw allocator (0x00 0xB0 .. 0xB2)
    // =========================================================================

    /**
     * @brief Ejecuta RAWALLOC: aloca memoria bruta no gestionada por el GC.
     *
     * reg1 = tamano en bytes.  Devuelve puntero host en R00 (0 si falla).
     */
    void exec_instr_raw_alloc(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * @brief Ejecuta RAWFREE: libera un bloque de memoria bruta por puntero.
     *
     * reg1 = puntero host al bloque a liberar.
     */
    void exec_instr_raw_free(ProcessVM *vm, const DecodedInstr &instr);

    /**
     * @brief Ejecuta RAWREALLOC: redimensiona un bloque de memoria bruta.
     *
     * reg1 = puntero original; reg2 = nuevo tamano en bytes.
     * Devuelve el nuevo puntero host en R00 (0 si falla).
     */
    void exec_instr_raw_realloc(ProcessVM *vm, const DecodedInstr &instr);

    // =========================================================================
    //  Ejecutores: sistema de objetos OOP (0x00 0xD0 .. 0xEB)
    // =========================================================================

    /** @brief NEWOBJRAW: aloca objeto raw sin GC; inicializa ObjectHeader con RAW_OWNED. */
    void exec_instr_newobjraw(ProcessVM *vm, const DecodedInstr &instr);
    /** @brief CALLVIRT: llamada virtual via vtable; empuja FrameHeader. */
    void exec_instr_callvirt(ProcessVM *vm, const DecodedInstr &instr);
    /** @brief CALLSUPER: llamada al metodo de la superclase via vtable de la clase indicada. */
    void exec_instr_callsuper(ProcessVM *vm, const DecodedInstr &instr);
    /** @brief THROW: lanza una excepcion buscando handler en la cadena de frames. */
    void exec_instr_throw(ProcessVM *vm, const DecodedInstr &instr);
    /** @brief RETHROW: relanza la excepcion activa (current_exception). */
    void exec_instr_rethrow(ProcessVM *vm, const DecodedInstr &instr);
    /** @brief GETCLASS: obtiene el puntero a ClassInfo del ObjectHeader del objeto. */
    void exec_instr_getclass(ProcessVM *vm, const DecodedInstr &instr);
    /** @brief INSTANCEOF: comprueba si el objeto es instancia de la clase indicada. */
    void exec_instr_instanceof(ProcessVM *vm, const DecodedInstr &instr);
    /** @brief CHECKCAST: verifica el tipo; THROW si es incompatible, devuelve ptr si ok. */
    void exec_instr_checkcast(ProcessVM *vm, const DecodedInstr &instr);
    /** @brief GETFIELD: obtiene puntero a FieldInfo por indice en la clase. */
    void exec_instr_getfield(ProcessVM *vm, const DecodedInstr &instr);
    /** @brief GETMETHOD: obtiene puntero a MethodInfo por indice en la clase. */
    void exec_instr_getmethod(ProcessVM *vm, const DecodedInstr &instr);
    /** @brief FIELDCOUNT: devuelve el numero de campos de la clase. */
    void exec_instr_fieldcount(ProcessVM *vm, const DecodedInstr &instr);
    /** @brief METHODCOUNT: devuelve el numero de metodos de la clase. */
    void exec_instr_methodcount(ProcessVM *vm, const DecodedInstr &instr);
    /** @brief CLASSNAME: devuelve puntero host al nombre de la clase. */
    void exec_instr_classname(ProcessVM *vm, const DecodedInstr &instr);

    // --- Reflexion y documentacion ---

    /** @brief CLASSDOC: devuelve puntero al texto de documentacion de la clase. */
    void exec_instr_classdoc(ProcessVM *vm, const DecodedInstr &instr);
    /** @brief CLASSATTRCOUNT: devuelve el numero de atributos de la clase. */
    void exec_instr_classattrcount(ProcessVM *vm, const DecodedInstr &instr);
    /** @brief CLASSATTRKEY: devuelve puntero a la clave del atributo indicado. */
    void exec_instr_classattrkey(ProcessVM *vm, const DecodedInstr &instr);
    /** @brief CLASSATTRVAL: devuelve puntero al valor del atributo indicado. */
    void exec_instr_classattrval(ProcessVM *vm, const DecodedInstr &instr);

    /** @brief METHODNAME: devuelve puntero al nombre del metodo. */
    void exec_instr_methodname(ProcessVM *vm, const DecodedInstr &instr);
    /** @brief METHODDOC: devuelve puntero a la documentacion del metodo. */
    void exec_instr_methoddoc(ProcessVM *vm, const DecodedInstr &instr);
    /** @brief METHODDESC: devuelve puntero al descriptor de tipo del metodo. */
    void exec_instr_methoddesc(ProcessVM *vm, const DecodedInstr &instr);
    /** @brief METHODATTRCOUNT: devuelve el numero de atributos del metodo. */
    void exec_instr_methodattrcount(ProcessVM *vm, const DecodedInstr &instr);
    /** @brief METHODATTRKEY: devuelve puntero a la clave del atributo del metodo. */
    void exec_instr_methodattrkey(ProcessVM *vm, const DecodedInstr &instr);
    /** @brief METHODATTRVAL: devuelve puntero al valor del atributo del metodo. */
    void exec_instr_methodattrval(ProcessVM *vm, const DecodedInstr &instr);

    /** @brief FIELDNAME: devuelve puntero al nombre del campo. */
    void exec_instr_fieldname(ProcessVM *vm, const DecodedInstr &instr);
    /** @brief FIELDDOC: devuelve puntero a la documentacion del campo. */
    void exec_instr_fielddoc(ProcessVM *vm, const DecodedInstr &instr);
    /** @brief FIELDATTRCOUNT: devuelve el numero de atributos del campo. */
    void exec_instr_fieldattrcount(ProcessVM *vm, const DecodedInstr &instr);
    /** @brief FIELDATTRKEY: devuelve puntero a la clave del atributo del campo. */
    void exec_instr_fieldattrkey(ProcessVM *vm, const DecodedInstr &instr);
    /** @brief FIELDATTRVAL: devuelve puntero al valor del atributo del campo. */
    void exec_instr_fieldattrval(ProcessVM *vm, const DecodedInstr &instr);

} // namespace runtime

#endif // EXEC_INSTRUCTION_H
