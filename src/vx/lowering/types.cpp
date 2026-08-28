/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vx/lowering/types.cpp
 * @brief De los tipos de Vesta a los del IR, y las conversiones entre ellos.
 *
 * El lenguaje tiene mas tipos de los que el IR distingue: donde el usuario
 * escribe un enum, una vista, un opcional o una clase, abajo hay enteros y
 * punteros.  Traducir uno a otro es la primera mitad del fichero, y no es una
 * tabla: hay que saber cuanto ocupa cada cosa, si viaja por valor o por
 * direccion, y si lo que se declaro es una vista sobre bytes ajenos.
 *
 * La otra mitad es convertir un valor de un tipo a otro, que es donde el
 * lenguaje promete cosas que la maquina no da sola: recortar a menos bits,
 * ensanchar rellenando con el signo o con ceros, pasar de entero a coma
 * flotante y al reves.  Elegir mal ahi no da un error, da un numero distinto.
 */
#include "util/env_flags.h"
#include "vx/lowering.h"
#include "util/thread_slot.h" // el estado por hilo NO va en thread_local
#include "ir/ir_type_info.h" // vocabulario UNICO de anchura/clase de un IrType
#include "loader/oop_types.h" // ADVICE_*: el orden de la cadena
#include <algorithm>
#include <chrono>
#include <iostream>
#include "ffi/virtual_lib_registry.h" // lookup_virtual_fn (bug 161: MC.23)
#include "vx/asm/asm_effects.h"       // inferencia de clobbers ( AS inc.4)
#include "vx/asm/asm_diag.h"      // diagnosticos estructurales del asm (ASA.2)
#include "vx/asm/asm_lift_emit.h" // lift de patrones atomicos a IR tipado (ASA.3)
#include "vx/asm/asm_lift_general.h" // lift general straight-line entero a IR real
#include "vx/asm/asm_lift_micro.h"
#include "vx/asm/asm_lift_registro.h"
#include "vx/asm/asm_phys_reg.h" // asm_body_subst_greedy // lift de asm opaco sin operandos -> ASM_MICRO
#include "vx/asm/instr_db.h"    // reschedule_asm (reoptimizador de asm, ASA)
#include "vx/asm/asm_backend.h" // validacion de sintaxis via Keystone (inc.4b)
#include "vx/collection_intrinsics.h"        // tabla de tipos coleccion
#include "vx/comptime/comptime_introspect.h" // helpers compartidos rama A
#include "vx/generics/concepts.h"      // conceptos como predicado -> CONST bool
#include "vx/generics/generic_clone.h" // clone_expr (custom print to_string)
#include "vx/lexer.h"                  // parse de fragments para @Macro
#include "vx/parser.h"                 // parse_one_expr para @Macro
#include "ir/ir_optimizer.h"           // register_pure_new_helper
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <utility>
#include "lowering_internal.h" // la cocina compartida del lowering

namespace vx {

ir::IrType Lowering::ir_type_from_primitive(PrimitiveKind p) noexcept {
    // Mapeo directo.  Se usa una tabla constexpr indexada por enum
    // (PrimitiveKind y IrType comparten posiciones logicas pero los
    // valores numericos no coinciden, asi que este switch es la
    // version mantenible).
    switch (p) {
    case PrimitiveKind::VOID: return ir::IrType::VOID;
    case PrimitiveKind::BOOL: return ir::IrType::BOOL;
    // CHAR no tiene contraparte directa en ir::IrType; mapeamos a U8
    // (mismo ancho, semanticamente equivalente porque el frontend
    // solo lo usa como entero pequenyo sin codepoint awareness).
    case PrimitiveKind::CHAR: return ir::IrType::U8;
    case PrimitiveKind::I8: return ir::IrType::I8;
    case PrimitiveKind::I16: return ir::IrType::I16;
    case PrimitiveKind::I32: return ir::IrType::I32;
    case PrimitiveKind::I64: return ir::IrType::I64;
    case PrimitiveKind::U8: return ir::IrType::U8;
    case PrimitiveKind::U16: return ir::IrType::U16;
    case PrimitiveKind::U32: return ir::IrType::U32;
    case PrimitiveKind::U64: return ir::IrType::U64;
    case PrimitiveKind::F32: return ir::IrType::F32;
    case PrimitiveKind::F64: return ir::IrType::F64;
    case PrimitiveKind::PTR: return ir::IrType::PTR;
    // Para STRUCT y ARRAY no hay un IrType directo; en el lowering
    // ambos se representan via su PUNTERO base (PTR).  Cuando un
    // caller pasa STRUCT/ARRAY a este helper esperando un IrType
    // unitario, devolvemos PTR como aproximacion mas razonable.
    case PrimitiveKind::STRUCT: return ir::IrType::PTR;
    case PrimitiveKind::ARRAY: return ir::IrType::PTR;
    // CLASS es reference type: la "variable" guarda un puntero a
    // ObjectHeader, asi que el IrType subyacente es PTR.
    case PrimitiveKind::CLASS: return ir::IrType::PTR;
    // Optional/Result builtins: la variable guarda un puntero
    // (PTR) al buffer en stack alocado por Some/Ok/etc.  16 bytes
    // para Optional, 24 para Result; el lowering emite ALLOCA.
    case PrimitiveKind::OPTIONAL: return ir::IrType::PTR;
    case PrimitiveKind::RESULT: return ir::IrType::PTR;
    // string es un GcHandle opaco i64.
    case PrimitiveKind::STRING: return ir::IrType::I64;
    // tipos primitivos de coleccion: i64 handle host pointer.
    // Cero overhead vs llamar el plugin directo (sin wrapping).
    case PrimitiveKind::ARRAYLIST: return ir::IrType::I64;
    case PrimitiveKind::HASHMAP: return ir::IrType::I64;
    case PrimitiveKind::HASHSET: return ir::IrType::I64;
    case PrimitiveKind::QUEUE: return ir::IrType::I64;
    case PrimitiveKind::DEQUE: return ir::IrType::I64;
    case PrimitiveKind::TREEMAP: return ir::IrType::I64;
    case PrimitiveKind::TREESET: return ir::IrType::I64;
    case PrimitiveKind::STACK: return ir::IrType::I64;
    // Smart pointers: slot stack con host_ptr (8 bytes).
    case PrimitiveKind::UNIQUE_PTR: return ir::IrType::PTR;
    case PrimitiveKind::SHARED_PTR: return ir::IrType::PTR;
    // Borrows: host_ptr de 8 bytes (zero overhead vs T* raw).
    case PrimitiveKind::BORROW: return ir::IrType::PTR;
    case PrimitiveKind::BORROW_MUT: return ir::IrType::PTR;
    // Future<T>: handle i64.
    case PrimitiveKind::FUTURE: return ir::IrType::I64;
    // FUNCTION: par (fn_addr, env_addr); usamos PTR como aproximacion.
    case PrimitiveKind::FUNCTION: return ir::IrType::PTR;
    case PrimitiveKind::COUNT: return ir::IrType::VOID;
    }
    return ir::IrType::VOID;
}

void Lowering::compute_type_intervals() {
    type_intervals_.clear();
    const auto &layouts = tc_.class_layouts();
    if (layouts.empty()) return;
    // children[S] = clases cuya super es S (S es a su vez una clase del
    // modulo).
    std::map<std::string, std::vector<std::string>> children;
    std::vector<std::string> roots;
    for (const auto &kv : layouts) {
        const std::string &nm = kv.first;
        const std::string &sup = kv.second.super_name;
        if (!sup.empty() && layouts.count(sup))
            children[sup].push_back(nm);
        else
            roots.push_back(nm);
    }
    for (auto &kv : children)
        std::sort(kv.second.begin(), kv.second.end());
    std::sort(roots.begin(), roots.end());
    uint32_t counter = 0;
    std::unordered_set<std::string> visiting;
    std::function<uint32_t(const std::string &)> dfs =
        [&](const std::string &nm) -> uint32_t {
        const uint32_t lo = counter++;
        uint32_t hi = lo;
        if (visiting.insert(nm).second) {
            // defensa anti-ciclo
            auto it = children.find(nm);
            if (it != children.end())
                for (const std::string &ch : it->second)
                    hi = std::max(hi, dfs(ch));
        }
        type_intervals_[nm] = {lo, hi};
        return hi;
    };
    for (const std::string &r : roots)
        dfs(r);
}

size_t Lowering::optional_buf_bytes(const Type &t, size_t base) const {
    // Payload escalar: 8 bytes, como toda la vida.  Struct por valor: su
    // tamano real alineado a 8, para que quepa entero dentro del buffer.
    size_t payload = 8;
    if (t.pointee) {
        const Type &p = *t.pointee;
        if (p.kind == PrimitiveKind::STRUCT) {
            const size_t sz = size_of_type(p);
            if (sz > payload) payload = (sz + 7u) & ~static_cast<size_t>(7);
        }
    }
    return base + payload;
}

size_t Lowering::size_of_type(const Type &t) const {
    if (t.kind == PrimitiveKind::STRUCT) {
        const auto &layouts = tc_.struct_layouts();
        auto it = layouts.find(t.struct_name);
        if (it != layouts.end()) return it->second.size_bytes;
        // bug4: STRUCT puede ser un ENUM (mismo kind).  Buscar en
        // enum_layouts_ tambien.  size_bytes = 8 (tag) + 8*max_payload.
        const auto &elayouts = tc_.enum_layouts();
        auto ite = elayouts.find(t.struct_name);
        return (ite == elayouts.end()) ? 0 : ite->second.size_bytes;
    }
    if (t.kind == PrimitiveKind::PTR) return 8;
    if (t.kind == PrimitiveKind::ARRAY) {
        // T[N] ocupa N*sizeof(T) bytes; T[] (size==0) decae a puntero.
        // bug4: para T[] (dynamic) sin tamano fijo, sizeof = 8 (el
        // valor es un host_ptr al buffer).  Caller que pregunte
        // sizeof(slot) obtiene la pointer-size correcta.
        if (!t.pointee) return 0;
        if (t.array_size == 0) return 8; // host_ptr al buffer
        return static_cast<size_t>(t.array_size) * size_of_type(*t.pointee);
    }
    // CLASS: ref host_ptr al objeto GC.
    if (t.kind == PrimitiveKind::CLASS) return 8;
    // STRING: GcHandle u64.
    if (t.kind == PrimitiveKind::STRING) return 8;
    // cfn (puntero a funcion crudo, fn_is_raw): SOLO la direccion -> 8 bytes.
    // Un lambda fn(...) es un fat-pointer de 16 bytes {fn_addr, env}.
    if (t.kind == PrimitiveKind::FUNCTION && t.fn_is_raw) return 8;
    return primitive_size_bytes(t.kind);
}

bool Lowering::type_is_overlay(const Type &t) const {
    if (t.kind != PrimitiveKind::STRUCT || t.struct_name.empty()) return false;
    const auto &layouts = tc_.struct_layouts();
    auto it = layouts.find(t.struct_name);
    return it != layouts.end() && it->second.is_overlay;
}

ir::IrValueId Lowering::cast_if_needed(ir::IrValueId v, ir::IrType from,
                                       ir::IrType to, uint32_t source_line,
                                       bool is_explicit) {
    // Overload de compatibilidad: sin SourceLoc completo, el warning apunta al
    // inicio (columna 1) de la linea.  Los call sites de alto impacto pasan el
    // SourceLoc de la expresion para apuntar a su columna real.
    SourceLoc loc;
    loc.file = current_file_;
    loc.line = source_line;
    loc.column = 1;
    return cast_if_needed(v, from, to, loc, is_explicit);
}

ir::IrValueId Lowering::cast_if_needed(ir::IrValueId v, ir::IrType from,
                                       ir::IrType to, const SourceLoc &loc,
                                       bool is_explicit) {
    if (from == to || v == ir::IR_NO_VALUE) return v;
    // Un valor CONSTANTE compile-time que CABE en el tipo destino no pierde
    // informacion aunque el destino sea mas estrecho: `u8 x = 0x48` o
    // `this.mod = 3` (bitfield) o un valued-enum (`Enc.RET`, cuyo valor es una
    // constante) no deben avisar de narrowing.  Misma politica que C/C++: no
    // se avisa por `char c = 65`.  Suprime el warning tratando el cast como
    // explicito (la conversion de bits sigue emitiendose igual).
    if (!is_explicit && v < fn_->values.size() && fn_->values[v].is_const) {
        const uint64_t cv = fn_->values[v].const_val;
        bool fits = false;
        switch (to) {
        case ir::IrType::U8: fits = (cv <= 0xFFULL); break;
        case ir::IrType::U16: fits = (cv <= 0xFFFFULL); break;
        case ir::IrType::U32: fits = (cv <= 0xFFFFFFFFULL); break;
        case ir::IrType::BOOL: fits = (cv <= 1ULL); break;
        case ir::IrType::I8: {
            const int64_t s = (int64_t)cv;
            fits = (s >= -128 && s <= 255);
            break;
        }
        case ir::IrType::I16: {
            const int64_t s = (int64_t)cv;
            fits = (s >= -32768 && s <= 65535);
            break;
        }
        case ir::IrType::I32: {
            const int64_t s = (int64_t)cv;
            fits = (s >= -2147483648LL && s <= 4294967295LL);
            break;
        }
        default: break;
        }
        if (fits) is_explicit = true;
    }
    // Warning de seguridad para casts implicitos que pueden perder
    // informacion: narrowing entero, float -> int, int -> float (los
    // grandes pierden mantissa).  Solo se emite cuando el usuario NO
    // escribio el cast explicitamente: `i32 x = i64_val` avisa, pero
    // `i32 x = (i32) i64_val` no.  Misma politica que -Wconversion en
    // GCC/Clang.
    if (!is_explicit) {
        auto bytes_for = [](ir::IrType t) -> int {
            return static_cast<int>(ir::type_slot_bytes(t));
        };
        const bool from_is_float =
            (from == ir::IrType::F32 || from == ir::IrType::F64);
        const bool to_is_float =
            (to == ir::IrType::F32 || to == ir::IrType::F64);
        const bool from_is_int =
            (from == ir::IrType::I8 || from == ir::IrType::I16 ||
             from == ir::IrType::I32 || from == ir::IrType::I64 ||
             from == ir::IrType::U8 || from == ir::IrType::U16 ||
             from == ir::IrType::U32 || from == ir::IrType::U64 ||
             from == ir::IrType::BOOL);
        const bool to_is_int =
            (to == ir::IrType::I8 || to == ir::IrType::I16 ||
             to == ir::IrType::I32 || to == ir::IrType::I64 ||
             to == ir::IrType::U8 || to == ir::IrType::U16 ||
             to == ir::IrType::U32 || to == ir::IrType::U64 ||
             to == ir::IrType::BOOL);
        const int from_bytes = bytes_for(from);
        const int to_bytes = bytes_for(to);
        std::string warn_msg;
        if (from_is_float && to_is_int) {
            warn_msg = "conversion implicita float -> int trunca la parte "
                       "fraccionaria; "
                       "usa cast explicito si es intencional";
        } else if (from_is_int && to_is_float) {
            // Solo avisar para enteros grandes a f32 (perdida de mantissa).
            // i64/u64 -> f32: ~24 bits de mantissa, perdida garantizada para
            // magnitudes >2^24. i32/u32 -> f32: tambien puede perder.  i*->f64
            // es exacto hasta 2^53.
            if (to == ir::IrType::F32 && from_bytes >= 4) {
                warn_msg =
                    "conversion implicita int -> f32 puede perder precision; "
                    "usa cast explicito si es intencional";
            }
        } else if (from_is_int && to_is_int) {
            if (to_bytes < from_bytes) {
                warn_msg = "conversion implicita reduce el ancho del entero "
                           "(narrowing); usa cast explicito si es intencional";
            }
        } else if (from_is_float && to_is_float) {
            if (to == ir::IrType::F32 && from == ir::IrType::F64) {
                warn_msg = "conversion implicita f64 -> f32 reduce precision; "
                           "usa cast explicito si es intencional";
            }
        }
        if (!warn_msg.empty()) {
            diags_.warning(loc, warn_msg);
        }
    }

    // Elegir el opcode de conversion correcto segun categoria.
    ir::IrOp op = ir::IrOp::CAST;
    const bool from_float =
        (from == ir::IrType::F32 || from == ir::IrType::F64);
    const bool to_float = (to == ir::IrType::F32 || to == ir::IrType::F64);

    if (from_float && to_float) {
        op = (from == ir::IrType::F32 && to == ir::IrType::F64)
                 ? ir::IrOp::F32TOF64
                 : ir::IrOp::F64TOF32;
    } else if (from_float && !to_float) {
        // Heuristica: si el destino es signed -> FTOI; si unsigned -> FTOUI.
        const bool to_signed = (to == ir::IrType::I8 || to == ir::IrType::I16 ||
                                to == ir::IrType::I32 || to == ir::IrType::I64);
        op = to_signed ? ir::IrOp::FTOI : ir::IrOp::FTOUI;
    } else if (!from_float && to_float) {
        // Heuristica simetrica: si origen signed -> ITOF; si unsigned -> UITOF.
        const bool from_signed =
            (from == ir::IrType::I8 || from == ir::IrType::I16 ||
             from == ir::IrType::I32 || from == ir::IrType::I64);
        // Bug fix 2026-05-23: ITOF/UITOF baja a `fcvt rd_gp, f0` que
        // opera sobre el reg de 64 bits.  Si el operando es i8/i16/i32,
        // los bits altos no estan extendidos correctamente -> el float
        // resultante es incorrecto.  Para i32 -7 -> trunc deja
        // 0xFFFFFFF9 con bits altos = 0 -> ITOF lo lee como 4294967289
        // (no -7).  Fix: SEXT (signed) o ZEXT (unsigned) a i64 ANTES
        // del ITOF/UITOF.
        /* Esta tabla se escribia a mano y OMITIA F32, que caia en el default y
         * valia 8 en vez de 4.  No fallaba porque esta rama solo se recorre
         * cuando el origen NO es flotante -- estaba protegida por el contexto,
         * no por ser correcta.  El vocabulario unico ya no deja escribir eso. */
        auto bytes_of_local = [](ir::IrType t) -> int {
            return static_cast<int>(ir::type_slot_bytes(t));
        };
        if (bytes_of_local(from) < 8) {
            ir::IrValueId v_ext = fn_->new_value(ir::IrType::I64);
            ir::IrInstr ext{};
            ext.op = from_signed ? ir::IrOp::SEXT : ir::IrOp::ZEXT;
            ext.type = ir::IrType::I64;
            ext.dst = v_ext;
            ext.operands.push_back(v);
            ext.source_line = loc.line;
            emit(current_block_, std::move(ext));
            v = v_ext;
        }
        op = from_signed ? ir::IrOp::ITOF : ir::IrOp::UITOF;
    } else {
        // Entero -> entero: elegir TRUNC, ZEXT o SEXT segun el cambio
        // de ancho y la signedness de la fuente.  Sin esto, el
        // emitter recibia siempre CAST y emitia un mov plano que NO
        // truncaba ni extendia: `i32 x = i64_value` dejaba los 8
        // bytes originales en el registro (bug de truncacion).
        /* Misma historia que bytes_of_local: omitia F32 y sobrevivia porque la
         * rama excluye los flotantes.  Ahora lo contesta el vocabulario. */
        auto bytes_of = [](ir::IrType t) -> int {
            return static_cast<int>(ir::type_slot_bytes(t));
        };
        const int from_b = bytes_of(from);
        const int to_b = bytes_of(to);
        const bool from_signed =
            (from == ir::IrType::I8 || from == ir::IrType::I16 ||
             from == ir::IrType::I32 || from == ir::IrType::I64);
        if (to_b < from_b) {
            op = ir::IrOp::TRUNC;
        } else if (to_b > from_b) {
            op = from_signed ? ir::IrOp::SEXT : ir::IrOp::ZEXT;
        } else {
            // Mismo ancho: nada que extender ni truncar; un BITCAST
            // (mov plano) es lo correcto a nivel de bytecode.  Esto
            // cubre cambios de signedness sin reinterpretacion (e.g.
            // i32 -> u32) y casts entre PTR e i64.
            op = ir::IrOp::BITCAST;
        }
    }

    const ir::IrValueId dst = fn_->new_value(to);
    ir::IrInstr ins{};
    ins.op = op;
    ins.type = to;
    ins.dst = dst;
    ins.operands = {v};
    ins.source_line = loc.line;
    emit(current_block_, std::move(ins));
    return dst;
}


} // namespace vx
