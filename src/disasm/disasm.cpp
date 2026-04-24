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
 * @file disasm.cpp
 * @brief Implementacion del desensamblador de bytecode VestaVM.
 *
 * Implementa las tres funciones publicas del modulo disasm:
 *   - disasm_velb():   carga un .velb mediante el Loader y desensambla desde el entrypoint.
 *   - disasm_bytes():  desensambla un buffer de bytes en memoria sin necesitar la VM.
 *   - print_instruction(): formatea e imprime una instruccion decodificada con colores opcionales.
 *
 * Codificaciones soportadas:
 *   - Tabla primaria (1 byte): inc/dec (0x04), callvm (0x10), jmp (0x11), push (0x12),
 *     pop (0x13), xchg (0x14), jmpr (0x15), callvmr (0x16).
 *   - Tabla extendida (prefijo 0x00): MOV reg-reg (0x14), MOV imm (0x15), MOV SIB (0x16),
 *     ALU con tres modos (0x05-0x13), logica (0x17-0x1D), MOVC/MOVCH (0x1E-0x1F), HLT (0x03),
 *     corutinas yield/resume/spawn/swapctx (0xEC-0xEF),
 *     flotante ZMM: fmow/fmov/fadd/fsub/fmul/fdiv/fcmp/fsqrt/fabs/fneg/fcvt/fmowi/fload/fstore
 *     (0xF0-0xFC).
 *
 * Codificacion XCHG (primario 0x14):
 *   byte[2] y byte[3] codifican cada operando:
 *     - bit 6 del byte = tipo (0=general, 1=especial)
 *     - Si especial: bits 5-0 = codigo de registro especial (ver special_reg_encoding)
 *     - Si general:  bits 7-4 = modo de acceso (0=b,1=w,2=d,3=q), bits 3-0 = numero de registro
 */

#include "disasm/disasm.h"

#include "runtime/manager_runtime.h"
#include "runtime/proceso_runtime.h"
#include "runtime/decode_table.h"
#include "emmit/emmit_decl.h"
#include "util/ansi.h"

#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>

using namespace Assembly::Bytecode;

namespace disasm {

// sufijos de modo de tamano (indice = modo 0-3)
static const char *mode_sfx[]  = {"b", "w", "d", "q"};
// multiplicadores de escala SIB
static const int   scale_val[] = {1, 2, 4, 8};

/**
 * @brief Traduce un codigo de registro especial (6 bits) a su nombre de texto.
 *
 * Los registros especiales (rip, rsp, rbp, rflags, cur0-cur3) tienen codigos
 * distintos a los registros generales R00-R15.
 *
 * @param code Codigo de 6 bits del registro especial.
 * @return Nombre del registro o "?" si el codigo es desconocido.
 */
static const char *special_reg_name(uint8_t code) {
    static const char *tbl[64] = {};
    static bool ready = false;
    if (!ready) {
        tbl[0]  = "cur0"; tbl[1] = "cur1"; tbl[2] = "cur2"; tbl[3] = "cur3";
        tbl[8]  = "rip";  tbl[9] = "rbp";  tbl[10] = "rsp"; tbl[11] = "rflags";
        ready = true;
    }
    return (code < 64 && tbl[code]) ? tbl[code] : "?";
}

/**
 * @brief Formatea un operando XCHG a texto.
 *
 * Codificacion de cada byte de operando:
 *   - bit 6 = tipo: 0=registro general, 1=registro especial
 *   - Si especial: bits 5-0 = codigo de registro especial
 *   - Si general:  bits 7-4 = modo (0=b,1=w,2=d,3=q), bits 3-0 = numero de registro
 *
 * @param b Byte del operando (raw[2] o raw[3]).
 * @return Cadena de texto del operando formateado.
 */
static std::string fmt_xchg_operand(uint8_t b) {
    char buf[32] = {};
    uint8_t is_special = (b >> 6) & 0x1; // bit 6 = tipo de registro
    uint8_t code       = b & 0x3F;        // bits 5-0 = codigo

    if (is_special) {
        // registro especial: codigo referencia directamente al registro
        return special_reg_name(code);
    } else {
        // registro general: nibble alto = modo de acceso, nibble bajo = numero
        uint8_t mode = code >> 4;   // modo de tamano (0=b, 1=w, 2=d, 3=q)
        uint8_t reg  = code & 0xF;  // numero de registro 0-15
        snprintf(buf, sizeof(buf), "r%u [%s]", reg, mode_sfx[mode < 4 ? mode : 3]);
        return buf;
    }
}

/**
 * @brief Devuelve true si el opcode extendido es una instruccion ZMM-ZMM binaria.
 *
 * Instrucciones binarias ZMM: fmov (0xF0), fadd (0xF1), fsub (0xF2),
 * fmul (0xF3), fdiv (0xF4), fcmp (0xF5).
 *
 * @param opc Segundo byte del opcode extendido.
 * @return true si la instruccion opera sobre dos registros ZMM.
 */
static bool is_zmm_binary(uint8_t opc) {
    return opc >= 0xF0 && opc <= 0xF5;
}

/**
 * @brief Devuelve true si el opcode extendido es una instruccion ZMM unaria.
 *
 * Instrucciones unarias ZMM: fsqrt (0xF6), fabs (0xF7), fneg (0xF8).
 *
 * @param opc Segundo byte del opcode extendido.
 * @return true si la instruccion opera sobre un unico registro ZMM.
 */
static bool is_zmm_unary(uint8_t opc) {
    return opc >= 0xF6 && opc <= 0xF8;
}

/**
 * @brief Formatea los operandos de una instruccion extendida (prefijo 0x00).
 *
 * Cubre todas las instrucciones de la tabla extendida, incluyendo:
 *   - ALU/MOV/MOVC (opcodes 0x03..0x1F)
 *   - GETPROC/GETVM/GETMGR (0xC6..0xC8)
 *   - Corutinas Modelo A: yield (0xEC), resume (0xED), spawn (0xEE)
 *   - Fibras: swapctx (0xEF)
 *   - Flotante ZMM-ZMM: fmov (0xF0), fadd-fcmp (0xF1..0xF5)
 *   - Flotante unario:   fsqrt/fabs/fneg (0xF6..0xF8)
 *   - Conversion:        fcvt/fcvt.ps (0xF9)
 *   - Inmediato ZMM:     fmowi (0xFA, FIXED_11)
 *   - Memoria ZMM:       fload (0xFB), fstore (0xFC)
 *
 * @param opc  Segundo byte de la instruccion (opcode extendido).
 * @param fmt  Descriptor del formato de instruccion.
 * @param raw  Buffer con los bytes crudos de la instruccion (hasta 12).
 * @param isz  Tamano total de la instruccion en bytes.
 * @return Cadena de texto con los operandos formateados.
 */
static std::string fmt_ext_operands(
    uint8_t opc,
    const runtime::InstrFormat *fmt,
    const uint8_t *raw,
    size_t isz
) {
    char buf[128] = {};

    switch (fmt->mode) {
        case AddressingMode::NONE:
            break; // instrucciones sin operandos (yield, hlt, nop...)

        case AddressingMode::INMED:
            // fmowi: FIXED_11 = [0x00][0xFA][ctrl][imm64 LE (8 bytes)]
            // ctrl (raw[2]): bits 7-6 = mode, bit 5 = is_f32, bits 3-0 = zmm_idx
            // bytes raw[3..10] = bits IEEE 754 del double en little-endian
            if (opc == 0xFA && isz >= 11) {
                uint8_t  zmm_dst = raw[2] & 0x0F; // nibble bajo de ctrl = indice ZMM
                uint64_t bits    = 0;
                __builtin_memcpy(&bits, raw + 3, 8); // inmediato de 64 bits a partir de raw[3]
                double   dval    = 0.0;
                __builtin_memcpy(&dval, &bits, 8);   // reinterpretar como double IEEE 754
                snprintf(buf, sizeof(buf),
                         "f%u, 0x%016llx  ; %.17g",
                         zmm_dst,
                         (unsigned long long)bits,
                         dval);
            }
            break;

        case AddressingMode::REG:
            if (opc == 0x1F || opc == 0x1E) {
                // MOVC/MOVCH: ctrl[2], datos[3]
                uint8_t ctrl = raw[2], b4 = raw[3];
                uint8_t dir  = (ctrl >> 5) & 0x1; // direccion del movimiento
                uint8_t r1   = ctrl & 0xF;         // registro fuente/destino
                uint8_t flag = (b4 >> 5) & 0x7;    // bandera de condicion
                uint8_t r2   = b4 & 0x1F;          // registro de memoria
                if (dir == 0) snprintf(buf, sizeof(buf), "r%u, [r%u], flag=%u", r1, r2, flag);
                else          snprintf(buf, sizeof(buf), "[r%u], r%u, flag=%u", r2, r1, flag);
            } else if (is_zmm_binary(opc) && isz == 4) {
                // fmov, fadd, fsub, fmul, fdiv, fcmp: dos registros ZMM
                // nibble bajo de raw[3] = ZMM destino
                // nibble alto de raw[3] = ZMM fuente
                uint8_t zmm_dst = raw[3] & 0xF;
                uint8_t zmm_src = raw[3] >> 4;
                snprintf(buf, sizeof(buf), "f%u, f%u", zmm_dst, zmm_src);
            } else if (is_zmm_unary(opc) && isz == 4) {
                // fsqrt fDst, fSrc / fabs fDst, fSrc / fneg fDst, fSrc
                // nibble bajo de raw[3] = ZMM destino, nibble alto = ZMM fuente
                uint8_t zmm_dst = raw[3] & 0xF;
                uint8_t zmm_src = raw[3] >> 4;
                snprintf(buf, sizeof(buf), "f%u, f%u", zmm_dst, zmm_src);
            } else if (opc == 0xF9 && isz == 4) {
                // fcvt / fcvt.ps: conversion GP<->ZMM
                // bit 2 de ctrl (raw[2]) = s: 0=GP->ZMM, 1=ZMM->GP
                // nibble bajo  de raw[3] = ZMM
                // nibble alto  de raw[3] = GP
                uint8_t s       = (raw[2] >> 2) & 0x1;
                uint8_t zmm_reg = raw[3] & 0xF;
                uint8_t gp_reg  = raw[3] >> 4;
                if (s == 0) snprintf(buf, sizeof(buf), "f%u, r%u", zmm_reg, gp_reg);
                else        snprintf(buf, sizeof(buf), "r%u, f%u", gp_reg,  zmm_reg);
            } else if (opc == 0xFB && isz == 4) {
                // fload fDst, rAddr: ZMM nibble bajo, GP nibble alto
                uint8_t zmm_dst = raw[3] & 0xF;
                uint8_t gp_addr = raw[3] >> 4;
                snprintf(buf, sizeof(buf), "f%u, r%u", zmm_dst, gp_addr);
            } else if (opc == 0xFC && isz == 4) {
                // fstore rAddr, fSrc: GP nibble alto, ZMM nibble bajo
                uint8_t zmm_src = raw[3] & 0xF;
                uint8_t gp_addr = raw[3] >> 4;
                snprintf(buf, sizeof(buf), "r%u, f%u", gp_addr, zmm_src);
            } else if ((opc == 0xED || opc == 0xEE) && isz == 4) {
                // resume rPID / spawn rAddr: un solo registro GP en nibble bajo
                uint8_t r1 = raw[3] & 0xF;
                snprintf(buf, sizeof(buf), "r%u", r1);
            } else if (opc == 0xEF && isz == 4) {
                // swapctx rDst, rSrc: nibble alto = destino, nibble bajo = origen
                uint8_t r_src = raw[3] & 0xF;
                uint8_t r_dst = raw[3] >> 4;
                snprintf(buf, sizeof(buf), "r%u, r%u", r_dst, r_src);
            } else if (isz == 4) {
                // instruccion reg-reg generica: ctrl[2] (mode 7-6), regs[3]
                uint8_t ctrl = raw[2], regs = raw[3];
                uint8_t mode = (ctrl >> 6) & 0x3;
                uint8_t r1   = regs & 0xF, r2 = regs >> 4;
                snprintf(buf, sizeof(buf), "r%u, r%u [%s]", r1, r2, mode_sfx[mode]);
            }
            break;

        case AddressingMode::MEM: {
            // instrucciones con inmediato (MIXED_SIZE): ctrl[2] codifica mode, sign, dir, reg
            uint8_t ctrl = raw[2];
            uint8_t mode = (ctrl >> 6) & 0x3; // tamano del inmediato: 0=8b, 1=16b, 2=32b, 3=64b
            uint8_t sign = (ctrl >> 5) & 0x1; // bit de signo
            uint8_t dir  = (ctrl >> 4) & 0x1; // 0=reg<-imm, 1=[reg]<-imm
            uint8_t reg  = ctrl & 0xF;         // numero de registro

            if (dir == 1 && sign == 1) {
                // caso especial: mov a registro especial; codigo = mode<<4|reg; imm64 en [3..10]
                uint64_t imm = 0; memcpy(&imm, raw + 3, 8);
                uint8_t  code = (uint8_t)((mode << 4) | reg);
                snprintf(buf, sizeof(buf), "%s, 0x%016llx",
                         special_reg_name(code), (unsigned long long)imm);
            } else {
                // operando reg/[reg] con inmediato de tamano variable
                uint64_t imm = 0;
                int nb = mode_to_bytes(mode); // numero de bytes del inmediato
                memcpy(&imm, raw + 3, nb);
                // enmascarar bits superiores para el tamano efectivo
                if      (mode == 0) imm &= 0xFFu;
                else if (mode == 1) imm &= 0xFFFFu;
                else if (mode == 2) imm &= 0xFFFFFFFFu;
                if (dir == 0)
                    snprintf(buf, sizeof(buf), "r%u, 0x%llx [%s]",
                             reg, (unsigned long long)imm, mode_sfx[mode]);
                else
                    snprintf(buf, sizeof(buf), "[r%u], 0x%llx [%s]",
                             reg, (unsigned long long)imm, mode_sfx[mode]);
            }
            break;
        }

        case AddressingMode::SIB: {
            // instruccion SIB FIXED_6: ctrl[2], regs[3], idx[4]
            uint8_t ctrl  = raw[2], regs = raw[3], idx = raw[4];
            uint8_t mode  = (ctrl >> 6) & 0x3; // tamano del acceso
            uint8_t dir   = (ctrl >> 4) & 0x1; // 0=reg<-[...], 1=[...]<-reg
            uint8_t scale = (ctrl >> 2) & 0x3; // indice en scale_val[]
            uint8_t hasi  = (ctrl >> 1) & 0x1; // 1=hay registro indice
            uint8_t rfin  = regs >> 4;          // registro final
            uint8_t rbase = regs & 0xF;         // registro base
            uint8_t ridx  = idx & 0xF;          // registro indice
            if (dir == 0) {
                if (hasi)
                    snprintf(buf, sizeof(buf), "r%u, [r%u+r%u*%d] [%s]",
                             rfin, rbase, ridx, scale_val[scale], mode_sfx[mode]);
                else
                    snprintf(buf, sizeof(buf), "r%u, [r%u] [%s]",
                             rfin, rbase, mode_sfx[mode]);
            } else {
                if (hasi)
                    snprintf(buf, sizeof(buf), "[r%u+r%u*%d], r%u [%s]",
                             rbase, ridx, scale_val[scale], rfin, mode_sfx[mode]);
                else
                    snprintf(buf, sizeof(buf), "[r%u], r%u [%s]",
                             rbase, rfin, mode_sfx[mode]);
            }
            break;
        }

        default:
            break;
    }
    return buf;
}

/**
 * @brief Formatea los operandos de una instruccion primaria (opcode de 1 byte).
 *
 * @param opc Primer byte de la instruccion (opcode primario).
 * @param fmt Descriptor del formato de instruccion.
 * @param raw Buffer con los bytes crudos de la instruccion (hasta 12).
 * @return Cadena de texto con los operandos formateados.
 */
static std::string fmt_primary_operands(
    uint8_t opc,
    const runtime::InstrFormat *fmt,
    const uint8_t *raw
) {
    char buf[128] = {};

    // tabla de mnemoticos de condicion de salto (indice = byte de condicion)
    static const char *cc[] = {
        "je","jne","jl","jle","jg","jge",
        "jb","jbe","ja","jae","jc","jnc",
        "jo","jno","js","jns","jmp"
    };

    switch (fmt->mode) {
        case AddressingMode::NONE:
            break;

        case AddressingMode::REG: {
            uint8_t data = raw[1];
            uint8_t reg  = data & 0xF; // numero de registro en nibble bajo
            if (opc == 0x04) {
                // inc/dec: bit6=variante (0=inc, 1=dec), bits5-4=modo de tamano
                uint8_t mode = (data >> 4) & 0x3;
                uint8_t var  = (data >> 6) & 0x1;
                snprintf(buf, sizeof(buf), "%s r%u [%s]",
                         (var ? "dec" : "inc"), reg, mode_sfx[mode]);
            } else if (opc == 0x14) {
                // XCHG: byte[2] y byte[3] codifican dos operandos mixtos (general/especial)
                // - bit 6 de cada byte: 0=general, 1=especial
                // - general: nibble alto = modo, nibble bajo = numero de registro
                // - especial: bits 5-0 = codigo de registro especial
                std::string op1 = fmt_xchg_operand(raw[2]);
                std::string op2 = fmt_xchg_operand(raw[3]);
                snprintf(buf, sizeof(buf), "%s, %s", op1.c_str(), op2.c_str());
            } else {
                // push, pop, jmpr, callvmr: un solo registro en nibble bajo de raw[1]
                snprintf(buf, sizeof(buf), "r%u", reg);
            }
            break;
        }

        case AddressingMode::INMED: {
            // JMP/CALLVM FIXED_10: [opc][cond][addr 8 bytes LE]
            uint8_t  cond = raw[1];
            uint64_t addr = 0; memcpy(&addr, raw + 2, 8);
            if (opc == 0x10) {
                // callvm: salto incondicional con direccion absoluta
                snprintf(buf, sizeof(buf), "0x%016llx", (unsigned long long)addr);
            } else {
                // jmp con condicion; indice 16 = jmp incondicional
                int ci = (cond < 0x10) ? (int)cond : 16;
                snprintf(buf, sizeof(buf), "%s 0x%016llx",
                         cc[ci], (unsigned long long)addr);
            }
            break;
        }

        default:
            break;
    }
    return buf;
}

/**
 * @brief Construye la cadena hexadecimal coloreada de los bytes de una instruccion.
 *
 * Cada byte recibe un color ANSI deterministico basado en su valor, de forma que
 * el mismo byte siempre aparece con el mismo color.  Muestra como maximo 10 bytes.
 *
 * @param raw    Buffer con los bytes crudos.
 * @param isz    Numero de bytes validos en @p raw.
 * @param color  Si es true aplica colores ANSI por byte.
 * @return Cadena con los bytes hex separados por espacio, con o sin codigos ANSI.
 */
static std::string build_hex_string(const uint8_t *raw, size_t isz, bool color) {
    std::ostringstream hs;
    size_t shown = (isz < 10) ? isz : 10; // mostrar hasta 10 bytes
    for (size_t i = 0; i < shown; ++i) {
        if (i) hs << " ";
        char hb[4]; snprintf(hb, sizeof(hb), "%02x", raw[i]);
        if (color) {
            hs << ansi::byte_color(raw[i]) << hb << ansi::RESET;
        } else {
            hs << hb;
        }
    }
    return hs.str();
}

// ---- API publica ----

void print_instruction(std::ostream &out, const DisasmResult &instr, const DisasmOptions &opts) {
    bool col = opts.use_color && ansi::is_enabled();

    // columna de direccion en gris atenuado
    char addr_buf[20];
    snprintf(addr_buf, sizeof(addr_buf), "%016llx", (unsigned long long)instr.address);
    if (col) out << ansi::DIM;
    out << "  " << addr_buf << "  ";
    if (col) out << ansi::RESET;

    if (opts.show_hex) {
        // columna de bytes hex (ya coloreados si opts.use_color)
        // padding a 30 caracteres: necesario aunque haya codigos ANSI
        // los codigos ANSI no son caracteres visibles, asi que se calcula
        // el numero de bytes visibles para alinear correctamente
        out << instr.hex;
        // calcular cuantos bytes visibles se imprimieron para el padding
        size_t shown = (instr.size < 10) ? instr.size : 10;
        // cada byte visible ocupa 3 chars ("xx ") excepto el ultimo (2 chars)
        // formula: shown*3-1 = chars visibles; se rellena hasta 32 para alinear el mnemotecnico
        size_t visible_chars = shown > 0 ? (shown * 3 - 1) : 0;
        size_t pad = (visible_chars < 32) ? (32 - visible_chars) : 1;
        for (size_t i = 0; i < pad; ++i) out << ' ';
    }

    // columna de mnemotecnico con color segun tipo
    if (col) out << ansi::mnem_color(instr.mnemonic.c_str());
    out << std::setw(10) << std::left << instr.mnemonic;
    if (col) out << ansi::RESET;

    // operandos en blanco normal
    out << instr.operands << "\n";
}

std::vector<DisasmResult> disasm_bytes(
    const uint8_t *data,
    size_t len,
    uint64_t base_addr,
    const DisasmOptions &opts
) {
    std::vector<DisasmResult> out;
    size_t off = 0; // desplazamiento actual en el buffer

    while (off < len && off < opts.max_bytes) {
        uint8_t b0 = data[off];
        bool    ext = (b0 == 0x00); // prefijo extendido
        uint8_t opc;
        const runtime::InstrFormat *fmt;

        if (ext) {
            if (off + 1 >= len) break; // no hay segundo byte
            opc = data[off + 1];
            fmt = &runtime::decode_table_extended[opc];
        } else {
            opc = b0;
            fmt = &runtime::decode_table_primary[b0];
        }

        DisasmResult r{};
        r.address = base_addr + off;

        // opcode desconocido: volcar como .db
        if (fmt->mode == AddressingMode::COUNT || !fmt->name || !fmt->name[0]) {
            char hb[8]; snprintf(hb, sizeof(hb), "%02x", b0);
            r.size     = 1;
            r.mnemonic = ".db";
            r.operands = std::string("0x") + hb;
            if (opts.show_hex) r.hex = build_hex_string(&b0, 1, opts.use_color && ansi::is_enabled());
            out.push_back(r);
            off++;
            continue;
        }

        // calcular tamano de la instruccion
        size_t isz;
        if (fmt->size == InstrSizeMode::MIXED_SIZE) {
            if (off + 2 >= len) break; // no hay ctrl byte
            uint8_t ctrl = data[off + 2];
            uint8_t mode = (ctrl >> 6) & 0x3;
            uint8_t sign = (ctrl >> 5) & 0x1;
            uint8_t dir  = (ctrl >> 4) & 0x1;
            isz = (dir == 1 && sign == 1) ? 11u : (size_t)(3 + mode_to_bytes(mode));
        } else {
            isz = instr_size(fmt->size);
        }
        if (off + isz > len) isz = len - off; // truncar si pasa el limite

        uint8_t raw[12] = {};
        memcpy(raw, data + off, (isz < 12) ? isz : 12);

        r.size     = isz;
        r.mnemonic = fmt->name;
        r.operands = ext ? fmt_ext_operands(opc, fmt, raw, isz)
                         : fmt_primary_operands(opc, fmt, raw);
        if (opts.show_hex)
            r.hex = build_hex_string(raw, isz, opts.use_color && ansi::is_enabled());

        bool is_hlt = (ext && opc == 0x03);
        out.push_back(r);
        off += isz;

        if (opts.stop_at_hlt && is_hlt) break;
    }
    return out;
}

void disasm_velb(const std::string &file, std::ostream &out, const DisasmOptions &opts) {
    ansi::init(); // habilitar colores si el terminal lo soporta
    bool col = opts.use_color && ansi::is_enabled();

    // crear VM temporal solo para el Loader; no se llama a start()
    runtime::ManageVM tmp(nullptr, 0);
    runtime::VM *vm_inst = tmp.loader.create_vm_instance(1);
    if (!vm_inst) { out << "[disasm] error: no se pudo crear VM\n"; return; }

    runtime::ProcessVM *proc = tmp.loader.load_executable(*vm_inst, file);
    if (!proc) {
        vm_inst->stop(); // seguro aunque start() no fue llamado
        out << "[disasm] error: no se pudo cargar el archivo\n";
        return;
    }

    uint64_t ip    = proc->registers.rip.raw(); // entrypoint segun cabecera .velb
    uint64_t limit = ip + opts.max_bytes;

    // cabecera del listado con color
    char ep_buf[24];
    snprintf(ep_buf, sizeof(ep_buf), "0x%016llx", (unsigned long long)ip);
    if (col) out << ansi::DIM;
    out << "; VestaVM disasm: " << file << "\n"
        << "; entrypoint: " << ep_buf << "\n\n";
    if (col) out << ansi::RESET;

    bool done = false;
    while (!done && ip < limit) {
        uint8_t raw[12] = {};
        uint8_t b0;

        try { b0 = proc->vm_mem[ip]; } catch (...) { break; }

        bool    ext = (b0 == 0x00);
        uint8_t opc;
        const runtime::InstrFormat *fmt;

        if (ext) {
            try { opc = proc->vm_mem[ip + 1]; } catch (...) { break; }
            fmt = &runtime::decode_table_extended[opc];
        } else {
            opc = b0;
            fmt = &runtime::decode_table_primary[b0];
        }

        // opcode desconocido: volcar como .db y avanzar un byte
        if (fmt->mode == AddressingMode::COUNT || !fmt->name || !fmt->name[0]) {
            if (col) out << ansi::DIM;
            char line[96];
            snprintf(line, sizeof(line),
                     "  %016llx   ", (unsigned long long)ip);
            out << line;
            if (col) { out << ansi::RESET; out << ansi::byte_color(b0); }
            char hb[4]; snprintf(hb, sizeof(hb), "%02x", b0);
            out << hb;
            if (col) out << ansi::RESET;
            out << "                             ";
            if (col) out << ansi::c(ansi::BR_BLACK);
            out << ".db       ";
            if (col) out << ansi::RESET;
            char op[8]; snprintf(op, sizeof(op), "0x%02x", b0);
            out << op << "\n";
            ip++;
            continue;
        }

        // calcular tamano de la instruccion
        size_t isz;
        if (fmt->size == InstrSizeMode::MIXED_SIZE) {
            uint8_t ctrl;
            try { ctrl = proc->vm_mem[ip + 2]; } catch (...) { break; }
            uint8_t mode = (ctrl >> 6) & 0x3;
            uint8_t sign = (ctrl >> 5) & 0x1;
            uint8_t dir  = (ctrl >> 4) & 0x1;
            isz = (dir == 1 && sign == 1) ? 11u : (size_t)(3 + mode_to_bytes(mode));
        } else {
            isz = instr_size(fmt->size);
        }

        // copiar bytes al buffer local
        for (size_t i = 0; i < isz && i < 12; ++i) {
            try { raw[i] = proc->vm_mem[ip + i]; } catch (...) { done = true; break; }
        }
        if (done) break;

        // construir y delegar en print_instruction
        DisasmResult r{};
        r.address  = ip;
        r.size     = isz;
        r.mnemonic = fmt->name;
        r.operands = ext ? fmt_ext_operands(opc, fmt, raw, isz)
                         : fmt_primary_operands(opc, fmt, raw);
        if (opts.show_hex)
            r.hex = build_hex_string(raw, isz, col);

        print_instruction(out, r, opts);

        if (opts.stop_at_hlt && ext && opc == 0x03) done = true;
        ip += isz;
    }

    vm_inst->stop(); // seguro: scheduler_futures vacio porque start() no fue llamado
}

} // namespace disasm
