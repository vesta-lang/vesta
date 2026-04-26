/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file ir_emitter.cpp
 * @brief Implementacion del emisor IR -> texto .vel (lowering con linear scan).
 *
 * Estructura del codigo emitido para cada funcion:
 *
 *   fn_label:
 *       enter <spill_count>      ; prologo: guarda rbp, reserva slots de pila
 *       ...instrucciones...
 *   fn_label_ret:
 *       leave
 *       ret
 *
 * Las etiquetas de bloque siguen el patron "fn_<nombre>_<bloque>".
 *
 * Manejo de operaciones de dos direcciones (.vel):
 *   adds r_dst, r_src  <=>  r_dst += r_src
 *   Si r_dst != reg_de(operando_izquierdo):
 *       mov r_dst, r_op0
 *       adds r_dst, r_op1
 *   Si r_dst == reg_de(operando_izquierdo):
 *       adds r_dst, r_op1     ; operacion en sitio, sin mov extra
 *
 * Spilling:
 *   Valores derramados se almacenan en slots de pila reservados por enter N.
 *   Cada slot ocupa 8 bytes; el acceso usa movc con base rbp y offset slot*8.
 *   load_src emite la carga antes de usar el valor; store_spilled emite el
 *   almacenamiento despues de calcularlo. Se usan r14 y r13 como scratches.
 */

#include "ir/ir_emitter.h"
#include "ir/ir_optimizer.h"
#include "ir/liveness.h"
#include "ir/regalloc.h"
#include <sstream>
#include <iomanip>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <cstdint>

namespace ir {

// =========================================================================
//  Contexto interno de emision por funcion
// =========================================================================

struct EmitCtx {
    const IrFunction  &fn;          // funcion SSA a emitir
    const AllocResult &alloc;       // asignacion de registros
    std::ostringstream &out;        // stream de salida .vel
    bool               comments;    // emitir comentarios de origen
    bool               emit_debug;  // emitir comentarios @line N por instruccion
    uint32_t           label_seq;   // secuencia para etiquetas unicas de condicion

    // nombre base para etiquetas de esta funcion
    std::string fn_lbl;

    EmitCtx(const IrFunction &fn_, const AllocResult &alloc_,
            std::ostringstream &out_, bool comments_, bool emit_debug_)
        : fn(fn_), alloc(alloc_), out(out_), comments(comments_),
          emit_debug(emit_debug_), label_seq(0), fn_lbl(sanitize(fn_.name)) {}

    // Convierte un nombre arbitrario a un identificador .vel valido
    static std::string sanitize(const std::string &s) {
        std::string r;
        r.reserve(s.size());
        for (char c : s) {
            if (std::isalnum((unsigned char)c) || c == '_') r += c;
            else r += '_';
        }
        return r;
    }

    // Nombre de etiqueta para un bloque
    std::string block_label(IrBlockId bid) const {
        if (bid < fn.blocks.size())
            return fn_lbl + "_" + fn.blocks[bid].name;
        return fn_lbl + "_bb" + std::to_string(bid);
    }

    // Nombre de registro para un valor (o r14 como fallback — sin efecto de lado)
    std::string reg_of(IrValueId vid) const {
        if (vid == IR_NO_VALUE) return "r0";
        auto it = alloc.reg_map.find(vid);
        if (it != alloc.reg_map.end()) return reg_name(it->second);
        return reg_name(SCRATCH_REG);
    }

    // True si el valor vid tiene un registro asignado (no derramado)
    bool is_in_reg(IrValueId vid) const {
        return vid != IR_NO_VALUE && alloc.reg_map.count(vid) > 0;
    }

    // Numero de registro de un valor (SCRATCH_REG si derramado)
    int reg_num(IrValueId vid) const {
        auto it = alloc.reg_map.find(vid);
        if (it != alloc.reg_map.end()) return it->second;
        return SCRATCH_REG;
    }

    // Materializa un operando FUENTE: si esta derramado emite un movc de carga
    // y devuelve el registro scratch (scratch_idx 0=r14, 1=r13).
    std::string load_src(IrValueId vid, int scratch_idx = 0) {
        if (vid == IR_NO_VALUE) return "r0";
        {
            auto it = alloc.reg_map.find(vid);
            if (it != alloc.reg_map.end()) return reg_name(it->second);
        }
        {
            auto it = alloc.spill_map.find(vid);
            if (it != alloc.spill_map.end()) {
                int sr = (scratch_idx == 0) ? SCRATCH_REG : SCRATCH2_REG;
                // Calcular direccion: r13 = rbp + slot*8
                out << "    mov r13, rbp\n";
                out << "    addu r13, " << (it->second * 8) << "\n";
                out << "    mov " << reg_name(sr) << ", [r13]\n";
                return reg_name(sr);
            }
        }
        return reg_name(SCRATCH_REG);
    }

    // Devuelve el registro DESTINO de vid: si esta derramado retorna r14
    // sin emitir codigo (el llamante debe invocar store_spilled despues).
    std::string dst_of(IrValueId vid) {
        if (vid == IR_NO_VALUE) return "r0";
        auto it = alloc.reg_map.find(vid);
        if (it != alloc.reg_map.end()) return reg_name(it->second);
        return reg_name(SCRATCH_REG);
    }

    // Si vid esta derramado, persiste SCRATCH_REG en su slot de pila.
    void store_spilled(IrValueId vid) {
        if (vid == IR_NO_VALUE) return;
        auto it = alloc.spill_map.find(vid);
        if (it != alloc.spill_map.end()) {
            // Calcular direccion: r13 = rbp + slot*8
            out << "    mov r13, rbp\n";
            out << "    addu r13, " << (it->second * 8) << "\n";
            out << "    mov [r13], " << reg_name(SCRATCH_REG) << "\n";
        }
    }

    // Emite un comentario si los comentarios estan activados
    void comment(const std::string &msg) {
        if (comments) out << "    // " << msg << "\n";
    }

    // Genera una etiqueta local unica
    std::string unique_lbl(const std::string &base) {
        return fn_lbl + "_" + base + "_" + std::to_string(label_seq++);
    }

    // Genera una referencia @Absolute con el prefijo de seccion "code."
    // Todas las etiquetas internas del .vel viven en la seccion "code".
    static std::string abs_lbl(const std::string &lbl) {
        return "code." + lbl;
    }
};

// =========================================================================
//  Utilidades internas del emisor
// =========================================================================

// Devuelve el tamano en bytes del tipo IR (para strides de arrays y similares)
static uint64_t ir_type_size(IrType t) {
    switch (t) {
        case IrType::I8:  case IrType::U8:  case IrType::BOOL: return 1;
        case IrType::I16: case IrType::U16: return 2;
        case IrType::I32: case IrType::U32: case IrType::F32:  return 4;
        default:                                                 return 8;
    }
}

// =========================================================================
//  Emision de instrucciones individuales
// =========================================================================

// Emite "mov r_dst, r_src" si son distintos (evita mov rx, rx)
static void emit_mov_if_needed(EmitCtx &ctx, const std::string &dst,
                                const std::string &src) {
    if (dst != src) ctx.out << "    mov " << dst << ", " << src << "\n";
}

// Emite una operacion binaria en formato dos-direcciones de .vel:
//   "op r_dst, r_src2"
// Carga operandos derramados desde pila (src1->r14, src2->r13) si es necesario.
// Almacena el resultado en pila si dst esta derramado.
static void emit_binop(EmitCtx &ctx, const std::string &mnemonic,
                        IrValueId dst, IrValueId src1, IrValueId src2) {
    std::string rs1 = ctx.load_src(src1, 0); // r14 si derramado
    std::string rs2 = ctx.load_src(src2, 1); // r13 si derramado
    std::string rd  = ctx.dst_of(dst);
    emit_mov_if_needed(ctx, rd, rs1);
    ctx.out << "    " << mnemonic << " " << rd << ", " << rs2 << "\n";
    ctx.store_spilled(dst);
}

// Emite una operacion unaria en dos-direcciones:
//   "op r_dst"
// Carga operando derramado (->r14) y almacena resultado si dst esta derramado.
static void emit_unop(EmitCtx &ctx, const std::string &mnemonic,
                       IrValueId dst, IrValueId src) {
    std::string rs = ctx.load_src(src, 0);
    std::string rd = ctx.dst_of(dst);
    emit_mov_if_needed(ctx, rd, rs);
    ctx.out << "    " << mnemonic << " " << rd << "\n";
    ctx.store_spilled(dst);
}

// Mnemonic de dos-direcciones para operaciones aritmeticas/logicas segun tipo.
static const char *arith_mnemonic(IrOp op, IrType type) {
    bool is_signed = (type == IrType::I8  || type == IrType::I16 ||
                      type == IrType::I32 || type == IrType::I64);
    switch (op) {
        case IrOp::ADD: return is_signed ? "adds" : "addu";
        case IrOp::SUB: return is_signed ? "subs" : "subu";
        case IrOp::MUL: return is_signed ? "muls" : "mulu";
        case IrOp::DIV: return is_signed ? "divs" : "divu";
        case IrOp::MOD: return is_signed ? "mods" : "modu";
        case IrOp::AND: return "and";
        case IrOp::OR:  return "or";
        case IrOp::XOR: return "xor";
        case IrOp::SHL: return "shl";
        case IrOp::SHR: return "shr";
        case IrOp::SAR: return "sar";
        // flotante
        case IrOp::FADD: return "fadd";
        case IrOp::FSUB: return "fsub";
        case IrOp::FMUL: return "fmul";
        case IrOp::FDIV: return "fdiv";
        case IrOp::FMIN: return "fmin";
        case IrOp::FMAX: return "fmax";
        default: return "add";
    }
}

// Mnemonic de comparacion segun tipo
static const char *cmp_mnemonic(IrOp op) {
    switch (op) {
        case IrOp::CMP_ULT: case IrOp::CMP_UGT:
        case IrOp::CMP_ULE: case IrOp::CMP_UGE: return "cmpu";
        case IrOp::FCMP_EQ: case IrOp::FCMP_NE:
        case IrOp::FCMP_LT: case IrOp::FCMP_GT:
        case IrOp::FCMP_LE: case IrOp::FCMP_GE: return "fcmp";
        default: return "cmps";
    }
}

// Inversion de condicion de salto: si cond es verdadera -> etiqueta false
// Emite "jmp.<cond_invertida> @Absolute(false_lbl)"
static void emit_cond_branch(EmitCtx &ctx, IrOp cmp_op,
                              const std::string &false_lbl) {
    const char *jmp = nullptr;
    switch (cmp_op) {
        case IrOp::CMP_EQ:  case IrOp::FCMP_EQ: jmp = "jmp.jne"; break;
        case IrOp::CMP_NE:  case IrOp::FCMP_NE: jmp = "jmp.je";  break;
        case IrOp::CMP_LT:  case IrOp::FCMP_LT: jmp = "jmp.jge"; break;
        case IrOp::CMP_GT:  case IrOp::FCMP_GT: jmp = "jmp.jle"; break;
        case IrOp::CMP_LE:  case IrOp::FCMP_LE: jmp = "jmp.jgt"; break;
        case IrOp::CMP_GE:  case IrOp::FCMP_GE: jmp = "jmp.jlt"; break;
        case IrOp::CMP_ULT: jmp = "jmp.jae"; break;
        case IrOp::CMP_UGT: jmp = "jmp.jls"; break;
        case IrOp::CMP_ULE: jmp = "jmp.jhi"; break;
        case IrOp::CMP_UGE: jmp = "jmp.jb";  break;
        default:             jmp = "jmp.je";  break; // cond==0 -> false
    }
    ctx.out << "    " << jmp << " @Absolute(\"" << EmitCtx::abs_lbl(false_lbl) << "\")\n";
}

// Emite el lowering de CMP standalone (no fusionada con BR_COND):
//   cmps r_a, r_b
//   jmp.<cond> __true
//   mov r_dst, 0
//   jmp __end
// __true:
//   mov r_dst, 1
// __end:
static void emit_cmp_standalone(EmitCtx &ctx, const IrInstr &ins) {
    if (ins.operands.size() < 2) return;
    std::string rd   = ctx.dst_of(ins.dst);
    std::string ra   = ctx.load_src(ins.operands[0], 0);
    std::string rb   = ctx.load_src(ins.operands[1], 1);
    std::string lbl_true = ctx.unique_lbl("ctrue");
    std::string lbl_end  = ctx.unique_lbl("cend");
    const char *cmp_mn = cmp_mnemonic(ins.op);

    ctx.out << "    " << cmp_mn << " " << ra << ", " << rb << "\n";
    // saltar a true si condicion se cumple (condicion directa)
    const char *jmp_direct = nullptr;
    switch (ins.op) {
        case IrOp::CMP_EQ:  case IrOp::FCMP_EQ: jmp_direct = "jmp.je";  break;
        case IrOp::CMP_NE:  case IrOp::FCMP_NE: jmp_direct = "jmp.jne"; break;
        case IrOp::CMP_LT:  case IrOp::FCMP_LT: jmp_direct = "jmp.jlt"; break;
        case IrOp::CMP_GT:  case IrOp::FCMP_GT: jmp_direct = "jmp.jgt"; break;
        case IrOp::CMP_LE:  case IrOp::FCMP_LE: jmp_direct = "jmp.jle"; break;
        case IrOp::CMP_GE:  case IrOp::FCMP_GE: jmp_direct = "jmp.jge"; break;
        case IrOp::CMP_ULT: jmp_direct = "jmp.jb";  break;
        case IrOp::CMP_UGT: jmp_direct = "jmp.jhi"; break;
        case IrOp::CMP_ULE: jmp_direct = "jmp.jls"; break;
        case IrOp::CMP_UGE: jmp_direct = "jmp.jae"; break;
        default: jmp_direct = "jmp.je"; break;
    }
    ctx.out << "    " << jmp_direct << " @Absolute(\"" << EmitCtx::abs_lbl(lbl_true) << "\")\n";
    ctx.out << "    mov " << rd << ", 0\n";
    ctx.out << "    jmp @Absolute(\"" << EmitCtx::abs_lbl(lbl_end) << "\")\n";
    ctx.out << lbl_true << ":\n";
    ctx.out << "    mov " << rd << ", 1\n";
    ctx.out << lbl_end << ":\n";
    ctx.store_spilled(ins.dst);
}

// Emite las copias paralelas para destruccion de PHI en el bloque predecesor.
//
// Implementa el algoritmo de copia paralela con deteccion de ciclos:
//   1. Construir lista de pares (dst_reg, src_reg).
//   2. Eliminar triviales (dst == src).
//   3. Emitir copias acicilicas en orden topologico (un destino que no es
//      fuente de otra copia puede emitirse de forma segura).
//   4. Para los ciclos restantes: romper cada ciclo usando SCRATCH_REG (r14)
//      como temporal.
//
// Para valores derramados (no en reg_map) se usa una carga/almacenamiento
// secuencial a traves de r14; los ciclos con derrames se gestionan igual.
static void emit_phi_copies(EmitCtx &ctx, IrBlockId pred_id, IrBlockId succ_id) {
    if (succ_id >= static_cast<IrBlockId>(ctx.fn.blocks.size())) return;
    const IrBlock &succ = ctx.fn.blocks[succ_id];

    // Paso 1: recopilar pares (dst_vid, src_vid)
    struct PhiCopy { IrValueId dst; IrValueId src; };
    std::vector<PhiCopy> copies;
    for (const auto &ins : succ.instrs) {
        if (ins.op != IrOp::PHI) break;
        if (ins.dst == IR_NO_VALUE) continue;
        for (const auto &pa : ins.phi_args) {
            if (pa.block == pred_id && pa.value != IR_NO_VALUE) {
                copies.push_back({ins.dst, pa.value});
            }
        }
    }
    if (copies.empty()) return;

    // Paso 2: separar copias en-registro de copias con derrames.
    // Las copias con derrame se emiten de forma simple (carga/mov/almacena).
    // Las copias totalmente en registro se someten al algoritmo de paralela.
    std::vector<PhiCopy> reg_copies;
    for (const auto &c : copies) {
        bool dst_in_reg = ctx.alloc.reg_map.count(c.dst) > 0;
        bool src_in_reg = ctx.alloc.reg_map.count(c.src) > 0;
        if (dst_in_reg && src_in_reg) {
            reg_copies.push_back(c);
        } else {
            // Al menos un operando esta derramado: copia secuencial segura
            // (los derrames son slots distintos, no hay alias entre ellos y r14)
            std::string r_src = ctx.load_src(c.src, 0);  // carga en r14 si spill
            std::string r_dst;
            bool dst_spilled = (ctx.alloc.spill_map.count(c.dst) > 0);
            r_dst = dst_spilled ? reg_name(SCRATCH_REG) : reg_name(ctx.alloc.reg_map.at(c.dst));
            emit_mov_if_needed(ctx, r_dst, r_src);
            if (dst_spilled) ctx.store_spilled(c.dst);
        }
    }

    // Paso 3: copia paralela para valores en registro.
    // Mapa: dst_reg -> src_reg (solo registros numericos)
    std::unordered_map<int, int> pending;
    for (const auto &c : reg_copies) {
        int d = ctx.alloc.reg_map.at(c.dst);
        int s = ctx.alloc.reg_map.at(c.src);
        if (d != s) pending[d] = s;
    }

    // Emitir copias cuyo destino no es fuente de ninguna otra (no introduce RAW)
    bool changed = true;
    while (changed && !pending.empty()) {
        changed = false;
        for (auto it = pending.begin(); it != pending.end(); ) {
            int d = it->first;
            int s = it->second;
            // Seguro si d no es fuente de otra copia pendiente
            bool d_is_src = false;
            for (const auto &p : pending) {
                if (p.first != d && p.second == d) { d_is_src = true; break; }
            }
            if (!d_is_src) {
                ctx.out << "    mov " << reg_name(d) << ", " << reg_name(s) << "\n";
                it = pending.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
    }

    // Paso 4: romper ciclos con r14 como temporal
    while (!pending.empty()) {
        auto it   = pending.begin();
        int start = it->first;
        // Guardar el valor inicial del primer registro del ciclo en r14
        ctx.out << "    mov " << reg_name(SCRATCH_REG) << ", " << reg_name(start) << "\n";
        int cur = start;
        for (;;) {
            int nxt = pending.at(cur);
            pending.erase(cur);
            if (nxt == start) {
                // Fin del ciclo: restaurar desde r14
                ctx.out << "    mov " << reg_name(cur) << ", " << reg_name(SCRATCH_REG) << "\n";
                break;
            }
            ctx.out << "    mov " << reg_name(cur) << ", " << reg_name(nxt) << "\n";
            cur = nxt;
        }
    }
}

// =========================================================================
//  Emision de una instruccion completa
// =========================================================================

// Devuelve true si ins es una CMP cuyo unico uso es la siguiente instruccion
// BR_COND (para fusion cmp+branch).
static bool can_fuse_cmp_brcond(const IrBlock &bb, size_t cmp_idx,
                                  const IrInstr &br_cond_ins) {
    const IrInstr &cmp = bb.instrs[cmp_idx];
    if (cmp.dst == IR_NO_VALUE) return false;
    // La fusion es segura si el resultado cmp no se usa en ningun otro sitio
    // dentro del bloque, aparte de la siguiente instruccion BR_COND.
    // Comprobacion simplificada: solo verificamos que sea el uso inmediato.
    if (!br_cond_ins.operands.empty() && br_cond_ins.operands[0] == cmp.dst)
        return true;
    return false;
}

static void emit_instr(EmitCtx &ctx, const IrBlock &bb, size_t idx,
                        bool &skip_next) {
    skip_next = false;
    const IrInstr &ins = bb.instrs[idx];

    if (ctx.emit_debug && ins.source_line > 0) {
        ctx.out << "    // @line " << ins.source_line << "\n";
    }

    switch (ins.op) {

        // --- NOP ---
        case IrOp::NOP:
            ctx.out << "    nop1\n";
            break;

        // --- CONST ---
        case IrOp::CONST: {
            std::string rd = ctx.dst_of(ins.dst);
            ctx.out << "    mov " << rd << ", " << ins.imm << "\n";
            ctx.store_spilled(ins.dst);
            break;
        }

        // --- MOV ---
        case IrOp::MOV:
            if (!ins.operands.empty()) {
                std::string rs = ctx.load_src(ins.operands[0], 0);
                std::string rd = ctx.dst_of(ins.dst);
                emit_mov_if_needed(ctx, rd, rs);
                ctx.store_spilled(ins.dst);
            }
            break;

        // --- Aritmetica entera binaria ---
        case IrOp::ADD: case IrOp::SUB: case IrOp::MUL:
        case IrOp::DIV: case IrOp::MOD:
        case IrOp::AND: case IrOp::OR:  case IrOp::XOR:
        case IrOp::SHL: case IrOp::SHR: case IrOp::SAR:
        // Aritmetica flotante binaria
        case IrOp::FADD: case IrOp::FSUB: case IrOp::FMUL:
        case IrOp::FDIV: case IrOp::FMIN: case IrOp::FMAX:
            if (ins.operands.size() >= 2)
                emit_binop(ctx, arith_mnemonic(ins.op, ins.type),
                           ins.dst, ins.operands[0], ins.operands[1]);
            break;

        // --- Aritmetica entera unaria ---
        case IrOp::NEG: {
            // -x = 0 - x  => mov r_dst, 0; subs r_dst, r_src
            if (ins.operands.empty()) break;
            {
                std::string rd  = ctx.dst_of(ins.dst);
                std::string rs  = ctx.load_src(ins.operands[0], 0);
                ctx.out << "    mov " << rd << ", 0\n";
                ctx.out << "    subs " << rd << ", " << rs << "\n";
                ctx.store_spilled(ins.dst);
            }
            break;
        }

        // --- Operaciones unarias bitwise/float ---
        case IrOp::NOT:
            if (!ins.operands.empty())
                emit_unop(ctx, "not", ins.dst, ins.operands[0]);
            break;
        case IrOp::FNEG:
            if (!ins.operands.empty())
                emit_unop(ctx, "fneg", ins.dst, ins.operands[0]);
            break;
        case IrOp::FABS:
            if (!ins.operands.empty())
                emit_unop(ctx, "fabs", ins.dst, ins.operands[0]);
            break;
        case IrOp::FSQRT:
            if (!ins.operands.empty())
                emit_unop(ctx, "fsqrt", ins.dst, ins.operands[0]);
            break;
        case IrOp::FFLOOR: case IrOp::FCEIL: case IrOp::FROUND: {
            // VestaVM no tiene floor/ceil/round nativo; copiar y delegar a stdlib
            if (!ins.operands.empty()) {
                std::string rs = ctx.load_src(ins.operands[0], 0);
                std::string rd = ctx.dst_of(ins.dst);
                emit_mov_if_needed(ctx, rd, rs);
                ctx.store_spilled(ins.dst);
            }
            ctx.comment("TODO: floor/ceil/round -> stdlib vesta_math");
            break;
        }

        // --- Conversion de tipos ---
        case IrOp::CAST: case IrOp::ZEXT: case IrOp::SEXT:
        case IrOp::TRUNC: {
            if (!ins.operands.empty()) {
                std::string rs = ctx.load_src(ins.operands[0], 0);
                std::string rd = ctx.dst_of(ins.dst);
                emit_mov_if_needed(ctx, rd, rs);
                ctx.store_spilled(ins.dst);
            }
            break;
        }
        case IrOp::ITOF:
        case IrOp::UITOF:
            if (!ins.operands.empty())
                emit_unop(ctx, "fcvt", ins.dst, ins.operands[0]);
            break;
        case IrOp::FTOI:
        case IrOp::FTOUI:
            if (!ins.operands.empty())
                emit_unop(ctx, "fmowi", ins.dst, ins.operands[0]);
            break;
        case IrOp::F32TOF64: case IrOp::F64TOF32: case IrOp::BITCAST: {
            if (!ins.operands.empty()) {
                std::string rs = ctx.load_src(ins.operands[0], 0);
                std::string rd = ctx.dst_of(ins.dst);
                emit_mov_if_needed(ctx, rd, rs);
                ctx.store_spilled(ins.dst);
            }
            break;
        }

        // --- Comparaciones (standalone, no fusionadas) ---
        case IrOp::CMP_EQ:  case IrOp::CMP_NE:
        case IrOp::CMP_LT:  case IrOp::CMP_GT:
        case IrOp::CMP_LE:  case IrOp::CMP_GE:
        case IrOp::CMP_ULT: case IrOp::CMP_UGT:
        case IrOp::CMP_ULE: case IrOp::CMP_UGE:
        case IrOp::FCMP_EQ: case IrOp::FCMP_NE:
        case IrOp::FCMP_LT: case IrOp::FCMP_GT:
        case IrOp::FCMP_LE: case IrOp::FCMP_GE: {
            // Intentar fusion con la siguiente instruccion BR_COND
            if (idx + 1 < bb.instrs.size()) {
                const IrInstr &next = bb.instrs[idx + 1];
                if (next.op == IrOp::BR_COND
                    && can_fuse_cmp_brcond(bb, idx, next)) {
                    // Fusion: emitir cmp + salto condicional ahora
                    if (ins.operands.size() >= 2) {
                        const char *cmp_mn = cmp_mnemonic(ins.op);
                        std::string ra = ctx.load_src(ins.operands[0], 0);
                        std::string rb = ctx.load_src(ins.operands[1], 1);
                        ctx.out << "    " << cmp_mn << " " << ra << ", " << rb << "\n";
                        // Copias phi para la rama true (pred=este bloque)
                        IrBlockId bid = static_cast<IrBlockId>(
                            &bb - ctx.fn.blocks.data());
                        emit_phi_copies(ctx, bid, next.target_block);
                        emit_cond_branch(ctx, ins.op,
                                         ctx.block_label(next.false_block));
                        // Copias phi para la rama false ya se emiten con el jmp
                        ctx.out << "    jmp @Absolute(\""
                                << EmitCtx::abs_lbl(ctx.block_label(next.target_block)) << "\")\n";
                    }
                    skip_next = true; // ya procesamos la siguiente instruccion
                    return;
                }
            }
            // Sin fusion: emitir comparacion como valor booleano
            emit_cmp_standalone(ctx, ins);
            break;
        }

        // --- Flujo de control ---
        case IrOp::BR: {
            IrBlockId bid = static_cast<IrBlockId>(&bb - ctx.fn.blocks.data());
            emit_phi_copies(ctx, bid, ins.target_block);
            ctx.out << "    jmp @Absolute(\""
                    << EmitCtx::abs_lbl(ctx.block_label(ins.target_block)) << "\")\n";
            break;
        }

        case IrOp::BR_COND: {
            // BR_COND no fusionada: el valor condicion es un bool (0 o 1)
            // Comparar r_cond con 0
            if (ins.operands.empty()) break;
            std::string rc  = ctx.load_src(ins.operands[0], 0);
            IrBlockId   bid = static_cast<IrBlockId>(&bb - ctx.fn.blocks.data());
            ctx.out << "    mov r14, 0\n";
            ctx.out << "    cmpu " << rc << ", r14\n";
            emit_phi_copies(ctx, bid, ins.false_block);
            ctx.out << "    jmp.je @Absolute(\""
                    << EmitCtx::abs_lbl(ctx.block_label(ins.false_block)) << "\")\n";
            emit_phi_copies(ctx, bid, ins.target_block);
            ctx.out << "    jmp @Absolute(\""
                    << EmitCtx::abs_lbl(ctx.block_label(ins.target_block)) << "\")\n";
            break;
        }

        case IrOp::RET: {
            if (!ins.operands.empty()) {
                std::string rs = ctx.load_src(ins.operands[0], 0);
                emit_mov_if_needed(ctx, "r0", rs);
            }
            ctx.out << "    jmp @Absolute(\"" << EmitCtx::abs_lbl(ctx.fn_lbl + "_ret") << "\")\n";
            break;
        }

        case IrOp::UNREACHABLE:
            ctx.out << "    hlt\n";
            break;

        // --- PHI: ya se manejo en emit_phi_copies; aqui es un no-op ---
        case IrOp::PHI:
            // Las copias se emitieron en los predecesores antes del salto
            break;

        // --- Llamadas ---
        case IrOp::CALL:
        case IrOp::TAILCALL: {
            // Marshal de argumentos: r1, r2, ..., r12
            size_t nargs = std::min(ins.operands.size(), (size_t)12);
            for (size_t ai = 0; ai < nargs; ++ai) {
                std::string r_arg = ctx.load_src(ins.operands[ai], 0);
                emit_mov_if_needed(ctx, reg_name(static_cast<int>(ai + 1)), r_arg);
            }
            ctx.out << "    mov r15, " << nargs << "\n";
            if (ins.op == IrOp::TAILCALL) {
                ctx.out << "    leave\n";
                // Cargar direccion en r0 y usar tailcall de registro (unica forma soportada)
                ctx.out << "    mov r0, @Absolute(\""
                        << EmitCtx::abs_lbl(EmitCtx::sanitize(ins.func_name)) << "\")\n";
                ctx.out << "    tailcall r0\n";
            } else {
                ctx.out << "    callvm @Absolute(\""
                        << EmitCtx::abs_lbl(EmitCtx::sanitize(ins.func_name)) << "\")\n";
                if (ins.dst != IR_NO_VALUE) {
                    std::string rd = ctx.dst_of(ins.dst);
                    emit_mov_if_needed(ctx, rd, "r0");
                    ctx.store_spilled(ins.dst);
                }
            }
            break;
        }

        case IrOp::CALLIND: {
            std::string rfn = ctx.load_src(ins.func_ptr, 0);
            size_t nargs = std::min(ins.operands.size(), (size_t)12);
            for (size_t ai = 0; ai < nargs; ++ai) {
                std::string r_arg = ctx.load_src(ins.operands[ai], 0);
                emit_mov_if_needed(ctx, reg_name(static_cast<int>(ai + 1)), r_arg);
            }
            ctx.out << "    mov r15, " << nargs << "\n";
            ctx.out << "    callvm " << rfn << "\n";
            if (ins.dst != IR_NO_VALUE) {
                std::string rd = ctx.dst_of(ins.dst);
                emit_mov_if_needed(ctx, rd, "r0");
                ctx.store_spilled(ins.dst);
            }
            break;
        }

        case IrOp::CALLVIRT: {
            if (ins.operands.empty()) break;
            std::string r_obj = ctx.load_src(ins.operands[0], 0);
            size_t nargs = ins.operands.size() > 1
                           ? std::min(ins.operands.size() - 1, (size_t)12) : 0;
            for (size_t ai = 0; ai < nargs; ++ai) {
                std::string r_arg = ctx.load_src(ins.operands[ai + 1], 0);
                emit_mov_if_needed(ctx, reg_name(static_cast<int>(ai + 1)), r_arg);
            }
            ctx.out << "    mov r15, " << nargs << "\n";
            ctx.out << "    callvirt " << r_obj << ", " << ins.imm << "\n";
            if (ins.dst != IR_NO_VALUE) {
                std::string rd = ctx.dst_of(ins.dst);
                emit_mov_if_needed(ctx, rd, "r0");
                ctx.store_spilled(ins.dst);
            }
            break;
        }

        case IrOp::CALLN: {
            size_t nargs = std::min(ins.operands.size(), (size_t)12);
            for (size_t ai = 0; ai < nargs; ++ai) {
                std::string r_arg = ctx.load_src(ins.operands[ai], 0);
                emit_mov_if_needed(ctx, reg_name(static_cast<int>(ai + 1)), r_arg);
            }
            ctx.out << "    mov r15, " << nargs << "\n";
            ctx.out << "    calln @Method(\"" << ins.func_name << "\")\n";
            if (ins.dst != IR_NO_VALUE) {
                std::string rd = ctx.dst_of(ins.dst);
                emit_mov_if_needed(ctx, rd, "r0");
                ctx.store_spilled(ins.dst);
            }
            break;
        }

        // --- Memoria ---
        case IrOp::ALLOCA: {
            // Reservar espacio en pila: subsp rsp, count*8
            uint64_t bytes = ins.imm * 8;
            ctx.out << "    subsp rsp, " << bytes << "\n";
            if (ins.dst != IR_NO_VALUE) {
                std::string rd = ctx.dst_of(ins.dst);
                ctx.out << "    readcur " << rd << "\n";
                ctx.store_spilled(ins.dst);
            }
            break;
        }

        case IrOp::LOAD: {
            if (ins.operands.empty()) break;
            std::string rp  = ctx.load_src(ins.operands[0], 0);
            std::string rd  = ctx.dst_of(ins.dst);
            ctx.out << "    movc " << rd << ", [" << rp << ", r0, 0, 0]\n";
            ctx.store_spilled(ins.dst);
            break;
        }

        case IrOp::STORE: {
            if (ins.operands.size() < 2) break;
            std::string rv = ctx.load_src(ins.operands[0], 0); // valor a escribir
            std::string rp = ctx.load_src(ins.operands[1], 1); // puntero destino
            ctx.out << "    movc [" << rp << ", r0, 0, 0], " << rv << "\n";
            break;
        }

        case IrOp::MEMCPY: {
            if (ins.operands.size() < 3) break;
            std::string r_dst_ = ctx.reg_of(ins.operands[0]);
            std::string r_src_ = ctx.reg_of(ins.operands[1]);
            std::string r_len_ = ctx.reg_of(ins.operands[2]);
            ctx.out << "    vmcopy " << r_dst_ << ", " << r_src_ << ", " << r_len_ << "\n";
            break;
        }

        // --- OOP / GC ---
        case IrOp::NEWOBJ: {
            if (ins.operands.empty()) break;
            std::string r_cls = ctx.reg_of(ins.operands[0]);
            ctx.out << "    mov r1, " << r_cls << "\n";
            ctx.out << "    mov r15, 1\n";
            ctx.out << "    newobj r1\n";
            if (ins.dst != IR_NO_VALUE)
                emit_mov_if_needed(ctx, ctx.reg_of(ins.dst), "r0");
            break;
        }

        case IrOp::GETFIELD: {
            // gcderef cur0, r_obj  ->  addcur cur0, byte_offset  ->  readcur r_dst, cur0
            if (ins.operands.empty()) break;
            std::string rd    = ctx.dst_of(ins.dst);
            std::string r_obj = ctx.load_src(ins.operands[0], 0);
            ctx.out << "    gcderef cur0, " << r_obj << "\n";
            if (ins.imm) ctx.out << "    addcur cur0, " << ins.imm << "\n";
            ctx.out << "    readcur " << rd << ", cur0\n";
            ctx.store_spilled(ins.dst);
            break;
        }

        case IrOp::SETFIELD: {
            // gcderef cur0, r_obj  ->  addcur cur0, byte_offset  ->  writecur cur0, r_val
            // Si el tipo es HANDLE: gcwb r_obj (write barrier)
            if (ins.operands.size() < 2) break;
            std::string r_obj = ctx.load_src(ins.operands[0], 0);
            std::string r_val = ctx.load_src(ins.operands[1], 1);
            ctx.out << "    gcderef cur0, " << r_obj << "\n";
            if (ins.imm) ctx.out << "    addcur cur0, " << ins.imm << "\n";
            ctx.out << "    writecur cur0, " << r_val << "\n";
            if (ins.type == IrType::HANDLE)
                ctx.out << "    gcwb " << r_obj << "\n";
            break;
        }

        case IrOp::INSTANCEOF: {
            if (ins.operands.size() < 2) break;
            std::string rd    = ctx.reg_of(ins.dst);
            std::string r_obj = ctx.reg_of(ins.operands[0]);
            std::string r_cls = ctx.reg_of(ins.operands[1]);
            ctx.out << "    instanceof " << rd << ", " << r_obj << ", " << r_cls << "\n";
            break;
        }

        case IrOp::CHECKCAST: {
            if (ins.operands.size() < 2) break;
            ctx.out << "    checkcast " << ctx.reg_of(ins.operands[0])
                    << ", " << ctx.reg_of(ins.operands[1]) << "\n";
            break;
        }

        case IrOp::ISNULL: {
            if (!ins.operands.empty())
                ctx.out << "    isnull " << ctx.reg_of(ins.dst)
                        << ", " << ctx.reg_of(ins.operands[0]) << "\n";
            break;
        }

        case IrOp::UNWRAP: {
            if (!ins.operands.empty()) {
                std::string rs = ctx.load_src(ins.operands[0], 0);
                std::string rd = ctx.dst_of(ins.dst);
                ctx.out << "    unwrap " << rd << ", " << rs << "\n";
                ctx.store_spilled(ins.dst);
            }
            break;
        }

        case IrOp::SPECIALIZE: {
            if (ins.operands.size() < 2) break;
            ctx.out << "    specialize " << ctx.reg_of(ins.dst)
                    << ", " << ctx.reg_of(ins.operands[0])
                    << ", " << ctx.reg_of(ins.operands[1]) << "\n";
            break;
        }

        // --- GEP / write barrier / arrays ---

        case IrOp::GEP: {
            // %ptr = gep.ptr %handle, byte_offset
            // Emite gcderef + addcur; el cursor cur0 queda apuntando al campo.
            // El %ptr resultante es un marcador; usar LOAD/STORE inmediatamente despues.
            if (ins.operands.empty()) break;
            std::string r_obj = ctx.load_src(ins.operands[0], 0);
            ctx.out << "    gcderef cur0, " << r_obj << "\n";
            if (ins.imm) ctx.out << "    addcur cur0, " << ins.imm << "\n";
            break;
        }

        case IrOp::GCWB_IR: {
            // gcwb_ir %handle  ->  gcwb r_handle
            if (!ins.operands.empty())
                ctx.out << "    gcwb " << ctx.load_src(ins.operands[0], 0) << "\n";
            break;
        }

        case IrOp::GCDEREF_IR: {
            // gcderef_ir %handle  ->  gcderef cur0, r_handle
            // Nota: no hay instruccion VM para exportar cur0 a registro general.
            // Este opcode es util solo cuando seguido de readcur/writecur via RAW_ASM.
            if (!ins.operands.empty())
                ctx.out << "    gcderef cur0, " << ctx.load_src(ins.operands[0], 0) << "\n";
            break;
        }

        case IrOp::ARRAY_ALLOC: {
            // %h = array_alloc.T %len
            // Layout en memoria VM: [u64 length][data[len * sizeof(T)]]
            // Delega a helper nativo stdlib/native/array/vesta_array:va_alloc(proc, esize, count)
            std::string r_len = ins.operands.empty() ? "r0" : ctx.load_src(ins.operands[0], 0);
            uint64_t esize = ir_type_size(ins.type);
            ctx.out << "    getproc r1\n";
            ctx.out << "    mov r2, " << esize << "\n";
            emit_mov_if_needed(ctx, "r3", r_len);
            ctx.out << "    mov r15, 3\n";
            ctx.out << "    calln @Method(\"stdlib/native/array/vesta_array:va_alloc\")\n";
            if (ins.dst != IR_NO_VALUE) {
                std::string rd = ctx.dst_of(ins.dst);
                emit_mov_if_needed(ctx, rd, "r0");
                ctx.store_spilled(ins.dst);
            }
            break;
        }

        case IrOp::ARRAY_LEN: {
            // longitud del array: offset 0 contiene el campo length (u64)
            if (ins.operands.empty()) break;
            std::string r_arr = ctx.load_src(ins.operands[0], 0);
            std::string rd    = ctx.dst_of(ins.dst);
            ctx.out << "    mov " << rd << ", [" << r_arr << "]\n";
            ctx.store_spilled(ins.dst);
            break;
        }

        case IrOp::ARRAY_LOAD: {
            // direccion del elemento: r_arr + r_idx * stride + 8 (los primeros 8 bytes son el campo length)
            if (ins.operands.size() < 2) break;
            std::string r_arr = ctx.load_src(ins.operands[0], 0);
            std::string r_idx = ctx.load_src(ins.operands[1], 1);
            std::string rd    = ctx.dst_of(ins.dst);
            uint64_t stride   = ir_type_size(ins.type);
            ctx.out << "    mov r13, " << r_idx << "\n";
            if (stride > 1)
                ctx.out << "    mulu r13, " << stride << "\n";
            ctx.out << "    addu r13, 8\n";
            ctx.out << "    addu r13, " << r_arr << "\n";
            ctx.out << "    mov " << rd << ", [r13]\n";
            ctx.store_spilled(ins.dst);
            break;
        }

        case IrOp::ARRAY_STORE: {
            // escritura de elemento: misma aritmetica de direccion que ARRAY_LOAD
            if (ins.operands.size() < 3) break;
            std::string r_arr = ctx.load_src(ins.operands[0], 0);
            std::string r_idx = ctx.load_src(ins.operands[1], 1);
            std::string r_val = ctx.load_src(ins.operands[2], 0);
            uint64_t stride   = ir_type_size(ins.type);
            ctx.out << "    mov r13, " << r_idx << "\n";
            if (stride > 1)
                ctx.out << "    mulu r13, " << stride << "\n";
            ctx.out << "    addu r13, 8\n";
            ctx.out << "    addu r13, " << r_arr << "\n";
            ctx.out << "    mov [r13], " << r_val << "\n";
            // write barrier si el tipo de elemento es HANDLE
            if (ins.type == IrType::HANDLE)
                ctx.out << "    gcwb " << r_arr << "\n";
            break;
        }

        // --- Operaciones de cadena ---

        case IrOp::STRMAKE: {
            // strmake.handle %buf_addr, %len [enc=imm]
            if (ins.operands.size() < 2) break;
            std::string rd    = ctx.dst_of(ins.dst);
            std::string r_buf = ctx.load_src(ins.operands[0], 0);
            std::string r_len = ctx.load_src(ins.operands[1], 1);
            ctx.out << "    strmake " << rd << ", " << r_buf << ", " << r_len << "\n";
            ctx.store_spilled(ins.dst);
            break;
        }

        case IrOp::STRLEN: {
            if (ins.operands.empty()) break;
            std::string rd    = ctx.dst_of(ins.dst);
            std::string r_str = ctx.load_src(ins.operands[0], 0);
            ctx.out << "    strlen " << rd << ", " << r_str << "\n";
            ctx.store_spilled(ins.dst);
            break;
        }

        case IrOp::STRCAT: {
            if (ins.operands.size() < 2) break;
            std::string rd = ctx.dst_of(ins.dst);
            std::string ra = ctx.load_src(ins.operands[0], 0);
            std::string rb = ctx.load_src(ins.operands[1], 1);
            ctx.out << "    strcat " << rd << ", " << ra << ", " << rb << "\n";
            ctx.store_spilled(ins.dst);
            break;
        }

        case IrOp::STRCMP: {
            if (ins.operands.size() < 2) break;
            std::string rd = ctx.dst_of(ins.dst);
            std::string ra = ctx.load_src(ins.operands[0], 0);
            std::string rb = ctx.load_src(ins.operands[1], 1);
            ctx.out << "    strcmp " << rd << ", " << ra << ", " << rb << "\n";
            ctx.store_spilled(ins.dst);
            break;
        }

        case IrOp::STRSLICE: {
            if (ins.operands.size() < 2) break;
            std::string rd    = ctx.dst_of(ins.dst);
            std::string r_str = ctx.load_src(ins.operands[0], 0);
            std::string r_rng = ctx.load_src(ins.operands[1], 1);
            ctx.out << "    strslice " << rd << ", " << r_str << ", " << r_rng << "\n";
            ctx.store_spilled(ins.dst);
            break;
        }

        case IrOp::STRFLAT: {
            if (ins.operands.empty()) break;
            std::string rd    = ctx.dst_of(ins.dst);
            std::string r_str = ctx.load_src(ins.operands[0], 0);
            ctx.out << "    strflat " << rd << ", " << r_str << "\n";
            ctx.store_spilled(ins.dst);
            break;
        }

        case IrOp::STRHASH: {
            if (ins.operands.empty()) break;
            std::string rd    = ctx.dst_of(ins.dst);
            std::string r_str = ctx.load_src(ins.operands[0], 0);
            ctx.out << "    strhash " << rd << ", " << r_str << "\n";
            ctx.store_spilled(ins.dst);
            break;
        }

        case IrOp::STRINTERN: {
            if (ins.operands.empty()) break;
            std::string rd    = ctx.dst_of(ins.dst);
            std::string r_str = ctx.load_src(ins.operands[0], 0);
            ctx.out << "    strintern " << rd << ", " << r_str << "\n";
            ctx.store_spilled(ins.dst);
            break;
        }

        case IrOp::STRRAW: {
            if (ins.operands.empty()) break;
            std::string rd    = ctx.dst_of(ins.dst);
            std::string r_str = ctx.load_src(ins.operands[0], 0);
            ctx.out << "    strraw " << rd << ", " << r_str << "\n";
            ctx.store_spilled(ins.dst);
            break;
        }

        case IrOp::STRCONV: {
            if (ins.operands.empty()) break;
            std::string rd    = ctx.dst_of(ins.dst);
            std::string r_str = ctx.load_src(ins.operands[0], 0);
            // el segundo operando puede ser un enc_handle o el imm codifica enc
            if (ins.operands.size() >= 2) {
                std::string r_enc = ctx.load_src(ins.operands[1], 1);
                ctx.out << "    strconv " << rd << ", " << r_str << ", " << r_enc << "\n";
            } else {
                ctx.out << "    strconv " << rd << ", " << r_str << ", " << ins.imm << "\n";
            }
            ctx.store_spilled(ins.dst);
            break;
        }

        case IrOp::STRRESERVE: {
            if (ins.operands.empty()) break;
            std::string rd    = ctx.dst_of(ins.dst);
            std::string r_cap = ctx.load_src(ins.operands[0], 0);
            ctx.out << "    strreserve " << rd << ", " << r_cap << "\n";
            ctx.store_spilled(ins.dst);
            break;
        }

        case IrOp::STRFINALIZE: {
            if (ins.operands.size() < 2) break;
            std::string r_str = ctx.load_src(ins.operands[0], 0);
            std::string r_len = ctx.load_src(ins.operands[1], 1);
            ctx.out << "    strfinalize " << r_str << ", " << r_len << "\n";
            break;
        }

        // --- Excepciones ---
        case IrOp::THROW: {
            if (!ins.operands.empty())
                ctx.out << "    throw " << ctx.reg_of(ins.operands[0]) << "\n";
            break;
        }

        case IrOp::TRYENTER: {
            if (ins.operands.size() < 2) break;
            ctx.out << "    tryenter " << ctx.reg_of(ins.operands[0])
                    << ", " << ctx.reg_of(ins.operands[1]) << "\n";
            break;
        }

        case IrOp::TRYLEAVE:
            ctx.out << "    tryleave\n";
            break;

        case IrOp::LANDINGPAD:
            if (ins.dst != IR_NO_VALUE)
                ctx.out << "    mov " << ctx.reg_of(ins.dst) << ", r0\n";
            break;

        // --- Async / futures ---
        case IrOp::FUTURE: {
            ctx.out << "    future\n"; // resultado en r0
            if (ins.dst != IR_NO_VALUE)
                emit_mov_if_needed(ctx, ctx.reg_of(ins.dst), "r0");
            break;
        }

        case IrOp::AWAIT: {
            if (!ins.operands.empty()) {
                emit_mov_if_needed(ctx, "r1", ctx.reg_of(ins.operands[0]));
                ctx.out << "    await r1\n"; // bloquea; resultado en r0
                if (ins.dst != IR_NO_VALUE)
                    emit_mov_if_needed(ctx, ctx.reg_of(ins.dst), "r0");
            }
            break;
        }

        case IrOp::FULFILL: {
            if (ins.operands.size() < 2) break;
            std::string r_fut = ctx.reg_of(ins.operands[0]);
            std::string r_val = ctx.reg_of(ins.operands[1]);
            ctx.out << "    fulfill " << r_fut << ", " << r_val << "\n";
            break;
        }

        case IrOp::REJECT: {
            if (ins.operands.size() < 2) break;
            ctx.out << "    reject " << ctx.reg_of(ins.operands[0])
                    << ", " << ctx.reg_of(ins.operands[1]) << "\n";
            break;
        }

        // --- Distribucion ---
        case IrOp::MSGSEND: {
            if (ins.operands.size() < 3) break;
            ctx.out << "    msgsend " << ctx.reg_of(ins.operands[0])
                    << ", " << ctx.reg_of(ins.operands[1])
                    << ", " << ctx.reg_of(ins.operands[2]) << "\n";
            if (ins.dst != IR_NO_VALUE)
                emit_mov_if_needed(ctx, ctx.reg_of(ins.dst), "r0");
            break;
        }

        case IrOp::MSGRECV: {
            if (ins.operands.size() < 2) break;
            ctx.out << "    msgrecv " << ctx.reg_of(ins.operands[0])
                    << ", " << ctx.reg_of(ins.operands[1]) << "\n";
            if (ins.dst != IR_NO_VALUE)
                emit_mov_if_needed(ctx, ctx.reg_of(ins.dst), "r0");
            break;
        }

        case IrOp::RSPAWN: {
            if (ins.operands.size() < 2) break;
            ctx.out << "    rspawn " << ctx.reg_of(ins.operands[0])
                    << ", " << ctx.reg_of(ins.operands[1]) << "\n";
            if (ins.dst != IR_NO_VALUE)
                emit_mov_if_needed(ctx, ctx.reg_of(ins.dst), "r0");
            break;
        }

        // --- Sincronizacion / monitores ---
        case IrOp::MONENTER:
            if (!ins.operands.empty())
                ctx.out << "    monenter " << ctx.reg_of(ins.operands[0]) << "\n";
            break;
        case IrOp::MONEXIT:
            if (!ins.operands.empty())
                ctx.out << "    monexit " << ctx.reg_of(ins.operands[0]) << "\n";
            break;
        case IrOp::MONWAIT:
            if (!ins.operands.empty())
                ctx.out << "    monwait " << ctx.reg_of(ins.operands[0]) << "\n";
            break;
        case IrOp::MONNOTI:
            if (!ins.operands.empty())
                ctx.out << "    monnoti " << ctx.reg_of(ins.operands[0]) << "\n";
            break;
        case IrOp::MONNOTA:
            if (!ins.operands.empty())
                ctx.out << "    monnota " << ctx.reg_of(ins.operands[0]) << "\n";
            break;

        // --- Intrinsics VM ---
        case IrOp::GETPROC:
            if (ins.dst != IR_NO_VALUE)
                ctx.out << "    getproc " << ctx.reg_of(ins.dst) << "\n";
            break;
        case IrOp::GETVM:
            if (ins.dst != IR_NO_VALUE)
                ctx.out << "    getvm " << ctx.reg_of(ins.dst) << "\n";
            break;
        case IrOp::GETMGR:
            if (ins.dst != IR_NO_VALUE)
                ctx.out << "    getmgr " << ctx.reg_of(ins.dst) << "\n";
            break;

        // --- Coroutines / scheduler ---
        case IrOp::SPAWN: {
            if (!ins.operands.empty()) {
                ctx.out << "    spawn " << ctx.reg_of(ins.operands[0]) << "\n";
                if (ins.dst != IR_NO_VALUE)
                    emit_mov_if_needed(ctx, ctx.reg_of(ins.dst), "r0");
            }
            break;
        }
        case IrOp::RESUME:
            if (!ins.operands.empty())
                ctx.out << "    resume " << ctx.reg_of(ins.operands[0]) << "\n";
            break;
        case IrOp::YIELD:
            ctx.out << "    yield\n";
            break;
        case IrOp::SWAPCTX:
            if (ins.operands.size() >= 2)
                ctx.out << "    swapctx " << ctx.reg_of(ins.operands[0])
                        << ", " << ctx.reg_of(ins.operands[1]) << "\n";
            break;

        case IrOp::RAW_ASM: {
            // Emitir cada linea del texto incrustado con indentacion estandar.
            // No se aplica ninguna transformacion: el contenido se vuelca verbatim.
            std::istringstream iss(ins.func_name);
            std::string ln;
            while (std::getline(iss, ln)) {
                // omitir lineas vacias al inicio/fin pero conservar las internas
                if (!ln.empty())
                    ctx.out << "    " << ln << "\n";
            }
            break;
        }

        default:
            ctx.comment("instruccion no soportada: " +
                        std::string(ir_op_name(ins.op)));
            ctx.out << "    nop1\n";
            break;
    }
}

// =========================================================================
//  Emision de una funcion completa
// =========================================================================

static std::string emit_function(const IrFunction &fn,
                                  const EmitOptions &opts,
                                  std::ostringstream &out,
                                  bool is_entry_point = false) {
    // Liveness + asignacion de registros
    LivenessResult liveness = compute_liveness(fn);
    AllocResult    alloc    = allocate_regs(fn, liveness);

    // Construir el contexto
    EmitCtx ctx(fn, alloc, out, opts.emit_comments, opts.emit_debug);

    // Etiqueta de funcion (exportada si corresponde)
    if (opts.export_all) {
        out << "@Export(" << ctx.fn_lbl << ")\n";
    }
    out << ctx.fn_lbl << ":\n";

    // Prologo: enter N (N = numero de slots de pila para spill)
    out << "    enter " << alloc.spill_count << "\n";

    // Spill de parametros extra (>12) que no caben en registros:
    // En la convencion actual se asumen ya en pila; solo emitimos comentario.
    if (opts.emit_comments && !fn.params.empty()) {
        out << "    // parametros: ";
        for (size_t i = 0; i < fn.params.size(); ++i) {
            IrValueId pid = fn.params[i];
            if (i > 0) out << ", ";
            if (alloc.reg_map.count(pid))
                out << fn.values[pid].name << "=" << reg_name(alloc.reg_map.at(pid));
            else
                out << fn.values[pid].name << "=[spill]";
        }
        out << "\n";
    }

    // Emision de bloques
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        const IrBlock &bb = fn.blocks[b];

        // Etiqueta del bloque (el bloque 0 = entry no necesita etiqueta extra
        // porque la etiqueta de la funcion ya apunta ahi, pero la emitimos igualmente
        // para que los saltos desde otros bloques puedan apuntar al entry).
        out << ctx.block_label(static_cast<IrBlockId>(b)) << ":\n";

        bool skip_next = false;
        for (size_t i = 0; i < bb.instrs.size(); ++i) {
            if (skip_next) { skip_next = false; continue; }
            emit_instr(ctx, bb, i, skip_next);
        }

        // Si el bloque no termina en terminador (bloque vacio o sin ret/br),
        // no emitir nada extra; el proximo bloque continua por caida natural.
    }

    // Epilogo comun de retorno
    out << ctx.fn_lbl << "_ret:\n";
    out << "    leave\n";
    // La funcion de entrada usa hlt para terminar la maquina explicitamente;
    // las demas funciones usan ret para retornar al llamador via callvm.
    out << (is_entry_point ? "    hlt\n\n" : "    ret\n\n");

    if (!alloc.spill_map.empty() && opts.emit_comments) {
        out << "    // INFO: " << alloc.spill_count
            << " valor(es) derramado(s) a pila; cargas/almacenamientos emitidos\n";
    }

    return {}; // sin error
}

// =========================================================================
//  Puntos de entrada publicos
// =========================================================================

EmitResult ir_emit_module(const IrModule &mod_in, const EmitOptions &opts) {
    EmitResult result;
    result.ok = true;

    // Trabajar sobre una copia para no modificar el modulo original
    IrModule mod = mod_in;

    // Aplicar optimizaciones IR
    ir_optimize(mod, opts.opt_level);

    std::ostringstream out;

    // Cabecera del modulo
    out << "// Emitido por ir_emitter - VestaVM\n";
    out << "// Nivel de optimizacion: O" << static_cast<int>(opts.opt_level) << "\n\n";

    // Cabecera .vel obligatoria: formato, espacio de direcciones y seccion
    // Si el modulo definio directivas @format/@space/@section las usamos;
    // en caso contrario emitimos valores por defecto razonables.
    std::string fmt = mod.format.empty() ? "velb" : mod.format;
    out << "@Format(\"" << fmt << "\")\n\n";

    if (mod.spaces.empty()) {
        // espacio de direcciones anonimo por defecto
        out << "@SpaceAddress {\n";
        out << "    @Name(\"anonymous\"),\n";
        out << "    @IniAddress(0x0000000000000000),\n";
        out << "    @EndAddress(0xFFFFFFFFFFFFFFFF)\n";
        out << "}\n\n";
    } else {
        for (const auto &sp : mod.spaces) {
            out << "@SpaceAddress {\n";
            out << "    @Name(\"" << sp.name << "\"),\n";
            out << "    @IniAddress(0x" << std::hex << std::setw(16) << std::setfill('0')
                << sp.ini_address << std::dec << "),\n";
            out << "    @EndAddress(0x"  << std::hex << std::setw(16) << std::setfill('0')
                << sp.end_address  << std::dec << ")\n";
            out << "}\n\n";
        }
    }

    if (mod.sections.empty()) {
        // seccion de codigo por defecto
        out << "@Section {\n";
        out << "    @Name(\"code\"),\n";
        out << "    @SpaceAddress(\"anonymous\")\n";
        out << "    @Align(0x1000)\n";
        out << "}\n\n";
    } else {
        for (const auto &sec : mod.sections) {
            out << "@Section {\n";
            out << "    @Name(\"" << sec.name << "\"),\n";
            out << "    @SpaceAddress(\"" << sec.space_name << "\")\n";
            out << "    @Align(0x" << std::hex << sec.align << std::dec << ")\n";
            out << "}\n\n";
        }
    }

    // Declaracion de modulo (@Module es obligatorio antes de @Export)
    std::string mod_name = opts.module_name.empty() ? mod.name : opts.module_name;
    if (mod_name.empty() && opts.export_all) mod_name = "ir_output";
    if (!mod_name.empty()) {
        out << "@Module(" << EmitCtx::sanitize(mod_name) << ")\n\n";
    }

    // Declaraciones de librerias nativas
    for (const auto &lib : mod.native_libs) {
        out << "@Lib(\"" << lib << "\")\n";
    }
    if (!mod.native_libs.empty()) out << "\n";

    // Importaciones
    for (const auto &imp : mod.imports) {
        out << "@import " << imp << "\n";
    }
    if (!mod.imports.empty()) out << "\n";

    // Emision de cada funcion; la primera funcion no-nativa es el punto de entrada
    bool first_func = true;
    for (const auto &fn : mod.functions) {
        if (fn.is_native) {
            // Stub nativo: solo comentario de importacion
            out << "// funcion nativa: " << fn.name << " (no se emite codigo)\n\n";
            continue;
        }
        std::string err = emit_function(fn, opts, out, first_func);
        first_func = false;
        if (!err.empty()) {
            result.ok    = false;
            result.error = err;
            return result;
        }
    }

    result.vel_text = out.str();
    return result;
}

EmitResult ir_emit_text(const std::string &ir_text, const EmitOptions &opts) {
    IrModule mod;
    std::string parse_err;
    if (!ir_parse(ir_text, mod, parse_err)) {
        EmitResult r;
        r.ok    = false;
        r.error = "parse: " + parse_err;
        return r;
    }
    return ir_emit_module(mod, opts);
}

} // namespace ir
