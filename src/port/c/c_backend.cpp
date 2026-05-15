/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file c_backend.cpp
 * @brief Backend C del transpiler.  Genera codigo C99/C11 portable y compacto.
 *
 * = Estrategia de generacion =
 *
 * El backend se apoya en TRES tecnicas para producir C de calidad humana:
 *
 *   1. **Single-use inlining**: cualquier SSA value con @c use_count == 1 y
 *      definicion pura (CONST, ADD, CMP, CAST, ...) NO se declara como
 *      variable.  Su expresion se construye en el sitio del uso via
 *      @c value_expr recursivo.  Ejemplo:
 *        IR:  v1 = const 1; v2 = v0 - v1; v3 = factorial(v2);
 *        out: v3 = factorial(v0 - 1);
 *
 *   2. **PHI parallel-move via lookup**: las copias PHI emiten al evaluar
 *      la expresion (no la variable intermedia), permitiendo que el
 *      "incremento de loop" se exprese inline en la propia copy:
 *        bb_body: v3 = v_acc + v_i;
 *                 v_acc = v3;  // phi copy
 *      se vuelve:
 *        bb_body: v_acc = v_acc + v_i;
 *
 *   3. **Skip de labels y gotos triviales**: bloques sin predecesores
 *      (entry) no llevan label.  BR a bloque inmediatamente siguiente se
 *      omite (fall-through natural).
 *
 * = Por que stdint.h =
 *
 * Cada SSA value lleva su tipo concreto (int8_t/int16_t/int32_t/...).
 * GCC -O3 usa la informacion para regalloc + vectorizar.  Sin tipo concreto,
 * el compilador trata todo como int/long y pierde oportunidades SIMD.
 *
 * = Comprobacion del binario =
 *
 * Output verificado con:
 *   - @c gcc -O3 -std=c11 -Wall -Wextra -Wpedantic produce 0 errores.
 *   - El asm generado es identico a C hecho a mano (4 instrucciones por iter
 *     en hot loops; ver tests/port_c/).
 */

#include "port/c/c_backend.h"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <sstream>
#include <unordered_set>

namespace port {

    // =========================================================================
    //  Mapeo de tipos
    // =========================================================================

    std::string CBackend::type_for(ir::IrType t, bool is_host_ptr) const {
        if (opts_.types == TypeStyle::Builtin) {
            switch (t) {
                case ir::IrType::VOID:   return "void";
                case ir::IrType::I8:     return "signed char";
                case ir::IrType::I16:    return "short";
                case ir::IrType::I32:    return "int";
                case ir::IrType::I64:    return "long long";
                case ir::IrType::U8:     return "unsigned char";
                case ir::IrType::U16:    return "unsigned short";
                case ir::IrType::U32:    return "unsigned int";
                case ir::IrType::U64:    return "unsigned long long";
                case ir::IrType::F32:    return "float";
                case ir::IrType::F64:    return "double";
                case ir::IrType::PTR:    return is_host_ptr ? "void*" : "uintptr_t";
                case ir::IrType::HANDLE: return "unsigned int";
                case ir::IrType::BOOL:   return "_Bool";
            }
        }
        switch (t) {
            case ir::IrType::VOID:   return "void";
            case ir::IrType::I8:     return "int8_t";
            case ir::IrType::I16:    return "int16_t";
            case ir::IrType::I32:    return "int32_t";
            case ir::IrType::I64:    return "int64_t";
            case ir::IrType::U8:     return "uint8_t";
            case ir::IrType::U16:    return "uint16_t";
            case ir::IrType::U32:    return "uint32_t";
            case ir::IrType::U64:    return "uint64_t";
            case ir::IrType::F32:    return "float";
            case ir::IrType::F64:    return "double";
            case ir::IrType::PTR:    return "void*";
            case ir::IrType::HANDLE: return "uint32_t";
            case ir::IrType::BOOL:   return "_Bool";
        }
        return "int64_t";
    }

    std::string CBackend::cast_for(ir::IrType t, bool is_host_ptr) const {
        return "(" + type_for(t, is_host_ptr) + ")";
    }

    // =========================================================================
    //  Sanitizacion de nombres
    // =========================================================================

    std::string CBackend::sanitize_name(const std::string &n) const {
        std::string out;
        out.reserve(n.size() + 8);
        if (n.empty() || std::isdigit(static_cast<unsigned char>(n[0]))) {
            out = "vex_";
        }
        for (char c : n) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
                out += c;
            } else {
                out += '_';
            }
        }
        if (out == "main") return "vex_main";
        return out;
    }

    // =========================================================================
    //  Formateo de constantes
    // =========================================================================

    std::string CBackend::format_const_literal(uint64_t imm, ir::IrType t) const {
        std::ostringstream oss;
        switch (t) {
            case ir::IrType::F64: {
                double d;
                std::memcpy(&d, &imm, 8);
                oss << "(double)" << d;
                return oss.str();
            }
            case ir::IrType::F32: {
                uint32_t bits = static_cast<uint32_t>(imm);
                float f;
                std::memcpy(&f, &bits, 4);
                oss << "(float)" << f << "f";
                return oss.str();
            }
            case ir::IrType::I8: case ir::IrType::I16:
            case ir::IrType::I32: case ir::IrType::I64: {
                int64_t s = static_cast<int64_t>(imm);
                // Optimizacion: si el valor cabe en int (32 bits con signo),
                // no le ponemos sufijo @c LL ni cast -- C lo promociona
                // automaticamente.  Resultado mas legible.
                if (s >= -0x80000000LL && s <= 0x7FFFFFFFLL && t == ir::IrType::I32) {
                    oss << s;
                } else {
                    oss << "(" << type_for(t, false) << ")" << s << "LL";
                }
                return oss.str();
            }
            case ir::IrType::U8: case ir::IrType::U16:
            case ir::IrType::U32: case ir::IrType::U64:
            case ir::IrType::HANDLE: {
                if (imm <= 0xFFFFFFFFu && t == ir::IrType::U32) {
                    oss << imm << "u";
                } else {
                    oss << "(" << type_for(t, false) << ")" << imm << "ULL";
                }
                return oss.str();
            }
            case ir::IrType::PTR:
                oss << "(void*)(uintptr_t)" << imm << "ULL";
                return oss.str();
            case ir::IrType::BOOL:
                return (imm != 0 ? "1" : "0");
            default:
                oss << imm << "ULL";
                return oss.str();
        }
    }

    // =========================================================================
    //  Inlining de expresiones (nivel A)
    // =========================================================================

    /**
     * @brief Sufijo binario C para un IrOp.
     */
    static const char *binop_symbol_for(ir::IrOp op) {
        switch (op) {
            case ir::IrOp::ADD: case ir::IrOp::FADD: return "+";
            case ir::IrOp::SUB: case ir::IrOp::FSUB: return "-";
            case ir::IrOp::MUL: case ir::IrOp::FMUL: return "*";
            case ir::IrOp::DIV: case ir::IrOp::FDIV: return "/";
            case ir::IrOp::MOD: return "%";
            case ir::IrOp::AND: return "&";
            case ir::IrOp::OR:  return "|";
            case ir::IrOp::XOR: return "^";
            case ir::IrOp::SHL: return "<<";
            default: return "?";
        }
    }

    static const char *cmp_symbol_for(ir::IrOp op, bool &is_unsigned) {
        is_unsigned = false;
        switch (op) {
            case ir::IrOp::CMP_EQ:   case ir::IrOp::FCMP_EQ: return "==";
            case ir::IrOp::CMP_NE:   case ir::IrOp::FCMP_NE: return "!=";
            case ir::IrOp::CMP_LT:   case ir::IrOp::FCMP_LT: return "<";
            case ir::IrOp::CMP_GT:   case ir::IrOp::FCMP_GT: return ">";
            case ir::IrOp::CMP_LE:   case ir::IrOp::FCMP_LE: return "<=";
            case ir::IrOp::CMP_GE:   case ir::IrOp::FCMP_GE: return ">=";
            case ir::IrOp::CMP_ULT:  is_unsigned = true; return "<";
            case ir::IrOp::CMP_UGT:  is_unsigned = true; return ">";
            case ir::IrOp::CMP_ULE:  is_unsigned = true; return "<=";
            case ir::IrOp::CMP_UGE:  is_unsigned = true; return ">=";
            default: return "==";
        }
    }

    std::string CBackend::value_expr(EmitContext &ctx, ir::IrValueId v) const {
        if (v == ir::IR_NO_VALUE) return "/*nil*/0";
        if (!ctx.tx || !ctx.tx->is_inline_candidate(v)) {
            return "v" + std::to_string(v);
        }
        const ir::IrInstr *def = ctx.tx->def_of(v);
        if (!def) return "v" + std::to_string(v);
        return build_inline_expr(ctx, *def);
    }

    std::string CBackend::build_inline_expr(EmitContext &ctx,
                                            const ir::IrInstr &ins) const {
        using ir::IrOp;
        switch (ins.op) {
            case IrOp::CONST:
                return format_const_literal(ins.imm, ins.type);

            case IrOp::MOV:
                return value_expr(ctx, ins.operands[0]);

            case IrOp::ADD: case IrOp::SUB: {
                // Aritmetica de PTR en el IR es POR BYTES.  En C, el
                // operador @c "ptr + int" hace aritmetica por sizeof(elem),
                // lo cual es INCORRECTO cuando el ptr esta tipado como
                // @c Counter* y queremos sumar bytes.  Cast a @c char* para
                // forzar byte arithmetic; cast final a @c void* para
                // compatibilidad con el load/store siguiente.
                std::string lhs = value_expr(ctx, ins.operands[0]);
                std::string rhs = value_expr(ctx, ins.operands[1]);
                if (ins.type == ir::IrType::PTR) {
                    const char *sym = (ins.op == ir::IrOp::ADD) ? "+" : "-";
                    return "(void*)((char*)" + lhs + " " + sym + " " + rhs + ")";
                }
                return "(" + lhs + " " + binop_symbol_for(ins.op) + " " + rhs + ")";
            }
            case IrOp::MUL:
            case IrOp::AND: case IrOp::OR:  case IrOp::XOR:
            case IrOp::SHL:
            case IrOp::FADD: case IrOp::FSUB: case IrOp::FMUL: case IrOp::FDIV: {
                std::string lhs = value_expr(ctx, ins.operands[0]);
                std::string rhs = value_expr(ctx, ins.operands[1]);
                return "(" + lhs + " " + binop_symbol_for(ins.op) + " " + rhs + ")";
            }

            case IrOp::SHR: {
                // logical shift -> cast a unsigned
                std::string utype;
                switch (ins.type) {
                    case ir::IrType::I8:  utype = "uint8_t";  break;
                    case ir::IrType::I16: utype = "uint16_t"; break;
                    case ir::IrType::I32: utype = "uint32_t"; break;
                    case ir::IrType::I64: utype = "uint64_t"; break;
                    default: utype = type_for(ins.type, false); break;
                }
                std::string lhs = value_expr(ctx, ins.operands[0]);
                std::string rhs = value_expr(ctx, ins.operands[1]);
                return "((" + type_for(ins.type, false) + ")(("
                       + utype + ")" + lhs + " >> " + rhs + "))";
            }
            case IrOp::SAR: {
                std::string stype;
                switch (ins.type) {
                    case ir::IrType::U8:  stype = "int8_t";  break;
                    case ir::IrType::U16: stype = "int16_t"; break;
                    case ir::IrType::U32: stype = "int32_t"; break;
                    case ir::IrType::U64: stype = "int64_t"; break;
                    default: stype = type_for(ins.type, false); break;
                }
                std::string lhs = value_expr(ctx, ins.operands[0]);
                std::string rhs = value_expr(ctx, ins.operands[1]);
                return "((" + type_for(ins.type, false) + ")(("
                       + stype + ")" + lhs + " >> " + rhs + "))";
            }

            case IrOp::FMIN: case IrOp::FMAX: {
                const char *cmp = (ins.op == IrOp::FMIN) ? "<" : ">";
                std::string lhs = value_expr(ctx, ins.operands[0]);
                std::string rhs = value_expr(ctx, ins.operands[1]);
                return "(" + lhs + " " + cmp + " " + rhs
                       + " ? " + lhs + " : " + rhs + ")";
            }

            case IrOp::NEG: case IrOp::FNEG:
                return "(-" + value_expr(ctx, ins.operands[0]) + ")";

            case IrOp::NOT:
                return "(~" + value_expr(ctx, ins.operands[0]) + ")";

            case IrOp::FABS: {
                std::string e = value_expr(ctx, ins.operands[0]);
                if (ins.type == ir::IrType::F32) {
                    return "((float)__builtin_fabsf(" + e + "))";
                }
                return "((double)__builtin_fabs(" + e + "))";
            }
            case IrOp::FSQRT: {
                std::string e = value_expr(ctx, ins.operands[0]);
                if (ins.type == ir::IrType::F32) {
                    return "((float)__builtin_sqrtf(" + e + "))";
                }
                return "((double)__builtin_sqrt(" + e + "))";
            }

            case IrOp::CMP_EQ: case IrOp::CMP_NE:
            case IrOp::CMP_LT: case IrOp::CMP_GT:
            case IrOp::CMP_LE: case IrOp::CMP_GE:
            case IrOp::CMP_ULT: case IrOp::CMP_UGT:
            case IrOp::CMP_ULE: case IrOp::CMP_UGE:
            case IrOp::FCMP_EQ: case IrOp::FCMP_NE:
            case IrOp::FCMP_LT: case IrOp::FCMP_GT:
            case IrOp::FCMP_LE: case IrOp::FCMP_GE: {
                bool is_unsigned = false;
                const char *cmp = cmp_symbol_for(ins.op, is_unsigned);
                std::string lhs = value_expr(ctx, ins.operands[0]);
                std::string rhs = value_expr(ctx, ins.operands[1]);
                if (is_unsigned && ctx.fn) {
                    // Determinar tipo de operandos para cast a unsigned.
                    ir::IrType ot = ir::IrType::I64;
                    if (ins.operands[0] < ctx.fn->values.size()) {
                        ot = ctx.fn->values[ins.operands[0]].type;
                    }
                    std::string utype;
                    switch (ot) {
                        case ir::IrType::I8:  utype = "uint8_t";  break;
                        case ir::IrType::I16: utype = "uint16_t"; break;
                        case ir::IrType::I32: utype = "uint32_t"; break;
                        case ir::IrType::I64: utype = "uint64_t"; break;
                        default: utype = type_for(ot, false); break;
                    }
                    return "((" + utype + ")" + lhs + " " + cmp
                           + " (" + utype + ")" + rhs + ")";
                }
                return "(" + lhs + " " + cmp + " " + rhs + ")";
            }

            case IrOp::CAST: case IrOp::TRUNC: case IrOp::ITOF: case IrOp::UITOF:
            case IrOp::FTOI: case IrOp::FTOUI:
            case IrOp::F32TOF64: case IrOp::F64TOF32: {
                return cast_for(ins.type, false)
                       + value_expr(ctx, ins.operands[0]);
            }

            case IrOp::ZEXT: {
                if (ctx.fn && ins.operands[0] < ctx.fn->values.size()) {
                    ir::IrType st = ctx.fn->values[ins.operands[0]].type;
                    std::string utype;
                    switch (st) {
                        case ir::IrType::I8:  utype = "uint8_t";  break;
                        case ir::IrType::I16: utype = "uint16_t"; break;
                        case ir::IrType::I32: utype = "uint32_t"; break;
                        case ir::IrType::I64: utype = "uint64_t"; break;
                        default: utype = type_for(st, false); break;
                    }
                    return cast_for(ins.type, false) + "(" + utype + ")"
                           + value_expr(ctx, ins.operands[0]);
                }
                return cast_for(ins.type, false)
                       + value_expr(ctx, ins.operands[0]);
            }
            case IrOp::SEXT: {
                if (ctx.fn && ins.operands[0] < ctx.fn->values.size()) {
                    ir::IrType st = ctx.fn->values[ins.operands[0]].type;
                    std::string stype;
                    switch (st) {
                        case ir::IrType::U8:  stype = "int8_t";  break;
                        case ir::IrType::U16: stype = "int16_t"; break;
                        case ir::IrType::U32: stype = "int32_t"; break;
                        case ir::IrType::U64: stype = "int64_t"; break;
                        default: stype = type_for(st, false); break;
                    }
                    return cast_for(ins.type, false) + "(" + stype + ")"
                           + value_expr(ctx, ins.operands[0]);
                }
                return cast_for(ins.type, false)
                       + value_expr(ctx, ins.operands[0]);
            }

            case IrOp::BITCAST: {
                ir::IrType st = ir::IrType::I64;
                if (ctx.fn && ins.operands[0] < ctx.fn->values.size()) {
                    st = ctx.fn->values[ins.operands[0]].type;
                }
                bool src_is_int = (st != ir::IrType::F32 && st != ir::IrType::F64);
                bool dst_is_int = (ins.type != ir::IrType::F32 && ins.type != ir::IrType::F64);
                std::string e = value_expr(ctx, ins.operands[0]);
                if (src_is_int && dst_is_int) {
                    return cast_for(ins.type, false) + e;
                }
                // Cross-domain bitcast: usar gcc statement expr.
                return "({ " + type_for(ins.type, false) + " __tmp; "
                       + "__builtin_memcpy(&__tmp, &(" + e + "), sizeof(__tmp)); __tmp; })";
            }

            default:
                return "v" + std::to_string(ins.dst);  // fallback
        }
    }

    // =========================================================================
    //  Prelude / postamble
    // =========================================================================

    // =========================================================================
    //  Soporte POO (clases, herencia, devirt)
    // =========================================================================

    const ir::IrClass *CBackend::lookup_class(const std::string &name) const {
        auto it = class_by_name_.find(name);
        return it == class_by_name_.end() ? nullptr : it->second;
    }

    std::string CBackend::field_name_at_offset(const ir::IrClass &cls,
                                                uint32_t offset,
                                                ir::IrType type) const {
        // Lookup lineal por offset.  Como las clases tipicas tienen pocos
        // campos (<10), O(N) es perfecto.  Si hay mas, podemos cachear un
        // mapa (offset, type) -> name.  El tipo se valida tambien para
        // evitar matches falsos en bit fields o casos raros.
        for (const auto &f : cls.fields) {
            if (f.offset == offset && f.type == type) {
                return f.name;
            }
            // Heurica: si el tipo coincide en tamano (e.g. todos i32/u32 son
            // 4 bytes pero distintos signos), aceptarlo igualmente.  GCC
            // emitira el cast correcto en el use site.
            if (f.offset == offset && f.size_bytes > 0) {
                return f.name;
            }
        }
        return "";
    }

    void CBackend::analyze_escapes(const ir::IrFunction &fn) {
        stack_alloc_candidates_.clear();

        // Identificar candidates: SSA values producidos por @c CALL a
        // @c __new_<X> donde X esta en class_by_name_.
        std::vector<ir::IrValueId> candidates;
        for (const auto &bb : fn.blocks) {
            for (const auto &ins : bb.instrs) {
                if (ins.op != ir::IrOp::CALL) continue;
                if (ins.dst == ir::IR_NO_VALUE) continue;
                if (ins.func_name.compare(0, 6, "__new_") != 0) continue;
                std::string cls_name = ins.func_name.substr(6);
                if (lookup_class(cls_name)) {
                    candidates.push_back(ins.dst);
                }
            }
        }
        if (candidates.empty()) return;

        // Para cada candidate, walk de usos y comprobar si escapa.
        // Conservativo: si CUALQUIER uso "raro" (RET / STORE como valor /
        // arg distinto del primero), NO stack-alloc.
        std::unordered_set<ir::IrValueId> cand_set(candidates.begin(),
                                                   candidates.end());

        // Walk usos.
        std::unordered_set<ir::IrValueId> escaped;
        for (const auto &bb : fn.blocks) {
            for (const auto &ins : bb.instrs) {
                using ir::IrOp;
                switch (ins.op) {
                    case IrOp::RET:
                        // Si el value retornado es candidate, escapa.
                        if (!ins.operands.empty()
                         && cand_set.count(ins.operands[0])) {
                            escaped.insert(ins.operands[0]);
                        }
                        break;
                    case IrOp::STORE:
                        // operands[0] = val (escapa si candidate), operands[1] = addr.
                        if (ins.operands.size() >= 1
                         && cand_set.count(ins.operands[0])) {
                            escaped.insert(ins.operands[0]);
                        }
                        break;
                    case IrOp::CALL:
                    case IrOp::CALLVIRT:
                    case IrOp::CALLM:
                    case IrOp::CALLN: {
                        // Permitido: candidate como PRIMER operando (this) en
                        // CALLVIRT/CALLM, o como arg en CALL a Class__ctor/dtor.
                        // No permitido: candidate como arg posterior (asume
                        // que la funcion puede guardarlo).
                        // Conservativo: si aparece en operands[1..], escapa.
                        // EXCEPCION: CALL a Class__ctor (es nuestro propio
                        // helper que no escapa this).
                        for (size_t i = 1; i < ins.operands.size(); ++i) {
                            if (cand_set.count(ins.operands[i])) {
                                escaped.insert(ins.operands[i]);
                            }
                        }
                        // operands[0] (el this o el func ptr) -- skip OK.
                        break;
                    }
                    case IrOp::PHI:
                        // Si el dst de un PHI recibe un candidate, el flow
                        // es ambiguo -> conservativo: escapa.
                        for (const auto &arg : ins.phi_args) {
                            if (cand_set.count(arg.value)) {
                                escaped.insert(arg.value);
                            }
                        }
                        break;
                    case IrOp::MOV:
                        // MOV es alias; el dst hereda. Si dst aparece despues
                        // en un escape, lo perdimos -> conservativo: escapa.
                        if (!ins.operands.empty()
                         && cand_set.count(ins.operands[0])) {
                            escaped.insert(ins.operands[0]);
                        }
                        break;
                    default:
                        break;
                }
            }
        }

        for (auto v : candidates) {
            if (!escaped.count(v)) stack_alloc_candidates_.insert(v);
        }
    }

    void CBackend::infer_concrete_types(const ir::IrFunction &fn) {
        concrete_type_.clear();

        // 1. Si la funcion es metodo (nombre @c "<Class>__<method>"),
        //    marcar el primer parametro (@c this) con tipo @c Class.
        const std::string &fn_name = fn.name;
        auto sep = fn_name.find("__");
        if (sep != std::string::npos && !fn.params.empty()) {
            std::string class_name = fn_name.substr(0, sep);
            if (lookup_class(class_name)) {
                concrete_type_[fn.params[0]] = class_name;
            }
        }

        // 2. Propagation iterativa: hasta punto fijo (raro mas de 2 pasadas).
        bool changed = true;
        size_t iter = 0;
        while (changed && iter < 4) {
            changed = false;
            iter++;
            for (const auto &bb : fn.blocks) {
                for (const auto &ins : bb.instrs) {
                    if (ins.dst == ir::IR_NO_VALUE) continue;

                    std::string new_type;

                    if (ins.op == ir::IrOp::CALL && !ins.func_name.empty()) {
                        // Si la funcion es @c __new_<X>, el resultado es X*.
                        if (ins.func_name.compare(0, 6, "__new_") == 0) {
                            new_type = ins.func_name.substr(6);
                        } else {
                            // Si la funcion es @c <Class>__<method> y el
                            // return type es PTR, no podemos inferir con
                            // certeza sin firma.  Skip por ahora.
                        }
                    } else if (ins.op == ir::IrOp::MOV) {
                        // MOV propaga el tipo.
                        if (!ins.operands.empty()) {
                            auto it = concrete_type_.find(ins.operands[0]);
                            if (it != concrete_type_.end()) new_type = it->second;
                        }
                    } else if (ins.op == ir::IrOp::PHI) {
                        // PHI: si TODOS los args tienen el mismo tipo concreto,
                        // propagar; si no, vacio.
                        bool all_same = !ins.phi_args.empty();
                        std::string first_t;
                        for (const auto &arg : ins.phi_args) {
                            auto it = concrete_type_.find(arg.value);
                            std::string t = (it == concrete_type_.end()) ? "" : it->second;
                            if (first_t.empty()) {
                                first_t = t;
                            } else if (first_t != t) {
                                all_same = false;
                                break;
                            }
                        }
                        if (all_same && !first_t.empty()) new_type = first_t;
                    } else if (ins.op == ir::IrOp::NEWOBJ) {
                        // NEWOBJ: el operando[0] es un class_ptr; sin info
                        // estatica no podemos resolver el nombre aqui.  Vex
                        // emite NEWOBJ desde __new_<X> que es donde lo
                        // detectamos via la rama CALL.
                    }
                    // BITCAST y otras conversiones a PTR no preservan tipo
                    // concreto -- el usuario expreso un cast.

                    if (!new_type.empty()) {
                        auto it = concrete_type_.find(ins.dst);
                        if (it == concrete_type_.end() || it->second != new_type) {
                            concrete_type_[ins.dst] = new_type;
                            changed = true;
                        }
                    }
                }
            }
        }
    }

    void CBackend::emit_class_decls(EmitContext &ctx, const ir::IrModule &mod) {
        if (mod.classes.empty()) return;

        // 1. Indexar clases por nombre para lookup O(1).  Llenamos
        //    class_by_name_ aqui porque infer_concrete_types lo necesita.
        class_by_name_.clear();
        for (const auto &cls : mod.classes) {
            class_by_name_[cls.name] = &cls;
        }

        // 2. Topological sort: emit super-clases antes que sub-clases.
        //    Algoritmo: Kahn's (BFS sobre dependencias).
        std::vector<std::string> emit_order;
        std::unordered_set<std::string> emitted;
        std::function<void(const std::string&)> emit_recur =
            [&](const std::string &name) {
                if (emitted.count(name)) return;
                const ir::IrClass *cls = lookup_class(name);
                if (!cls) {
                    emitted.insert(name);
                    return;
                }
                if (!cls->super_name.empty() && cls->super_name != "Object") {
                    emit_recur(cls->super_name);
                }
                emitted.insert(name);
                emit_order.push_back(name);
            };
        for (const auto &cls : mod.classes) {
            emit_recur(cls.name);
        }

        // 3. Forward typedef de todas las clases (permite punteros cruzados).
        if (opts_.emit_comments) {
            ctx.out << "/* Forward declarations de clases */\n";
        }
        for (const auto &name : emit_order) {
            const ir::IrClass *cls = lookup_class(name);
            if (!cls || cls->is_interface) continue;
            ctx.out << "typedef struct " << name << " " << name << ";\n";
        }
        ctx.out << "\n";

        // 4. Definiciones de struct (en orden topologico).
        if (opts_.emit_comments) {
            ctx.out << "/* Definiciones de struct */\n";
        }
        for (const auto &name : emit_order) {
            const ir::IrClass *cls = lookup_class(name);
            if (!cls || cls->is_interface) continue;

            ctx.out << "struct " << name << " {\n";
            // Herencia: el campo @c __base es el struct de la super
            // (struct embedding C-style).  Permite acceso transparente
            // a fields heredados via @c obj->__base.parent_field.
            if (!cls->super_name.empty() && cls->super_name != "Object"
             && lookup_class(cls->super_name)) {
                ctx.out << "    " << cls->super_name << " __base;\n";
            }
            // Fields propios (skip los heredados que ya estan en __base).
            // ClassLayout incluye los heredados en lay.fields; el offset es
            // continuo (heredados primero, propios al final).  Heredados:
            // los primeros @c inherited_field_count.  Como IrClass no
            // expone inherited_field_count, los detectamos por offset:
            // si super.size_bytes > 0 y field.offset < super.size_bytes,
            // es heredado.  Skip en ese caso.
            const ir::IrClass *super = (cls->super_name.empty() || cls->super_name == "Object")
                ? nullptr : lookup_class(cls->super_name);
            const uint32_t inherited_bound = super ? super->size_bytes : 0;

            for (const auto &f : cls->fields) {
                if (f.offset < inherited_bound) continue;  // heredado, en __base
                ctx.out << "    " << type_for(f.type, false) << " "
                        << f.name << ";\n";
            }
            ctx.out << "};\n\n";
        }

        // 5. Forward decls de metodos (incluyendo Class__new y Class__delete).
        if (opts_.emit_comments) {
            ctx.out << "/* Forward decls de metodos */\n";
        }
        for (const auto &name : emit_order) {
            const ir::IrClass *cls = lookup_class(name);
            if (!cls || cls->is_interface) continue;
            // Buscar el ctor para extraer su firma -- la forward decl de
            // @c Class__new debe coincidir con la definicion emitida en
            // @c emit_class_bodies.
            const ir::IrMethod *ctor = nullptr;
            for (const auto &m : cls->methods) {
                if (m.is_constructor) { ctor = &m; break; }
            }
            ctx.out << "static " << name << " *" << name << "__new(";
            if (ctor && !ctor->param_types.empty()) {
                for (size_t i = 0; i < ctor->param_types.size(); ++i) {
                    if (i) ctx.out << ", ";
                    ctx.out << type_for(ctor->param_types[i], false);
                }
            } else {
                ctx.out << "void";
            }
            ctx.out << ");\n";
            ctx.out << "static void " << name << "__delete("
                    << name << " *self);\n";
            for (const auto &m : cls->methods) {
                if (m.is_static) continue;
                // Solo emitir si el metodo esta DEFINIDO en esta clase.
                // (Metodos heredados sin override apuntan a la implementacion
                // de la super; no se redefinen aqui.)
                if (!m.defining_class.empty() && m.defining_class != cls->name) {
                    continue;
                }
                ctx.out << "static " << type_for(m.return_type, false) << " "
                        << cls->name << "__" << (m.is_constructor ? "ctor" : m.name)
                        << "(" << cls->name << " *self";
                for (size_t pi = 0; pi < m.param_types.size(); ++pi) {
                    ctx.out << ", " << type_for(m.param_types[pi], false)
                            << " p" << pi;
                }
                ctx.out << ");\n";
            }
        }
        ctx.out << "\n";
    }

    void CBackend::emit_class_bodies(EmitContext &ctx, const ir::IrModule &mod) {
        if (mod.classes.empty()) return;

        // Emite @c Class__new(args) (calloc + ctor) y @c Class__delete(self) (dtor + free).
        for (const auto &cls : mod.classes) {
            if (cls.is_interface) continue;

            // Localizar el constructor para extraer su firma.
            const ir::IrMethod *ctor = nullptr;
            for (const auto &m : cls.methods) {
                if (m.is_constructor) { ctor = &m; break; }
            }

            // @c Class__new(args...) : calloc + Class__ctor(self, args...).
            //  calloc da zero-init de un golpe (mas eficiente que malloc+memset).
            //  Si la clase tiene multiples ctors, solo el primero se invoca
            //  desde new -- el frontend Vex elige cual via overload resolution.
            ctx.out << "static VEX_UNUSED " << cls.name << " *" << cls.name
                    << "__new(";
            if (ctor && !ctor->param_types.empty()) {
                for (size_t i = 0; i < ctor->param_types.size(); ++i) {
                    if (i) ctx.out << ", ";
                    ctx.out << type_for(ctor->param_types[i], false)
                            << " p" << i;
                }
            } else {
                ctx.out << "void";
            }
            ctx.out << ") {\n";
            ctx.out << "    " << cls.name << " *o = ("
                    << cls.name << "*)calloc(1, sizeof(" << cls.name << "));\n";
            ctx.out << "    if (!o) return (" << cls.name << "*)0;\n";
            // Invocar el constructor del usuario (si existe).
            if (ctor) {
                ctx.out << "    " << cls.name << "__ctor(o";
                for (size_t i = 0; i < ctor->param_types.size(); ++i) {
                    ctx.out << ", p" << i;
                }
                ctx.out << ");\n";
            }
            ctx.out << "    return o;\n";
            ctx.out << "}\n\n";

            // @c Class__delete(self) : dtor (si existe) + free.
            ctx.out << "static VEX_UNUSED void " << cls.name << "__delete("
                    << cls.name << " *self) {\n";
            ctx.out << "    if (!self) return;\n";
            if (cls.has_destructor) {
                ctx.out << "    " << cls.name << "____dtor(self);\n";
            }
            ctx.out << "    free(self);\n";
            ctx.out << "}\n\n";
        }
    }

    // =========================================================================
    //  Prelude / postamble
    // =========================================================================

    void CBackend::emit_prelude(EmitContext &ctx, const ir::IrModule &mod) {
        if (!opts_.source_path.empty()) {
            ctx.out << "/*\n";
            ctx.out << " * Generated from: " << opts_.source_path << "\n";
            ctx.out << " * by VestaVM port-C transpiler\n";
            ctx.out << " * Edit at your own risk - regenerate with:\n";
            ctx.out << " *   vm --port=c <source.vex> -o " << opts_.source_path << ".c\n";
            ctx.out << " */\n\n";
        }

        ctx.out << "#include <stdint.h>\n";
        ctx.out << "#include <stdlib.h>\n";
        ctx.out << "#include <string.h>\n";
        if (opts_.exc == ExcMode::SetJmp) {
            ctx.out << "#include <setjmp.h>\n";
        }
        if (opts_.gc == GcMode::Vesta) {
            ctx.out << "#include \"vesta_rt/public.h\"\n";
            ctx.out << "#include \"vesta_rt/abi.h\"\n";
        } else if (opts_.gc == GcMode::Boehm) {
            ctx.out << "#include <gc.h>\n";
        }
        ctx.out << "\n";

        ctx.out << "#if defined(__GNUC__) || defined(__clang__)\n";
        ctx.out << "#  define VEX_RESTRICT __restrict__\n";
        ctx.out << "#  define VEX_HOT      __attribute__((hot))\n";
        ctx.out << "#  define VEX_COLD     __attribute__((cold))\n";
        ctx.out << "#  define VEX_UNUSED   __attribute__((unused))\n";
        ctx.out << "#  define VEX_NORETURN __attribute__((noreturn))\n";
        ctx.out << "#  define VEX_UNREACHABLE __builtin_unreachable()\n";
        ctx.out << "#else\n";
        ctx.out << "#  define VEX_RESTRICT\n";
        ctx.out << "#  define VEX_HOT\n";
        ctx.out << "#  define VEX_COLD\n";
        ctx.out << "#  define VEX_UNUSED\n";
        ctx.out << "#  define VEX_NORETURN\n";
        ctx.out << "#  define VEX_UNREACHABLE ((void)0)\n";
        ctx.out << "#endif\n\n";

        // Solo emitir vex_throw si el modulo TIENE alguna IR op de excepcion.
        // (Phase A: deteccion conservativa -- siempre emitir mientras se decide
        //  el modelo final.)  Marcamos como VEX_UNUSED para silenciar warning.
        if (opts_.exc == ExcMode::SetJmp) {
            ctx.out << "/* Manejo de excepciones via setjmp/longjmp */\n";
            ctx.out << "static jmp_buf vex_exc_buf;\n";
            ctx.out << "static int     vex_exc_code;\n";
            ctx.out << "static VEX_UNUSED VEX_NORETURN void vex_throw(int code) {\n";
            ctx.out << "    vex_exc_code = code;\n";
            ctx.out << "    longjmp(vex_exc_buf, 1);\n";
            ctx.out << "}\n\n";
        } else if (opts_.exc == ExcMode::None) {
            ctx.out << "static VEX_UNUSED VEX_NORETURN void vex_throw(int code) {\n";
            ctx.out << "    (void)code; abort();\n";
            ctx.out << "}\n\n";
        }

        // Emitir clases (structs + forward decls de metodos) ANTES de los
        // forward decls de funciones libres, asi los typedefs estan listos.
        emit_class_decls(ctx, mod);

        if (opts_.emit_comments) {
            ctx.out << "/* Forward declarations de funciones libres */\n";
        }
        for (const auto &fn : mod.functions) {
            if (fn.is_native) continue;
            // Skip funciones POO que ya se declararon en emit_class_decls.
            // Patron: @c "<Class>__<method>" o @c "<Class>____dtor".
            const std::string &n = fn.name;
            auto sep = n.find("__");
            if (sep != std::string::npos && lookup_class(n.substr(0, sep))) {
                continue;  // ya forward-decl en emit_class_decls
            }
            // Skip @c __module_init: el transpiler emite la inicializacion
            // de clases via @c Class__new y constructores estaticos cuando
            // sea necesario.  El runtime de VestaVM lo necesita; C no.
            if (n == "__module_init") continue;
            // Skip @c __new_<X>: emitido por @c emit_class_bodies.
            if (n.compare(0, 6, "__new_") == 0
             && lookup_class(n.substr(6))) {
                continue;
            }
            ctx.out << type_for(fn.ret_type, false) << " "
                    << sanitize_name(fn.name) << "(";
            if (fn.params.empty()) {
                ctx.out << "void";
            } else {
                for (size_t i = 0; i < fn.params.size(); ++i) {
                    if (i) ctx.out << ", ";
                    ir::IrValueId vid = fn.params[i];
                    if (vid < fn.values.size()) {
                        const auto &v = fn.values[vid];
                        ctx.out << type_for(v.type, v.is_host_ptr);
                    } else {
                        ctx.out << "int64_t";
                    }
                }
            }
            ctx.out << ");\n";
        }
        ctx.out << "\n";

        // Cuerpos de @c Class__new y @c Class__delete (helpers de POO).
        // Las funciones de usuario (Class__method) son IrFunctions normales
        // que emit_function se encarga de procesar.
        emit_class_bodies(ctx, mod);
    }

    bool CBackend::should_skip_function(const ir::IrFunction &fn,
                                        const ir::IrModule &mod) const {
        (void)mod;
        const std::string &n = fn.name;
        // 1. @c __module_init: el runtime VestaVM lo usa para registrar
        //    clases dinamicamente.  En C standalone con structs estaticos
        //    no se necesita -- las clases son literales.  Skip.
        if (n == "__module_init") return true;
        // 2. @c __new_<X>: reemplazado por @c X__new del backend (definido
        //    en emit_class_bodies).  El user code que llamaba a __new_<X>
        //    se redirige a X__new en emit_call.
        if (n.compare(0, 6, "__new_") == 0) {
            std::string class_name = n.substr(6);
            if (lookup_class(class_name)) return true;
        }
        return false;
    }

    void CBackend::emit_postamble(EmitContext &ctx, const ir::IrModule &mod) {
        bool has_main = false;
        ir::IrType main_ret = ir::IrType::VOID;
        for (const auto &fn : mod.functions) {
            if (fn.name == "main") {
                has_main = true;
                main_ret = fn.ret_type;
                break;
            }
        }
        if (!has_main) return;
        ctx.out << "\n/* Entry point wrapper */\n";
        ctx.out << "int main(int argc, char **argv) {\n";
        ctx.out << "    (void)argc; (void)argv;\n";
        if (opts_.exc == ExcMode::SetJmp) {
            ctx.out << "    if (setjmp(vex_exc_buf) != 0) return vex_exc_code;\n";
        }
        if (main_ret == ir::IrType::VOID) {
            ctx.out << "    vex_main();\n";
            ctx.out << "    return 0;\n";
        } else {
            ctx.out << "    return (int)vex_main();\n";
        }
        ctx.out << "}\n";
    }

    // =========================================================================
    //  Firma + locales
    // =========================================================================

    void CBackend::emit_fn_signature(EmitContext &ctx, const ir::IrFunction &fn) {
        // Pre-pasada de inferencia de tipos concretos para esta funcion.
        // Llenamos @c concrete_type_ con el class_name de cada SSA value
        // que conocemos.  Lo usaremos para devirtualizar CALLVIRT y para
        // emitir field access tipados.
        infer_concrete_types(fn);
        // Pre-pasada de escape analysis: identifica @c __new_<X> que NO
        // escapan -> safe para stack alloc.  Big perf win vs heap.
        analyze_escapes(fn);
        // Reset RAII tracking: cada funcion empieza con su propia lista
        // de stack allocs.  Se llena durante @c emit_call y se drena en
        // @c emit_return (LIFO).
        stack_alloc_in_fn_.clear();

        // Si la funcion es un metodo @c Class__name, el primer parametro
        // es @c Class *self (no void*).  Tambien arriva el ctor (Class__ctor).
        bool is_method = false;
        std::string method_class;
        {
            auto sep = fn.name.find("__");
            if (sep != std::string::npos && !fn.params.empty()) {
                std::string class_name = fn.name.substr(0, sep);
                if (lookup_class(class_name)) {
                    is_method = true;
                    method_class = class_name;
                }
            }
        }

        // Detectar "static" para metodos (para mantener simbolos privados al
        // archivo, evitando colisiones cross-modulo en builds futuros).
        if (is_method) ctx.out << "static ";

        ctx.out << type_for(fn.ret_type, false) << " "
                << sanitize_name(fn.name) << "(";
        if (fn.params.empty()) {
            ctx.out << "void";
        } else {
            for (size_t i = 0; i < fn.params.size(); ++i) {
                if (i) ctx.out << ", ";
                ir::IrValueId vid = fn.params[i];
                if (vid < fn.values.size()) {
                    const auto &v = fn.values[vid];
                    // Si es metodo Y este es el primer param (this) -> tipo concreto.
                    if (is_method && i == 0) {
                        ctx.out << method_class << " *";
                        if (opts_.emit_compiler_hints) {
                            ctx.out << "VEX_RESTRICT ";
                        }
                        ctx.out << "v" << vid;
                        continue;
                    }
                    std::string t = type_for(v.type, v.is_host_ptr);
                    ctx.out << t << " ";
                    if (v.is_host_ptr && opts_.emit_compiler_hints) {
                        ctx.out << "VEX_RESTRICT ";
                    }
                    ctx.out << "v" << vid;
                } else {
                    ctx.out << "int64_t v" << vid;
                }
            }
        }
        ctx.out << ")";
    }

    void CBackend::emit_local_decl(EmitContext &ctx,
                                   ir::IrValueId id,
                                   const ir::IrValue &value) {
        ctx.indent();
        // Si conocemos el tipo concreto del value (por SSA propagation),
        // emitir @c "ClassX *" en lugar de @c "void *".  Permite al
        // compilador host devirtualizar las llamadas de metodo y al lector
        // humano ver de un vistazo el flujo de tipos.
        auto it = concrete_type_.find(id);
        if (it != concrete_type_.end() && lookup_class(it->second)) {
            ctx.out << it->second << " *v" << id << ";";
        } else {
            ctx.out << type_for(value.type, value.is_host_ptr) << " v" << id << ";";
        }
        if (opts_.emit_comments && !value.name.empty()) {
            ctx.out << "  /* " << value.name << " */";
        }
        ctx.out << "\n";
    }

    // =========================================================================
    //  Helpers
    // =========================================================================

    void CBackend::emit_assign_lhs(EmitContext &ctx, ir::IrValueId dst) const {
        ctx.indent();
        ctx.out << "v" << dst << " = ";
    }

    // =========================================================================
    //  Control flow
    // =========================================================================

    void CBackend::emit_return(EmitContext &ctx, ir::IrValueId val) {
        // NOTA: NO emitimos cleanups RAII aqui porque el frontend Vex YA
        // inserta @c callvirt %obj, vtbl_idx_dtor() en el IR antes del
        // RET para cada objeto con destructor en scope (modelo "destructor
        // virtual" del propio Vex).  @c emit_callvirt los devirtualiza a
        // @c Class____dtor(obj) directos.  Si emitieramos aqui ademas, el
        // objeto se destruiria dos veces.
        ctx.indent();
        if (val == ir::IR_NO_VALUE) {
            ctx.out << "return;\n";
        } else {
            // Usar value_expr para que constantes y expresiones simples se
            // inlinean directamente en el @c return.
            ctx.out << "return " << value_expr(ctx, val) << ";\n";
        }
    }

    void CBackend::emit_cond_branch(EmitContext &ctx,
                                    ir::IrValueId cond,
                                    ir::IrBlockId true_id,
                                    ir::IrBlockId false_id) {
        // Override del default para usar value_expr (que puede inlinear
        // un CMP single-use en la propia condicion del if).
        ctx.indent();
        ctx.out << "if (" << value_expr(ctx, cond) << ") goto "
                << label_for(true_id) << ";\n";
        ctx.indent();
        ctx.out << "goto " << label_for(false_id) << ";\n";
    }

    void CBackend::emit_phi_copy(EmitContext &ctx,
                                 ir::IrValueId dst,
                                 ir::IrValueId src,
                                 ir::IrType t) {
        (void)t;
        // Emite la copia PHI con la expresion del src (que puede ser un
        // inline candidate -- entonces se substituye por la expresion
        // completa en lugar de @c v<id>).  El destino siempre es una
        // variable declarada (los PHIs nunca son inline candidates porque
        // su uso esta en el bloque del PHI y la def en los predecesores).
        ctx.indent();
        ctx.out << "v" << dst << " = " << value_expr(ctx, src) << ";\n";
    }

    // =========================================================================
    //  Constantes / Mov
    // =========================================================================

    void CBackend::emit_const(EmitContext &ctx,
                              ir::IrValueId dst, uint64_t imm, ir::IrType t) {
        // Skip si el value es inline candidate -- su expresion se construye
        // perezosamente en el use site via @c value_expr.
        if (ctx.tx && ctx.tx->is_inline_candidate(dst)) return;
        emit_assign_lhs(ctx, dst);
        ctx.out << format_const_literal(imm, t) << ";\n";
    }

    void CBackend::emit_mov(EmitContext &ctx,
                            ir::IrValueId dst, ir::IrValueId src, ir::IrType t) {
        (void)t;
        if (ctx.tx && ctx.tx->is_inline_candidate(dst)) return;
        emit_assign_lhs(ctx, dst);
        ctx.out << value_expr(ctx, src) << ";\n";
    }

    // =========================================================================
    //  Binarias + unarias
    // =========================================================================

    void CBackend::emit_binop(EmitContext &ctx, ir::IrOp op,
                              ir::IrValueId dst, ir::IrValueId lhs,
                              ir::IrValueId rhs, ir::IrType t) {
        if (ctx.tx && ctx.tx->is_inline_candidate(dst)) return;
        emit_assign_lhs(ctx, dst);
        // Pointer arithmetic: byte-wise, cast lhs a char* (ver build_inline_expr).
        if (t == ir::IrType::PTR && (op == ir::IrOp::ADD || op == ir::IrOp::SUB)) {
            const char *sym = (op == ir::IrOp::ADD) ? "+" : "-";
            ctx.out << "(void*)((char*)" << value_expr(ctx, lhs) << " " << sym
                    << " " << value_expr(ctx, rhs) << ");\n";
            return;
        }
        if (op == ir::IrOp::FMIN || op == ir::IrOp::FMAX) {
            const char *cmp = (op == ir::IrOp::FMIN) ? "<" : ">";
            std::string l = value_expr(ctx, lhs);
            std::string r = value_expr(ctx, rhs);
            ctx.out << "(" << l << " " << cmp << " " << r
                    << " ? " << l << " : " << r << ");\n";
            return;
        }
        if (op == ir::IrOp::SHR) {
            std::string utype;
            switch (t) {
                case ir::IrType::I8:  utype = "uint8_t";  break;
                case ir::IrType::I16: utype = "uint16_t"; break;
                case ir::IrType::I32: utype = "uint32_t"; break;
                case ir::IrType::I64: utype = "uint64_t"; break;
                default: utype = type_for(t, false); break;
            }
            ctx.out << "(" << type_for(t, false) << ")((" << utype << ")"
                    << value_expr(ctx, lhs) << " >> "
                    << value_expr(ctx, rhs) << ");\n";
            return;
        }
        if (op == ir::IrOp::SAR) {
            std::string stype;
            switch (t) {
                case ir::IrType::U8:  stype = "int8_t";  break;
                case ir::IrType::U16: stype = "int16_t"; break;
                case ir::IrType::U32: stype = "int32_t"; break;
                case ir::IrType::U64: stype = "int64_t"; break;
                default: stype = type_for(t, false); break;
            }
            ctx.out << "(" << type_for(t, false) << ")((" << stype << ")"
                    << value_expr(ctx, lhs) << " >> "
                    << value_expr(ctx, rhs) << ");\n";
            return;
        }
        ctx.out << value_expr(ctx, lhs) << " " << binop_symbol_for(op)
                << " " << value_expr(ctx, rhs) << ";\n";
    }

    void CBackend::emit_unop(EmitContext &ctx, ir::IrOp op,
                             ir::IrValueId dst, ir::IrValueId src, ir::IrType t) {
        if (ctx.tx && ctx.tx->is_inline_candidate(dst)) return;
        emit_assign_lhs(ctx, dst);
        switch (op) {
            case ir::IrOp::NEG: case ir::IrOp::FNEG:
                ctx.out << "-" << value_expr(ctx, src) << ";\n";
                break;
            case ir::IrOp::NOT:
                ctx.out << "~" << value_expr(ctx, src) << ";\n";
                break;
            case ir::IrOp::FABS:
                if (t == ir::IrType::F32) {
                    ctx.out << "__builtin_fabsf(" << value_expr(ctx, src) << ");\n";
                } else {
                    ctx.out << "__builtin_fabs(" << value_expr(ctx, src) << ");\n";
                }
                break;
            case ir::IrOp::FSQRT:
                if (t == ir::IrType::F32) {
                    ctx.out << "__builtin_sqrtf(" << value_expr(ctx, src) << ");\n";
                } else {
                    ctx.out << "__builtin_sqrt(" << value_expr(ctx, src) << ");\n";
                }
                break;
            default:
                ctx.out << value_expr(ctx, src) << "; /* unknown unop */\n";
                break;
        }
    }

    void CBackend::emit_cmp(EmitContext &ctx, ir::IrOp op,
                            ir::IrValueId dst, ir::IrValueId lhs,
                            ir::IrValueId rhs, ir::IrType operand_type) {
        if (ctx.tx && ctx.tx->is_inline_candidate(dst)) return;
        emit_assign_lhs(ctx, dst);
        bool is_unsigned = false;
        const char *cmp = cmp_symbol_for(op, is_unsigned);
        if (is_unsigned) {
            std::string utype;
            switch (operand_type) {
                case ir::IrType::I8:  utype = "uint8_t";  break;
                case ir::IrType::I16: utype = "uint16_t"; break;
                case ir::IrType::I32: utype = "uint32_t"; break;
                case ir::IrType::I64: utype = "uint64_t"; break;
                default: utype = type_for(operand_type, false); break;
            }
            ctx.out << "((" << utype << ")" << value_expr(ctx, lhs) << " " << cmp
                    << " (" << utype << ")" << value_expr(ctx, rhs) << ");\n";
        } else {
            ctx.out << "(" << value_expr(ctx, lhs) << " " << cmp
                    << " " << value_expr(ctx, rhs) << ");\n";
        }
    }

    void CBackend::emit_convert(EmitContext &ctx, ir::IrOp op,
                                ir::IrValueId dst, ir::IrValueId src,
                                ir::IrType dst_type, ir::IrType src_type) {
        if (ctx.tx && ctx.tx->is_inline_candidate(dst)) return;
        emit_assign_lhs(ctx, dst);
        switch (op) {
            case ir::IrOp::BITCAST: {
                bool src_is_int  = (src_type != ir::IrType::F32 && src_type != ir::IrType::F64);
                bool dst_is_int  = (dst_type != ir::IrType::F32 && dst_type != ir::IrType::F64);
                if (src_is_int && dst_is_int) {
                    ctx.out << cast_for(dst_type, false) << value_expr(ctx, src) << ";\n";
                } else {
                    std::string e = value_expr(ctx, src);
                    ctx.out << "({ " << type_for(dst_type, false) << " __tmp; "
                            << "__builtin_memcpy(&__tmp, &(" << e << "), "
                            << "sizeof(__tmp)); __tmp; });\n";
                }
                return;
            }
            case ir::IrOp::ITOF: case ir::IrOp::UITOF:
            case ir::IrOp::FTOI: case ir::IrOp::FTOUI:
            case ir::IrOp::F32TOF64: case ir::IrOp::F64TOF32:
            case ir::IrOp::CAST: case ir::IrOp::TRUNC: {
                ctx.out << cast_for(dst_type, false) << value_expr(ctx, src) << ";\n";
                return;
            }
            case ir::IrOp::ZEXT: {
                std::string utype;
                switch (src_type) {
                    case ir::IrType::I8:  utype = "uint8_t";  break;
                    case ir::IrType::I16: utype = "uint16_t"; break;
                    case ir::IrType::I32: utype = "uint32_t"; break;
                    case ir::IrType::I64: utype = "uint64_t"; break;
                    default: utype = type_for(src_type, false); break;
                }
                ctx.out << cast_for(dst_type, false)
                        << "(" << utype << ")" << value_expr(ctx, src) << ";\n";
                return;
            }
            case ir::IrOp::SEXT: {
                std::string stype;
                switch (src_type) {
                    case ir::IrType::U8:  stype = "int8_t";  break;
                    case ir::IrType::U16: stype = "int16_t"; break;
                    case ir::IrType::U32: stype = "int32_t"; break;
                    case ir::IrType::U64: stype = "int64_t"; break;
                    default: stype = type_for(src_type, false); break;
                }
                ctx.out << cast_for(dst_type, false)
                        << "(" << stype << ")" << value_expr(ctx, src) << ";\n";
                return;
            }
            default:
                ctx.out << cast_for(dst_type, false) << value_expr(ctx, src) << ";\n";
                return;
        }
    }

    // =========================================================================
    //  Memoria
    // =========================================================================

    void CBackend::emit_alloca(EmitContext &ctx,
                               ir::IrValueId dst, uint64_t size_bytes) {
        ctx.indent();
        ctx.out << "{\n";
        ctx.indent_level++;
        ctx.indent();
        ctx.out << "static uint8_t __alloca_storage_" << dst
                << "[" << size_bytes << "];\n";
        ctx.indent();
        ctx.out << "v" << dst << " = (void*)__alloca_storage_" << dst << ";\n";
        ctx.indent_level--;
        ctx.indent();
        ctx.out << "}\n";
    }

    /**
     * @brief Intenta reconocer el patron @c "ADD base + const_offset" como
     *        acceso a un field de una clase con tipo concreto conocido.
     * @return Cadena C @c "base->fieldname" si aplica, vacio en caso contrario.
     *
     * Esta optimizacion convierte aritmetica byte-wise generica:
     *   @c *(int32_t*)((char*)v0 + 24)
     * en acceso tipado:
     *   @c v0->count
     *
     * Ventajas:
     *   - Codigo C legible (igual a lo escrito a mano).
     *   - GCC hace mejor alias analysis (sabe el tipo del field).
     *   - Habilita devirtualizacion downstream.
     *   - Compatible con @c -Wstrict-aliasing (no rompe TBAA).
     */
    std::string CBackend::try_match_field_access(EmitContext &ctx,
                                                  ir::IrValueId addr,
                                                  ir::IrType type) const {
        (void)type;
        if (!ctx.tx) return "";
        const ir::IrInstr *def = ctx.tx->def_of(addr);
        if (!def) return "";
        if (def->op != ir::IrOp::ADD) return "";
        if (def->operands.size() < 2) return "";
        if (def->type != ir::IrType::PTR) return "";

        ir::IrValueId base = def->operands[0];
        ir::IrValueId off  = def->operands[1];

        const ir::IrInstr *off_def = ctx.tx->def_of(off);
        if (!off_def || off_def->op != ir::IrOp::CONST) return "";
        uint64_t offset = off_def->imm;

        auto it = concrete_type_.find(base);
        if (it == concrete_type_.end()) return "";
        const ir::IrClass *cls = lookup_class(it->second);
        if (!cls) return "";

        // Buscar field por offset en coordenadas del IR (que incluyen el
        // ObjectHeader de 24 bytes que NO emitimos en Mode TRIVIAL).
        // Walking inheritance chain: si @c f.offset < super.size_bytes,
        // el field vive en @c __base; iterar recursivamente con la super
        // para construir @c "base->__base.field" o @c "base->__base.__base.field".
        std::string base_expr = value_expr(ctx, base);
        std::string suffix;
        const ir::IrClass *cur = cls;
        while (cur != nullptr) {
            // Si super existe y el offset cae dentro de su layout,
            // bajar un nivel.
            const ir::IrClass *super = (cur->super_name.empty()
                                        || cur->super_name == "Object")
                ? nullptr : lookup_class(cur->super_name);
            if (super != nullptr && offset < super->size_bytes) {
                suffix += "->__base";
                cur = super;
                continue;
            }
            // No baja mas; buscar field directamente en @c cur.
            for (const auto &f : cur->fields) {
                if (f.offset == offset) {
                    return base_expr + suffix + (suffix.empty() ? "->" : ".")
                           + f.name;
                }
            }
            break;
        }
        return "";
    }

    void CBackend::emit_load(EmitContext &ctx,
                             ir::IrValueId dst, ir::IrValueId addr,
                             ir::IrType t, bool is_host_ptr) {
        (void)is_host_ptr;
        emit_assign_lhs(ctx, dst);
        // Pattern-match field access: @c "ADD base + const_N" donde base
        // tiene tipo concreto -> emitir @c "base->fieldname" tipado.
        // Si el patron no aplica, emit el load generico con cast.
        std::string field_expr = try_match_field_access(ctx, addr, t);
        if (!field_expr.empty()) {
            ctx.out << field_expr << ";\n";
            return;
        }
        ctx.out << "*(" << type_for(t, false) << "*)"
                << value_expr(ctx, addr) << ";\n";
    }

    void CBackend::emit_store(EmitContext &ctx,
                              ir::IrValueId val, ir::IrValueId addr,
                              ir::IrType t, bool is_host_ptr) {
        (void)is_host_ptr;
        ctx.indent();
        std::string field_expr = try_match_field_access(ctx, addr, t);
        if (!field_expr.empty()) {
            ctx.out << field_expr << " = " << cast_for(t, false)
                    << value_expr(ctx, val) << ";\n";
            return;
        }
        ctx.out << "*(" << type_for(t, false) << "*)" << value_expr(ctx, addr)
                << " = " << cast_for(t, false) << value_expr(ctx, val) << ";\n";
    }

    void CBackend::emit_raw_alloc(EmitContext &ctx,
                                  ir::IrValueId dst, ir::IrValueId size) {
        emit_assign_lhs(ctx, dst);
        if (opts_.gc == GcMode::Boehm) {
            ctx.out << "GC_malloc((size_t)" << value_expr(ctx, size) << ");\n";
        } else {
            ctx.out << "malloc((size_t)" << value_expr(ctx, size) << ");\n";
        }
    }

    void CBackend::emit_raw_free(EmitContext &ctx, ir::IrValueId ptr) {
        ctx.indent();
        if (opts_.gc == GcMode::Boehm) {
            ctx.out << "(void)" << value_expr(ctx, ptr) << "; /* Boehm: no-op */\n";
        } else {
            ctx.out << "free(" << value_expr(ctx, ptr) << ");\n";
        }
    }

    // =========================================================================
    //  Llamadas
    // =========================================================================

    void CBackend::emit_call(EmitContext &ctx,
                             ir::IrValueId dst,
                             const std::string &func_name,
                             const std::vector<ir::IrValueId> &args,
                             ir::IrType ret_type) {
        // Skip llamadas a @c __module_init: la funcion no se emite (es para
        // VestaVM runtime).  Las clases en C son literales con structs
        // estaticos -- nada que inicializar dinamicamente.
        if (func_name == "__module_init") {
            if (opts_.emit_comments) {
                ctx.indent();
                ctx.out << "/* __module_init() omitido: clases son literales en C */\n";
            }
            return;
        }

        // Detect early: es esta llamada a @c __new_<X> con escape-free dst?
        // Si si, emitir STACK ALLOC en lugar de heap.  Tiene que decidirse
        // ANTES de emit_assign_lhs para evitar @c "v0 = Counter __stk..." rota.
        std::string new_class_name;
        if (func_name.compare(0, 6, "__new_") == 0) {
            std::string class_name = func_name.substr(6);
            if (lookup_class(class_name)) {
                new_class_name = class_name;
                if (dst != ir::IR_NO_VALUE) {
                    concrete_type_[dst] = class_name;
                }
            }
        }

        if (!new_class_name.empty()
         && dst != ir::IR_NO_VALUE
         && stack_alloc_candidates_.count(dst)) {
            // === STACK ALLOC PATH ===
            //   Class __stk_<dst> = {0};
            //   Class__ctor(&__stk_<dst>, args...);
            //   Class *v<dst> = &__stk_<dst>;
            // GCC -O3 hace SROA y elimina __stk_<dst> -- equivalente o
            // mejor que C++ stack alloc.
            const ir::IrClass *cls = lookup_class(new_class_name);
            ctx.indent();
            ctx.out << new_class_name << " __stk_" << dst << " = {0};\n";
            bool has_ctor = false;
            if (cls) {
                for (const auto &m : cls->methods) {
                    if (m.is_constructor) { has_ctor = true; break; }
                }
            }
            if (has_ctor) {
                ctx.indent();
                ctx.out << new_class_name << "__ctor(&__stk_" << dst;
                for (auto a : args) {
                    ctx.out << ", " << value_expr(ctx, a);
                }
                ctx.out << ");\n";
            }
            emit_assign_lhs(ctx, dst);
            ctx.out << "&__stk_" << dst << ";\n";
            // Si la clase tiene destructor, registrar para LIFO cleanup
            // al exit del scope (RAII).  Si no, omitir (no-op evita
            // ruido en el output C).
            if (cls && cls->has_destructor) {
                stack_alloc_in_fn_.emplace_back(dst, new_class_name);
            }
            return;
        }

        // === HEAP ALLOC / NORMAL CALL PATH ===
        if (ret_type != ir::IrType::VOID && dst != ir::IR_NO_VALUE) {
            emit_assign_lhs(ctx, dst);
        } else {
            ctx.indent();
        }
        std::string callee = new_class_name.empty() ? func_name
                                                    : new_class_name + "__new";
        ctx.out << sanitize_name(callee) << "(";
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) ctx.out << ", ";
            ctx.out << value_expr(ctx, args[i]);
        }
        ctx.out << ");\n";
    }

    void CBackend::emit_callvirt(EmitContext &ctx,
                                  ir::IrValueId dst,
                                  ir::IrValueId obj,
                                  uint32_t vtable_idx,
                                  const std::vector<ir::IrValueId> &args,
                                  ir::IrType ret_type) {
        // Estrategia: SIEMPRE intentar devirtualizar.  Si el tipo concreto
        // del receiver es conocido (via @c concrete_type_), buscar el
        // metodo en @c cls.methods por vtable_index y emitir DIRECT CALL.
        // Si no se puede determinar, caer a vtable lookup (Mode VIRTUAL).
        //
        // En Mode TRIVIAL (sin vtable), si no podemos devirtualizar,
        // emitimos error -- el modulo no clasifica como TRIVIAL en tal caso.
        std::string class_name;
        auto it = concrete_type_.find(obj);
        if (it != concrete_type_.end()) {
            class_name = it->second;
        }

        const ir::IrClass *cls = lookup_class(class_name);
        if (cls != nullptr) {
            // Buscar metodo con @c vtable_index = vtable_idx.
            const ir::IrMethod *method = nullptr;
            for (const auto &m : cls->methods) {
                if (m.vtable_index == static_cast<int32_t>(vtable_idx)) {
                    method = &m;
                    break;
                }
            }
            if (method) {
                // DEVIRTUALIZED: emit direct call.
                if (ret_type != ir::IrType::VOID && dst != ir::IR_NO_VALUE) {
                    emit_assign_lhs(ctx, dst);
                } else {
                    ctx.indent();
                }
                // Si el metodo esta definido en una super-clase (heredado
                // puro, no override), el simbolo @c ir_fn_name pertenece
                // a esa super.  El receiver @c obj es de tipo @c cls;
                // necesitamos pasar @c &obj->__base[->__base...] para que
                // el tipo del puntero coincida con el del parametro de la
                // super.  Construimos la cadena caminando @c super_name
                // hasta @c defining_class.
                std::string this_expr = value_expr(ctx, obj);
                if (!method->defining_class.empty()
                    && method->defining_class != cls->name) {
                    std::string chain = "&" + this_expr;
                    const ir::IrClass *cur = cls;
                    while (cur != nullptr
                        && cur->name != method->defining_class) {
                        chain += "->__base";
                        if (cur->super_name.empty()
                         || cur->super_name == "Object") break;
                        cur = lookup_class(cur->super_name);
                    }
                    this_expr = chain;
                }
                ctx.out << method->ir_fn_name << "(" << this_expr;
                for (size_t i = 0; i < args.size(); ++i) {
                    ctx.out << ", " << value_expr(ctx, args[i]);
                }
                ctx.out << ");\n";
                // Trackear tipo concreto del resultado si es un metodo que
                // devuelve PTR (devolvera *otra* clase o la misma; sin
                // metadata adicional no podemos saber, pero podemos
                // propagar conservadoramente vacio).
                return;
            }
        }

        // FALLBACK: no podemos devirtualizar.  En Mode VIRTUAL emitiriamos
        // vtable lookup, pero todavia no tenemos infraestructura.  Por
        // ahora emit un comentario indicando que se requiere Mode VIRTUAL.
        ctx.indent();
        ctx.out << "/* CALLVIRT no devirtualizable (idx=" << vtable_idx
                << ", obj=v" << obj
                << "): se requiere Mode VIRTUAL con vtable */\n";
        if (dst != ir::IR_NO_VALUE) {
            emit_assign_lhs(ctx, dst);
            ctx.out << "0;\n";
        }
    }

    void CBackend::emit_call_indirect(EmitContext &ctx,
                                      ir::IrValueId dst,
                                      ir::IrValueId fn_ptr,
                                      const std::vector<ir::IrValueId> &args,
                                      ir::IrType ret_type) {
        if (ret_type != ir::IrType::VOID && dst != ir::IR_NO_VALUE) {
            emit_assign_lhs(ctx, dst);
        } else {
            ctx.indent();
        }
        ctx.out << "((" << type_for(ret_type, false) << "(*)(";
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) ctx.out << ", ";
            ctx.out << "uintptr_t";
        }
        ctx.out << "))" << value_expr(ctx, fn_ptr) << ")(";
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) ctx.out << ", ";
            ctx.out << "(uintptr_t)" << value_expr(ctx, args[i]);
        }
        ctx.out << ");\n";
    }

} // namespace port
