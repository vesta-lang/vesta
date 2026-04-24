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
 * @file parser_to_bytecode.h
 * @brief Ensamblador de 3 fases: convierte un AST generado por el parser en bytecode ejecutable.
 *
 * Contiene:
 *  - @c PseudoInstructions : conjunto de directivas de preprocesado (global, extern, bits, align, org).
 *  - @c InstrTable         : tabla completa de instrucciones con sus variantes de codificacion.
 *  - @c Assembler          : clase principal del ensamblador (3 fases).
 *  - @c resolve_imports()  : funcion estatica que expande recursivamente los nodos @c ImportNode.
 *
 * Proceso de ensamblado (3 fases):
 *  1. Primera pasada  (@c first_pass)  : recoleccion de simbolos, labels y calculo de offsets.
 *  2. Segunda pasada  (@c emit_pass)   : emision de datos y evaluacion de expresiones.
 *  3. Tercera pasada  (fusionada en 2) : emision de instrucciones y resolucion de saltos.
 */

#ifndef PARSER_TO_BYTECODE_H
#define PARSER_TO_BYTECODE_H

#include <filesystem>

#include "emmit_decl.h"
#include "annotations.h"

namespace Assembly::Bytecode {

    /**
     * @brief Conjunto de pseudo-instrucciones (directivas de preprocesado).
     *
     * Estas directivas deben aparecer siempre al inicio del programa, antes de
     * cualquier instruccion ejecutable.  El ensamblador las procesa en la primera
     * pasada y no generan bytecode directo.
     *
     * Directivas soportadas:
     *  - @c global : exporta un simbolo al scope global.
     *  - @c extern : declara un simbolo externo (importado).
     *  - @c bits   : establece el ancho de los operandos (8, 16, 32, 64).
     *  - @c align  : alinea el offset actual a un multiplo de N bytes.
     *  - @c org    : fija la direccion base del ensamblado.
     */
    static const std::unordered_set<std::string> PseudoInstructions = {
        "global",
        "extern",
        "bits",
        "align",
        "org"
    };

    /**
     * @brief Tabla de instrucciones: mapea cada mnemotecnico a sus variantes de codificacion.
     *
     * Cada entrada asocia un nombre de instruccion con un vector de @c InstrInfo.
     * Cuando una instruccion tiene multiples variantes (por ejemplo, ADD con reg+reg,
     * reg+inmediato o reg+SIB), el ensamblador llama a @c select_variant() para elegir
     * la correcta segun los operandos del nodo AST.
     *
     * Grupos de instrucciones:
     *  - Informacion de VM      : vminfo, vminfomanager.
     *  - Aritmetica con/sin signo: adds/addu, subs/subu, muls/mulu, divs/divu, cmps/cmpu.
     *  - Transferencia          : mov (reg/inmed/SIB), movc, movch.
     *  - Logica                 : and, or, xor, not, shl, shr, sar.
     *  - Saltos absolutos       : jmp (incondicional + 14 condicionales), jmpr.
     *  - Saltos relativos       : jrel (incondicional + 14 condicionales).
     *  - Pila                   : push, pop, xchg.
     *  - Llamadas               : callvm, callvmr, calln, callnr.
     *  - Control de flujo       : enter, leave, ret, loop.
     *  - GC generacional        : newobj, gcrun, gcconfig, drop, gcwb, gcalloc.
     *  - Asignador raw          : alloc, free, realloc.
     *  - Cursores               : readcur, writecur, gcderef.
     *  - OOP                    : newobjraw, callvirt, callsuper, throw, rethrow,
     *                             getclass, instanceof, checkcast, getfield, getmethod,
     *                             fieldcount, methodcount, classname.
     *  - Reflexion/doc OOP      : classdoc, classattrcount, classattrkey, classattrval,
     *                             methodname, methoddoc, methoddesc, methodattrcount,
     *                             methodattrkey, methodattrval, fieldname, fielddoc,
     *                             fieldattrcount, fieldattrkey, fieldattrval.
     *  - NOP                    : nop1 (1 byte), nop2 (2 bytes).
     *  - Protocolo distribuido  : edmw4, edmw6, edm, hlt.
     *
     * Formato de cada variante (@c InstrInfo):
     * @code
     * { opcode1, opcode2, InstrSizeMode, AddressingMode, emit_fn }
     * @endcode
     * Si opcode1 == 0x00 la instruccion es extendida (2 bytes de opcode).
     */
    static const std::unordered_map<std::string, std::vector<InstrInfo> > InstrTable = {
        /* --- Informacion de VM --- */
        {"vminfo",       {{0x01, 0x00, InstrSizeMode::FIXED_2,  AddressingMode::NONE,  nullptr}}},
        {"vminfomanager",{{0x02, 0x00, InstrSizeMode::FIXED_2,  AddressingMode::NONE,  nullptr}}},

        /* INC y DEC usan la misma subrutina de emision porque se codifican igual,
           cambiando solo el segundo byte. */
        {"inc", {{0x04, 0x00, InstrSizeMode::FIXED_2, AddressingMode::REG, emit_inc_dec}}},
        {"dec", {{0x04, 0x00, InstrSizeMode::FIXED_2, AddressingMode::REG, emit_inc_dec}}},

        /* callvm <label|addr>  - llama a funcion interna empujando retorno en pila. */
        {"callvm",  {{0x10, 0x00, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        /* callvmr <reg>        - igual que callvm pero la direccion viene de un registro. */
        {"callvmr", {{0x16, 0x00, InstrSizeMode::FIXED_2,  AddressingMode::REG,   emit_pop_push}}},

        /* --- Saltos absolutos (opcode1 = 0x11) --- */
        /* jmp incondicional: opcode2 = 0x0F */
        {"jmp",     {{0x11, 0x0F, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        /* jmp condicionales: sufijo determina la condicion, opcode2 = codigo de condicion. */
        {"jmp.je",  {{0x11, 0x00, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        {"jmp.jz",  {{0x11, 0x00, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        {"jmp.jne", {{0x11, 0x01, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        {"jmp.jnz", {{0x11, 0x01, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        {"jmp.jcs", {{0x11, 0x02, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        {"jmp.jae", {{0x11, 0x02, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        {"jmp.jcc", {{0x11, 0x03, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        {"jmp.jb",  {{0x11, 0x03, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        {"jmp.jmi", {{0x11, 0x04, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        {"jmp.jpl", {{0x11, 0x05, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        {"jmp.jvs", {{0x11, 0x06, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        {"jmp.jvc", {{0x11, 0x07, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        {"jmp.jhi", {{0x11, 0x08, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        {"jmp.jls", {{0x11, 0x09, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        {"jmp.jge", {{0x11, 0x0A, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        {"jmp.jlt", {{0x11, 0x0B, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        {"jmp.jgt", {{0x11, 0x0C, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        {"jmp.jle", {{0x11, 0x0D, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},

        /* jmpr <reg>  - salto incondicional por registro. */
        {"jmpr",    {{0x15, 0x00, InstrSizeMode::FIXED_2,  AddressingMode::REG,   emit_pop_push}}},

        /* --- Saltos relativos con desplazamiento de 32 bits (opcode extendido 0x00 0x2D) --- */
        {"jrel",     {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},
        {"jrel.je",  {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},
        {"jrel.jz",  {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},
        {"jrel.jne", {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},
        {"jrel.jnz", {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},
        {"jrel.jcs", {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},
        {"jrel.jae", {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},
        {"jrel.jcc", {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},
        {"jrel.jb",  {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},
        {"jrel.jmi", {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},
        {"jrel.jpl", {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},
        {"jrel.jvs", {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},
        {"jrel.jvc", {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},
        {"jrel.jhi", {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},
        {"jrel.jls", {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},
        {"jrel.jge", {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},
        {"jrel.jlt", {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},
        {"jrel.jgt", {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},
        {"jrel.jle", {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},

        /* enter <frame_size> - crea stack frame reservando N bytes para variables locales. */
        {"enter", {{0x28, 0x00, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        /* leave - destruye el frame actual (sin operandos). */
        {"leave", {{0x29, 0x00, InstrSizeMode::FIXED_1,  AddressingMode::NONE,  nullptr}}},

        /* --- Pila --- */
        {"push", {{0x12, 0x00, InstrSizeMode::FIXED_2, AddressingMode::REG, emit_pop_push}}},
        {"pop",  {{0x13, 0x00, InstrSizeMode::FIXED_2, AddressingMode::REG, emit_pop_push}}},
        {"xchg", {{0x14, 0x00, InstrSizeMode::FIXED_4, AddressingMode::REG, emit_xchg}}},

        /* --- GC generacional (opcode extendido 0x00 0xA0..0xA5) --- */
        {"newobj",   {{0x00, 0xA0, InstrSizeMode::FIXED_4, AddressingMode::REG,  emit_instr_one_reg}}},
        {"gcrun",    {{0x00, 0xA1, InstrSizeMode::FIXED_2, AddressingMode::NONE, nullptr}}},
        {"gcconfig", {{0x00, 0xA2, InstrSizeMode::FIXED_4, AddressingMode::REG,  emit_instr_one_reg}}},
        {"drop",     {{0x00, 0xA3, InstrSizeMode::FIXED_4, AddressingMode::REG,  emit_instr_one_reg}}},
        {"gcwb",     {{0x00, 0xA4, InstrSizeMode::FIXED_4, AddressingMode::REG,  emit_instr_one_reg}}},
        {"gcalloc",  {{0x00, 0xA5, InstrSizeMode::FIXED_4, AddressingMode::REG,  emit_instr_one_reg}}},

        /* --- Asignador raw (opcode extendido 0x00 0xB0..0xB2) --- */
        {"alloc",   {{0x00, 0xB0, InstrSizeMode::FIXED_4, AddressingMode::REG,  emit_instr_one_reg}}},
        {"free",    {{0x00, 0xB1, InstrSizeMode::FIXED_4, AddressingMode::REG,  emit_instr_one_reg}}},
        {"realloc", {{0x00, 0xB2, InstrSizeMode::FIXED_4, AddressingMode::REG,  emit_instr_reg}}},

        /* --- Cursores: acceso a memoria real (opcode extendido 0x00 0xC0..0xC2) --- */
        {"readcur",  {{0x00, 0xC0, InstrSizeMode::FIXED_4, AddressingMode::REG, emit_cursor_rw}}},
        {"writecur", {{0x00, 0xC1, InstrSizeMode::FIXED_4, AddressingMode::REG, emit_cursor_rw}}},
        {"gcderef",  {{0x00, 0xC2, InstrSizeMode::FIXED_4, AddressingMode::REG, emit_gcderef}}},

        /* --- OOP: sistema de objetos (opcode extendido 0x00 0xD0..0xDC) --- */
        {"newobjraw",  {{0x00, 0xD0, InstrSizeMode::FIXED_4, AddressingMode::REG,  emit_instr_reg}}},
        {"callvirt",   {{0x00, 0xD1, InstrSizeMode::FIXED_4, AddressingMode::INMED, emit_instr_reg_imm8}}},
        {"callsuper",  {{0x00, 0xD2, InstrSizeMode::FIXED_4, AddressingMode::INMED, emit_instr_reg_imm8}}},
        {"throw",      {{0x00, 0xD3, InstrSizeMode::FIXED_4, AddressingMode::REG,  emit_instr_one_reg}}},
        {"rethrow",    {{0x00, 0xD4, InstrSizeMode::FIXED_2, AddressingMode::NONE, nullptr}}},
        {"getclass",   {{0x00, 0xD5, InstrSizeMode::FIXED_4, AddressingMode::REG,  emit_instr_one_reg}}},
        {"instanceof", {{0x00, 0xD6, InstrSizeMode::FIXED_4, AddressingMode::REG,  emit_instr_reg}}},
        {"checkcast",  {{0x00, 0xD7, InstrSizeMode::FIXED_4, AddressingMode::REG,  emit_instr_reg}}},
        {"getfield",   {{0x00, 0xD8, InstrSizeMode::FIXED_4, AddressingMode::INMED, emit_instr_reg_imm8}}},
        {"getmethod",  {{0x00, 0xD9, InstrSizeMode::FIXED_4, AddressingMode::INMED, emit_instr_reg_imm8}}},
        {"fieldcount", {{0x00, 0xDA, InstrSizeMode::FIXED_4, AddressingMode::REG,  emit_instr_one_reg}}},
        {"methodcount",{{0x00, 0xDB, InstrSizeMode::FIXED_4, AddressingMode::REG,  emit_instr_one_reg}}},
        {"classname",  {{0x00, 0xDC, InstrSizeMode::FIXED_4, AddressingMode::REG,  emit_instr_one_reg}}},

        /* --- Reflexion/documentacion OOP (opcode extendido 0x00 0xDD..0xEB) --- */
        {"classdoc",        {{0x00, 0xDD, InstrSizeMode::FIXED_4, AddressingMode::REG,   emit_instr_one_reg}}},
        {"classattrcount",  {{0x00, 0xDE, InstrSizeMode::FIXED_4, AddressingMode::REG,   emit_instr_one_reg}}},
        {"classattrkey",    {{0x00, 0xDF, InstrSizeMode::FIXED_4, AddressingMode::INMED, emit_instr_reg_imm8}}},
        {"classattrval",    {{0x00, 0xE0, InstrSizeMode::FIXED_4, AddressingMode::INMED, emit_instr_reg_imm8}}},
        {"methodname",      {{0x00, 0xE1, InstrSizeMode::FIXED_4, AddressingMode::REG,   emit_instr_one_reg}}},
        {"methoddoc",       {{0x00, 0xE2, InstrSizeMode::FIXED_4, AddressingMode::REG,   emit_instr_one_reg}}},
        {"methoddesc",      {{0x00, 0xE3, InstrSizeMode::FIXED_4, AddressingMode::REG,   emit_instr_one_reg}}},
        {"methodattrcount", {{0x00, 0xE4, InstrSizeMode::FIXED_4, AddressingMode::REG,   emit_instr_one_reg}}},
        {"methodattrkey",   {{0x00, 0xE5, InstrSizeMode::FIXED_4, AddressingMode::INMED, emit_instr_reg_imm8}}},
        {"methodattrval",   {{0x00, 0xE6, InstrSizeMode::FIXED_4, AddressingMode::INMED, emit_instr_reg_imm8}}},
        {"fieldname",       {{0x00, 0xE7, InstrSizeMode::FIXED_4, AddressingMode::REG,   emit_instr_one_reg}}},
        {"fielddoc",        {{0x00, 0xE8, InstrSizeMode::FIXED_4, AddressingMode::REG,   emit_instr_one_reg}}},
        {"fieldattrcount",  {{0x00, 0xE9, InstrSizeMode::FIXED_4, AddressingMode::REG,   emit_instr_one_reg}}},
        {"fieldattrkey",    {{0x00, 0xEA, InstrSizeMode::FIXED_4, AddressingMode::INMED, emit_instr_reg_imm8}}},
        {"fieldattrval",    {{0x00, 0xEB, InstrSizeMode::FIXED_4, AddressingMode::INMED, emit_instr_reg_imm8}}},

        /* --- NOP --- */
        {"nop1", {{0x33, 0x00, InstrSizeMode::FIXED_1, AddressingMode::NONE, nullptr}}},
        {"nop2", {{0x00, 0x33, InstrSizeMode::FIXED_2, AddressingMode::NONE, nullptr}}},

        /* callnr <reg>  - llamada nativa por registro (sin retorno). */
        {"callnr", {{0x55, 0x00, InstrSizeMode::FIXED_1, AddressingMode::REG, nullptr}}},

        /* ret - retorna de una subrutina. */
        {"ret", {{0xC3, 0x00, InstrSizeMode::FIXED_1, AddressingMode::NONE, nullptr}}},

        /* --- Protocolo distribuido (opcode extendido 0x00 0x00..0x03) --- */
        {"edmw4", {{0x00, 0x00, InstrSizeMode::FIXED_4, AddressingMode::NONE, nullptr}}},
        {"edmw6", {{0x00, 0x01, InstrSizeMode::FIXED_4, AddressingMode::NONE, nullptr}}},
        {"edm",   {{0x00, 0x02, InstrSizeMode::FIXED_2, AddressingMode::NONE, nullptr}}},
        {"hlt",   {{0x00, 0x03, InstrSizeMode::FIXED_2, AddressingMode::NONE, nullptr}}},

        /*
         * Instrucciones aritmeticas con multiples variantes.
         * Ejemplo de busqueda de variante:
         *
         *   auto& variants = InstrTable["adds"];
         *   for (auto& v : variants) {
         *       if (matches_operands(v.sizeMode, operands))
         *           return v;
         *   }
         *
         * Cada grupo expone 3 variantes:
         *   1. reg, reg    -> FIXED_4 / REG
         *   2. reg, [mem]  -> MIXED_SIZE / INMED
         *   3. reg, SIB    -> FIXED_6 / SIB
         */

        /* --- ADDS / ADDU (suma con/sin signo) --- */
        {
            "adds", {
                {0x00, 0x05, InstrSizeMode::FIXED_4,   AddressingMode::REG,   emit_instr_reg},
                {0x00, 0x06, InstrSizeMode::MIXED_SIZE, AddressingMode::INMED, emit_instr_inmed},
                {0x00, 0x07, InstrSizeMode::FIXED_6,   AddressingMode::SIB,   emit_instr_sib}
            }
        },
        {
            "addu", {
                {0x00, 0x05, InstrSizeMode::FIXED_4,   AddressingMode::REG,   emit_instr_reg},
                {0x00, 0x06, InstrSizeMode::MIXED_SIZE, AddressingMode::INMED, emit_instr_inmed},
                {0x00, 0x07, InstrSizeMode::FIXED_6,   AddressingMode::SIB,   emit_instr_sib}
            }
        },

        /* --- SUBS / SUBU (resta con/sin signo) --- */
        {
            "subu", {
                {0x00, 0x08, InstrSizeMode::FIXED_4,   AddressingMode::REG,   emit_instr_reg},
                {0x00, 0x09, InstrSizeMode::MIXED_SIZE, AddressingMode::INMED, emit_instr_inmed},
                {0x00, 0x0A, InstrSizeMode::FIXED_6,   AddressingMode::SIB,   emit_instr_sib}
            }
        },
        {
            "subs", {
                {0x00, 0x08, InstrSizeMode::FIXED_4,   AddressingMode::REG,   emit_instr_reg},
                {0x00, 0x09, InstrSizeMode::MIXED_SIZE, AddressingMode::INMED, emit_instr_inmed},
                {0x00, 0x0A, InstrSizeMode::FIXED_6,   AddressingMode::SIB,   emit_instr_sib}
            }
        },

        /* --- MULS / MULU (multiplicacion con/sin signo) --- */
        {
            "muls", {
                {0x00, 0x0B, InstrSizeMode::FIXED_4,   AddressingMode::REG,   emit_instr_reg},
                {0x00, 0x0C, InstrSizeMode::MIXED_SIZE, AddressingMode::INMED, emit_instr_inmed},
                {0x00, 0x0D, InstrSizeMode::FIXED_6,   AddressingMode::SIB,   emit_instr_sib}
            }
        },
        {
            "mulu", {
                {0x00, 0x0B, InstrSizeMode::FIXED_4,   AddressingMode::REG,   emit_instr_reg},
                {0x00, 0x0C, InstrSizeMode::MIXED_SIZE, AddressingMode::INMED, emit_instr_inmed},
                {0x00, 0x0D, InstrSizeMode::FIXED_6,   AddressingMode::SIB,   emit_instr_sib}
            }
        },

        /* --- DIVS / DIVU (division con/sin signo) --- */
        {
            "divu", {
                {0x00, 0x0E, InstrSizeMode::FIXED_4,   AddressingMode::REG,   emit_instr_reg},
                {0x00, 0x0F, InstrSizeMode::MIXED_SIZE, AddressingMode::INMED, emit_instr_inmed},
                {0x00, 0x10, InstrSizeMode::FIXED_6,   AddressingMode::SIB,   emit_instr_sib}
            }
        },
        {
            "divs", {
                {0x00, 0x0E, InstrSizeMode::FIXED_4,   AddressingMode::REG,   emit_instr_reg},
                {0x00, 0x0F, InstrSizeMode::MIXED_SIZE, AddressingMode::INMED, emit_instr_inmed},
                {0x00, 0x10, InstrSizeMode::FIXED_6,   AddressingMode::SIB,   emit_instr_sib}
            }
        },

        /* --- CMPS / CMPU (comparacion con/sin signo) --- */
        {
            "cmpu", {
                {0x00, 0x11, InstrSizeMode::FIXED_4,   AddressingMode::REG,   emit_instr_reg},
                {0x00, 0x12, InstrSizeMode::MIXED_SIZE, AddressingMode::INMED, emit_instr_inmed},
                {0x00, 0x13, InstrSizeMode::FIXED_6,   AddressingMode::SIB,   emit_instr_sib},
            },
        },
        {
            "cmps", {
                {0x00, 0x11, InstrSizeMode::FIXED_4,   AddressingMode::REG,   emit_instr_reg},
                {0x00, 0x12, InstrSizeMode::MIXED_SIZE, AddressingMode::INMED, emit_instr_inmed},
                {0x00, 0x13, InstrSizeMode::FIXED_6,   AddressingMode::SIB,   emit_instr_sib},
            },
        },

        /* --- MOV: transferencia de datos --- */
        {
            "mov", {
                {0x00, 0x14, InstrSizeMode::FIXED_4,   AddressingMode::REG,   emit_instr_mov_reg},
                {0x00, 0x15, InstrSizeMode::MIXED_SIZE, AddressingMode::INMED, emit_instr_mov_inmed},
                {0x00, 0x16, InstrSizeMode::FIXED_6,   AddressingMode::SIB,   emit_instr_mov_sib},
            },
        },

        /* --- MOVC / MOVCH: movimiento con bandera de modo (VM mem / host mem) --- */
        {
            "movc", {
                /* movc reg1, [reg2], flag  ||  movc [reg1], reg2, flag -> acceso memoria VM */
                {0x00, 0x1E, InstrSizeMode::FIXED_4, AddressingMode::MEM, emit_instr_movc},
                /* movc reg1, reg2, flag -> modo registro a registro */
                {0x00, 0x1F, InstrSizeMode::FIXED_4, AddressingMode::REG, emit_instr_movc},
            }
        },
        {
            "movch", {
                /* movch reg1, [reg2], flag  ||  movch [reg1], reg2, flag -> acceso memoria host */
                {0x00, 0x1E, InstrSizeMode::FIXED_4, AddressingMode::MEM, emit_instr_movc},
            }
        },

        /* --- Logica bit a bit --- */
        {"and", {{0x00, 0x17, InstrSizeMode::FIXED_4, AddressingMode::REG, emit_instr_reg}}},
        {"or",  {{0x00, 0x18, InstrSizeMode::FIXED_4, AddressingMode::REG, emit_instr_reg}}},
        {"xor", {{0x00, 0x19, InstrSizeMode::FIXED_4, AddressingMode::REG, emit_instr_reg}}},
        {"not", {{0x00, 0x1A, InstrSizeMode::FIXED_4, AddressingMode::REG, emit_instr_one_reg}}},
        {"shl", {{0x00, 0x1B, InstrSizeMode::FIXED_4, AddressingMode::REG, emit_instr_reg}}},
        {"shr", {{0x00, 0x1C, InstrSizeMode::FIXED_4, AddressingMode::REG, emit_instr_reg}}},
        {"sar", {{0x00, 0x1D, InstrSizeMode::FIXED_4, AddressingMode::REG, emit_instr_reg}}},

        /* loop <label>  - decrementa contador y salta si != 0. */
        {
            "loop", {
                {0x00, 0x31, InstrSizeMode::FIXED_8, AddressingMode::INMED, nullptr},
            },
        },

        /* calln <addr>  - llamada a funcion nativa por direccion inmediata. */
        {
            "calln", {
                {0x00, 0x55, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_calln_inmmed},
            },
        },
    };

    /**
     * @class Assembler
     * @brief Convierte un AST generado por el parser en bytecode ejecutable para la VM.
     *
     * El ensamblado se realiza en 3 fases conceptuales (la 2.a y 3.a estan fusionadas):
     *  1. Primera pasada  (@c first_pass)  : recorre el AST para registrar todos los
     *     simbolos (labels, secciones, directivas @e global / @e extern) y calcular
     *     sus offsets definitivos.  Sin esta fase no es posible resolver saltos forward.
     *  2. Segunda+tercera pasada (@c emit_pass) : emite datos (directivas @e db/dw/dd/dq)
     *     e instrucciones en el mismo recorrido, ahora que los simbolos ya tienen
     *     direccion conocida.
     *
     * Las relocalizaciones pendientes (saltos a simbolos de otros espacios) se
     * registran en @c ctx y se resuelven durante el enlazado posterior.
     *
     * @note El ensamblador genera un unico buffer de bytecode secuencial.  El linker
     *       es responsable de combinar multiples buffers en un ejecutable final.
     */
    class Assembler {
    public:
        /// Tabla de simbolos construida en la primera pasada: nombre -> puntero a Label.
        std::unordered_map<std::string, Label *> symbol_table;

        /// Buffer de salida donde se escribe el bytecode final byte a byte.
        ByteWriter output;

        /// Contexto global del ensamblador: espacios, secciones, labels y relocalizaciones.
        Context ctx{};

        /// Seccion actualmente activa; cambia al procesar directivas @e @Section.
        Section *current_section = nullptr;

        /// Label actualmente analizada; se usa como cursor interno durante la emision.
        Label *current_label = nullptr;

        /**
         * @brief Constructor del ensamblador.
         *
         * Inicializa las estructuras internas, vacia el buffer de salida y
         * establece la direccion base por defecto (0x0000).
         */
        Assembler();

        /**
         * @brief Calcula el tamano final de cada label tras la primera pasada.
         *
         * Debe llamarse despues de @c first_pass y antes de @c emit_pass para
         * que cada label conozca cuantos bytes ocupa en el bytecode.
         */
        void compute_label_sizes();

        /**
         * @brief Ensambla un AST completo y devuelve el bytecode resultante.
         *
         * Ejecuta las 3 fases en orden:
         *  1. @c first_pass  sobre cada nodo raiz.
         *  2. @c compute_label_sizes.
         *  3. @c emit_pass sobre cada nodo raiz.
         *
         * @param ast Lista de nodos raiz del AST generado por el parser.
         * @return Vector de bytes con el bytecode listo para enlazar o ejecutar.
         */
        std::vector<uint8_t> assemble(const std::vector<std::unique_ptr<vm::ASTNode> > &ast);

        /**
         * @brief Segunda+tercera pasada: emite datos e instrucciones en el buffer.
         *
         * La segunda y tercera fase estan fusionadas porque, una vez que todos los
         * simbolos tienen offsets definitivos (tras @c first_pass), datos e
         * instrucciones pueden emitirse en un unico recorrido.
         *
         * @param node Nodo actual del AST a procesar.
         */
        void emit_pass(const vm::ASTNode *node);

        /**
         * @brief Devuelve el tamano en bytes de una directiva de datos.
         *
         * @param dir Nombre de la directiva: "db" (1), "dw" (2), "dd" (4), "dq" (8), "ptr" (8).
         * @return Numero de bytes que ocupa un elemento de esa directiva.
         */
        size_t size_of_directive(const std::string &dir) const;

        /**
         * @brief Emite un valor numerico segun la directiva indicada.
         *
         * El valor se escribe en formato little-endian en el buffer de salida
         * y se avanza el offset interno del ensamblador.
         *
         * @param dir   Directiva de datos ("db", "dw", "dd", "dq", "ptr").
         * @param value Valor numerico ya evaluado que se escribira.
         */
        void emit_directive(const std::string &dir, uint64_t value);

        /**
         * @brief Emite los datos de un nodo @c DataDecl del AST.
         *
         * Las cadenas de texto se emiten caracter a caracter (sin nulo terminal
         * a menos que sea explicito).  Las expresiones numericas se evaluan con
         * @c eval_expr y se pasan a @c emit_directive.
         *
         * @param data Nodo @c DataDecl del AST con la lista de valores a emitir.
         */
        void emit_data(const vm::DataDecl *data);

        /**
         * @brief Selecciona la variante correcta de una instruccion segun sus operandos.
         *
         * Varias instrucciones (mov, add, sub...) tienen multiples codificaciones.
         * Esta funcion recorre el vector de variantes en @c InstrTable y elige la que
         * coincide con el modo de direccionamiento de los operandos proporcionados.
         *
         * @param mnemonic Nombre de la instruccion (p.ej. "adds", "mov", "jmp").
         * @param ops      Lista de operandos del nodo instruccion en el AST.
         * @return Referencia constante a la @c InstrInfo de la variante seleccionada.
         * @throws std::runtime_error si no existe ninguna variante compatible.
         */
        const InstrInfo &select_variant(const std::string &                               mnemonic,
                                        const std::vector<std::unique_ptr<vm::ASTNode> > &ops) const;

        /**
         * @brief Emite una instruccion completa al buffer de salida.
         *
         * Escribe opcode1, luego opcode2 si la instruccion es extendida (opcode1==0x00),
         * y a continuacion los operandos segun el formato definido en @c InstrInfo.
         *
         * @param instr Nodo @c Instruction del AST con mnemotecnico y operandos.
         */
        void emit_instruction(const vm::Instruction *instr);

        /**
         * @brief Evalua un operando del AST y devuelve su valor numerico de 64 bits.
         *
         * Resuelve registros, inmediatos, referencias a labels y expresiones
         * compuestas delegando en @c eval_expr cuando el operando es un @c ExprNode.
         *
         * @param op Nodo operando del AST.
         * @return Valor numerico de 64 bits sin signo del operando evaluado.
         */
        uint64_t eval_operand(const vm::ASTNode *op);

        /**
         * @brief Evalua una expresion del AST y devuelve su valor numerico.
         *
         * Soporta:
         *  - Literales numericos enteros y hexadecimales.
         *  - Referencias a labels (se sustituyen por su offset).
         *  - Expresiones binarias con operadores: +, -, *, /.
         *
         * @param expr Nodo @c ExprNode del AST que representa la expresion.
         * @return Valor numerico de 64 bits resultado de la evaluacion.
         * @throws std::runtime_error si la expresion contiene un operador no soportado.
         */
        uint64_t eval_expr(vm::ExprNode *expr);

        /**
         * @brief Primera pasada: recoleccion de simbolos y calculo de offsets.
         *
         * Recorre el AST y registra en @c ctx todas las labels, secciones y
         * espacios, asignando a cada uno su offset definitivo en el bytecode.
         * Tambien procesa directivas pseudo-instruccion (@e global, @e extern, etc.).
         *
         * @param node   Nodo actual del AST a analizar.
         * @param offset Offset acumulado (se actualiza en cada llamada recursiva).
         */
        void first_pass(const vm::ASTNode *node, uint64_t &offset);

        /**
         * @brief Aplica una anotacion del AST al contexto del ensamblador.
         *
         * Despacha el procesado de la anotacion a traves de @c annotation_handlers,
         * que modifica @c ctx segun la directiva anotacion (@e @SpaceAddress,
         * @e @Section, @e @Format, @e @InitPc, @e @Import...).
         *
         * @param annotation Nodo @c AnnotationNode del AST con nombre y argumentos.
         */
        void apply_annotation(const vm::AnnotationNode *annotation);

        /**
         * @brief Procesa una directiva pseudo-instruccion (global, extern, bits...).
         *
         * Interpreta el nodo instruccion como una directiva de control del ensamblador
         * en lugar de una instruccion ejecutable y actualiza el estado interno del
         * ensamblador en consecuencia.
         *
         * @param instr Nodo @c Instruction del AST cuyo mnemotecnico esta en
         *              @c PseudoInstructions.
         */
        void apply_directive(const vm::Instruction *instr);

    private:
    };

    /**
     * @brief Expande recursivamente los nodos @c ImportNode de un AST, sustituyendolos
     *        por los nodos del archivo importado.
     *
     * Algoritmo (solo movimientos, sin copias ni duplicacion de nodos):
     *  1. Recorre @p ast nodo a nodo.
     *  2. Si encuentra un @c ImportNode cuyo archivo no ha sido procesado todavia:
     *     a. Lee el archivo fuente.
     *     b. Lo lexifica y parsea obteniendo un nuevo AST.
     *     c. Llama a @c resolve_imports recursivamente sobre el AST importado
     *        (para resolver imports anidados).
     *     d. Inserta todos los nodos resultantes en el AST final.
     *  3. Si el archivo ya fue importado (@p imported contiene su ruta), lo ignora
     *     para evitar inclusiones multiples e importaciones ciclicas.
     *  4. Los nodos que no son @c ImportNode se copian directamente al resultado.
     *  5. Al terminar, @p ast es reemplazado por el vector resultado fusionado.
     *
     * Ejemplo:
     * @verbatim
     *   AST original : [A, B, import C, D]
     *   AST de C     : [X, Y, Z]
     *   AST final    : [A, B, X, Y, Z, D]
     * @endverbatim
     *
     * @warning Si algun nodo guarda punteros laterales a otros nodos del mismo AST
     *          (no solo a sus hijos), esas referencias quedaran invalidadas tras el
     *          movimiento.  En un AST puramente jerarquico esto no supone ningun problema.
     *
     * @param ast      AST original que puede contener nodos @c ImportNode; se modifica
     *                 in-place reemplazando su contenido por el AST expandido.
     * @param imported Conjunto de rutas de archivo ya procesadas; se actualiza durante
     *                 la recursion para evitar importaciones duplicadas.
     */
    /**
     * @brief Expande recursivamente los nodos ImportNode de un AST.
     *
     * Orden de busqueda para cada import relativo:
     *  1. @p base_dir : directorio del archivo fuente que contiene el import.
     *  2. @p search_paths : lista adicional de directorios (CWD, dir del ejecutable, etc.).
     *  3. La ruta tal cual (si es absoluta o el CWD ya la contiene).
     *
     * @param ast          AST a expandir; se modifica en el lugar.
     * @param imported     Rutas ya procesadas (evita reimportaciones y ciclos).
     * @param base_dir     Directorio del archivo que contiene este AST.
     * @param search_paths Directorios adicionales donde buscar los imports.
     */
    static void resolve_imports(
        std::vector<std::unique_ptr<vm::ASTNode> > &ast,
        std::unordered_set<std::string> &           imported,
        const std::string &                         base_dir    = "",
        const std::vector<std::string> &            search_paths = {}
    ) {
        std::vector<std::unique_ptr<vm::ASTNode> > result;

        for (auto &node: ast) {
            if (auto imp = dynamic_cast<vm::ImportNode *>(node.get())) {
                const std::string &raw = imp->filename;

                // resolver la ruta del archivo importado con orden de busqueda:
                std::string resolved;
                auto try_path = [&](const std::string &dir) {
                    if (!resolved.empty()) return;
                    std::filesystem::path p =
                        (std::filesystem::path(dir) / raw).lexically_normal();
                    if (std::filesystem::exists(p)) resolved = p.string();
                };

                // 1. directorio del archivo fuente actual
                if (!base_dir.empty()) try_path(base_dir);
                // 2. rutas de busqueda adicionales (CWD, dir del ejecutable, etc.)
                for (auto &sp : search_paths) try_path(sp);
                // 3. la ruta tal cual (absoluta o relativa al CWD vigente)
                if (resolved.empty() && std::filesystem::exists(raw)) resolved = raw;

                if (resolved.empty()) {
                    std::string searched = base_dir.empty() ? "." : base_dir;
                    for (auto &sp : search_paths) searched += ", " + sp;
                    throw std::runtime_error(
                        "No se pudo encontrar el archivo importado: '" + raw +
                        "'\n  Directorios buscados: " + searched
                    );
                }

                /* evitar importaciones multiples del mismo archivo */
                if (imported.find(resolved) != imported.end())
                    continue;
                imported.insert(resolved);

                /* leer archivo fuente importado */
                std::ifstream f(resolved);
                if (!f.is_open()) {
                    throw std::runtime_error(
                        "No se pudo abrir el archivo importado: " + resolved);
                }
                std::string code((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());

                /* lex + parse del codigo importado */
                vm::Lexer  lx(code);
                vm::Parser px(lx);
                auto       imported_ast = px.parse();

                /* directorio del archivo importado como base para sus propios imports */
                std::string imported_base =
                    std::filesystem::path(resolved).parent_path().string();

                /* expandir imports anidados con la misma lista de search_paths */
                resolve_imports(imported_ast, imported, imported_base, search_paths);

                /* insertar nodos del archivo importado en el resultado */
                for (auto &n: imported_ast)
                    result.push_back(std::move(n));
            } else {
                /* nodo normal: copiar directamente al resultado */
                result.push_back(std::move(node));
            }
        }

        /* sustituir el AST original por el resultado expandido */
        ast = std::move(result);
    }

} // namespace Assembly::Bytecode

#endif // PARSER_TO_BYTECODE_H
