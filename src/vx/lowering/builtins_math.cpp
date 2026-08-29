/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vx/lowering/builtins_math.cpp
 * @brief Bajada de las operaciones matematicas.
 *
 * Veintiocho, y no son una sola cosa aunque lo parezcan: unas trabajan con
 * numeros reales -- raiz, potencia, seno --, otras con enteros -- el mayor de
 * dos, acotar entre dos limites -- y otras con los BITS del numero -- contar
 * los que estan a uno, cuantos ceros hay delante, dar la vuelta al orden de
 * los bytes --.  Las tres se escriben igual en Vesta y de ahi que se atiendan
 * juntas, pero bajan distinto y por eso el bloque distingue.
 *
 * Lo que las une de verdad es como viajan los numeros con decimales.  La
 * maquina virtual pasa los argumentos en registros de proposito general, asi
 * que un `f64` no viaja como numero sino como sus BITS metidos en un entero de
 * sesenta y cuatro; la funcion nativa los recibe asi y devuelve otros bits que
 * hay que volver a leer como numero.  Ese ida y vuelta es invisible en el
 * fuente -- `sqrt(2.0)` no lo menciona -- y es la razon de que estas no se
 * puedan tratar como una llamada normal.
 *
 * Las que devuelven un entero (el mayor de dos, acotar, contar bits) NO hacen
 * esa conversion: su resultado es un numero, no unos bits que representen uno.
 * Confundir los dos casos da valores absurdos que parecen aleatorios, y es el
 * error clasico de esta zona.
 *
 * Se separo de la funcion que despacha todos los builtins.  Entra por su
 * propio punto: si el nombre no es de esta familia, contesta que no y quien
 * pregunta sigue con las demas.
 */
#include "vx/lowering.h"
#include "ir/ir_type_info.h" // vocabulario UNICO de anchura/clase de un IrType
#include "lowering_internal.h" // la cocina compartida (no es la interfaz)
#include <string>
#include <utility>

namespace vx {

/**
 * @brief Intenta bajar @p e como una de las operaciones matematicas.
 *
 * @param e         La llamada.
 * @param b         Que builtin es, ya resuelto por quien despacha.
 * @param out_value Donde dejar el resultado.
 * @return @c true si @p b era de esta familia y quedo bajado.
 */
bool Lowering::try_lower_math_builtins(ast::CallExpr *e, Builtin b,
                                       ir::IrValueId &out_value) {
    const bool is_math_sqrt = (b == Builtin::Sqrt);
    const bool is_math_pow = (b == Builtin::Pow);
    const bool is_math_fabs = (b == Builtin::Fabs);
    const bool is_math_floor = (b == Builtin::Floor);
    const bool is_math_ceil = (b == Builtin::Ceil);
    const bool is_math_round = (b == Builtin::Round);
    const bool is_math_fmin = (b == Builtin::Fmin);
    const bool is_math_fmax = (b == Builtin::Fmax);
    const bool is_math_log = (b == Builtin::Log);
    const bool is_math_log2 = (b == Builtin::Log2);
    const bool is_math_log10 = (b == Builtin::Log10);
    const bool is_math_sin = (b == Builtin::Sin);
    const bool is_math_cos = (b == Builtin::Cos);
    const bool is_math_tan = (b == Builtin::Tan);
    const bool is_math_abs = (b == Builtin::Abs);
    const bool is_math_imin = (b == Builtin::Imin);
    const bool is_math_imax = (b == Builtin::Imax);
    const bool is_math_clamp = (b == Builtin::Clamp);
    const bool is_math_trunc = (b == Builtin::Trunc);
    const bool is_math_iminu = (b == Builtin::Iminu);
    const bool is_math_imaxu = (b == Builtin::Imaxu);
    const bool is_math_ilog2 = (b == Builtin::Ilog2);
    const bool is_math_popcount = (b == Builtin::Popcount);
    const bool is_math_clz = (b == Builtin::Clz);
    const bool is_math_ctz = (b == Builtin::Ctz);
    const bool is_math_bswap = (b == Builtin::Bswap);
    const bool is_math_rotl = (b == Builtin::Rotl);
    const bool is_math_rotr = (b == Builtin::Rotr);
    const bool is_any_math =
        is_math_sqrt || is_math_pow || is_math_fabs || is_math_floor ||
        is_math_ceil || is_math_round || is_math_fmin || is_math_fmax ||
        is_math_log || is_math_log2 || is_math_log10 || is_math_sin ||
        is_math_cos || is_math_tan || is_math_abs || is_math_imin ||
        is_math_imax || is_math_clamp || is_math_trunc || is_math_iminu ||
        is_math_imaxu || is_math_ilog2 || is_math_popcount || is_math_clz ||
        is_math_ctz || is_math_bswap || is_math_rotl || is_math_rotr;
    if (!is_any_math) return false;

    // directo) o expresion string-typed (no soportado todavia).
    // Math builtins -> CALLN a vesta_math.dll.  ABI: bits IEEE 754
    // como uint64_t en r1..rN, retorno (bits) en r0.  Para funciones
    // con tipo de retorno float (sqrt, pow, sin, ...), el callee
    // devuelve los bits f64.  Para funciones que devuelven int (abs,
    // imin, imax, clamp), el valor se devuelve como i64 directo.
    if (is_any_math) {
        const std::string lib_math = "stdlib/native/math/vesta_math";

        // Math-IR-promote (raw_asm-elim wave 4): para builtins con IR
        // op nativa (FSQRT/FABS/FMIN/FMAX/FFLOOR/FCEIL/FROUND/FTRUNC),
        // emitir el IR op directamente.  Beneficios:
        //   (a) Constant folding: sqrt(2.0) -> literal compile-time.
        //   (b) Selector JIT puede emitir sqrtsd/andpd/roundsd nativos
        //       (~4 ciclos) en lugar de CALLN (~50ns).
        //   (c) Cross-target: cuando llegue ARM Selector, emitira fsqrt.d
        //       sin tocar el IR.
        // Fallback CALLN sigue activo para transcendentales (log/sin/cos/
        // tan/pow/exp): libm los implementa mejor que cualquier inline.
        auto emit_float_irop = [&](ir::IrOp op, size_t nargs) -> bool {
            if (e->args.size() != nargs) {
                return builtin_error(e->loc, std::string("'") + std::string(builtin_name(b)) +
                                                 "': " + std::to_string(nargs) + " arg(s)", out_value);
            }
            std::vector<ir::IrValueId> ops;
            ops.reserve(nargs);
            for (auto &a : e->args) {
                ir::IrValueId v = lower_expr(a.get());
                if (v == ir::IR_NO_VALUE) {
                    out_value = ir::IR_NO_VALUE;
                    return true;
                }
                // Promover f32 a f64 si hace falta (IR ops trabajan en f64).
                const ir::IrType vt = fn_->values[v].type;
                if (vt == ir::IrType::F32) {
                    ir::IrValueId f64v =
                        emit_ir_unop(ir::IrOp::F32TOF64, v,
                                     ir::IrType::F64, e->loc.line);
                    v = f64v;
                } else if (vt != ir::IrType::F64) {
                    // Si el arg no es float, lo dejamos como esta (el
                    // emitter trata bits como i64 o el caller hizo cast).
                }
                ops.push_back(v);
            }
            const ir::IrValueId v_dst = fn_->new_value(ir::IrType::F64);
            ir::IrInstr in{};
            in.op = op;
            in.type = ir::IrType::F64;
            in.dst = v_dst;
            in.operands = std::move(ops);
            in.source_line = e->loc.line;
            emit(current_block_, std::move(in));
            out_value = v_dst;
            return true;
        };
        // Math-IR-promote: despachar a IR op directamente para los que
        // tienen instr hardware nativa (target-agnostico).  Beneficios:
        //   (a) FSQRT/FABS/FNEG bajan a bytecode VM nativo (fsqrt/fabs/
        //       fneg, ~5ns) en lugar de CALLN (~50ns).
        //   (b) Para FMIN/FMAX/FFLOOR/FCEIL/FROUND/FTRUNC el bytecode
        //       todavia no tiene opcodes; el IR emitter (ir_emitter.cpp)
        //       tiene un pre-pase que los convierte a CALLN equivalente.
        //   (c) El Selector JIT (futuro) emite sqrtsd/andpd/minsd/roundsd
        //       nativos sin tocar el frontend.
        //   (d) Constant folding (cuando se añada) funciona uniforme.
        if (is_math_sqrt) return emit_float_irop(ir::IrOp::FSQRT, 1);
        if (is_math_fabs) return emit_float_irop(ir::IrOp::FABS, 1);
        if (is_math_fmin) return emit_float_irop(ir::IrOp::FMIN, 2);
        if (is_math_fmax) return emit_float_irop(ir::IrOp::FMAX, 2);
        if (is_math_floor) return emit_float_irop(ir::IrOp::FFLOOR, 1);
        if (is_math_ceil) return emit_float_irop(ir::IrOp::FCEIL, 1);
        if (is_math_round) return emit_float_irop(ir::IrOp::FROUND, 1);
        if (is_math_trunc) return emit_float_irop(ir::IrOp::FTRUNC, 1);

        // Math-IR-promote v2.2a: bit ops + int ops adicionales.
        // Producen i64 (no float).  Lambda paralela a emit_float_irop.
        auto emit_int_irop = [&](ir::IrOp op, size_t nargs) -> bool {
            if (e->args.size() != nargs) {
                return builtin_error(e->loc, std::string("'") + std::string(builtin_name(b)) +
                                                 "': " + std::to_string(nargs) + " arg(s)", out_value);
            }
            std::vector<ir::IrValueId> ops;
            ops.reserve(nargs);
            for (auto &a : e->args) {
                ir::IrValueId v = lower_expr(a.get());
                if (v == ir::IR_NO_VALUE) {
                    out_value = ir::IR_NO_VALUE;
                    return true;
                }
                ops.push_back(cast_if_needed(v, fn_->values[v].type,
                                             ir::IrType::I64, e->loc.line));
            }
            const ir::IrValueId v_dst = fn_->new_value(ir::IrType::I64);
            ir::IrInstr in{};
            in.op = op;
            in.type = ir::IrType::I64;
            in.dst = v_dst;
            in.operands = std::move(ops);
            in.source_line = e->loc.line;
            emit(current_block_, std::move(in));
            out_value = v_dst;
            return true;
        };
        if (is_math_iminu) return emit_int_irop(ir::IrOp::IMINU, 2);
        if (is_math_imaxu) return emit_int_irop(ir::IrOp::IMAXU, 2);
        if (is_math_ilog2) return emit_int_irop(ir::IrOp::ILOG2, 1);
        if (is_math_popcount) return emit_int_irop(ir::IrOp::POPCNT, 1);
        if (is_math_clz) return emit_int_irop(ir::IrOp::CLZ, 1);
        if (is_math_ctz) return emit_int_irop(ir::IrOp::CTZ, 1);
        if (is_math_bswap) return emit_int_irop(ir::IrOp::BYTESWAP, 1);
        if (is_math_rotl) return emit_int_irop(ir::IrOp::ROTL, 2);
        if (is_math_rotr) return emit_int_irop(ir::IrOp::ROTR, 2);
        // Promocion IMIN/IMAX/IABS a IR op tambien (los wires antiguos
        // CALLN siguen activos abajo pero el pre-pase los re-wirea).
        if (is_math_abs) return emit_int_irop(ir::IrOp::IABS, 1);
        if (is_math_imin) return emit_int_irop(ir::IrOp::IMIN, 2);
        if (is_math_imax) return emit_int_irop(ir::IrOp::IMAX, 2);

        // Camino CALLN tradicional para transcendentales
        // (log/exp/sin/cos/tan/pow) e ints (abs/imin/imax/clamp).  libm los
        // implementa mejor que cualquier inline que podamos emitir.
        std::string func_name;
        size_t expected_args = 1;
        ir::IrType ret_ir = ir::IrType::F64;
        ir::IrType arg_ir =
            ir::IrType::I64; // por defecto pasa bits f64 como i64
        bool dst_is_float = true;
        if (is_math_pow) {
            func_name = "vmath_pow";
            expected_args = 2;
        } else if (is_math_log) {
            func_name = "vmath_log";
        } else if (is_math_log2) {
            func_name = "vmath_log2";
        } else if (is_math_log10) {
            func_name = "vmath_log10";
        } else if (is_math_sin) {
            func_name = "vmath_sin";
        } else if (is_math_cos) {
            func_name = "vmath_cos";
        } else if (is_math_tan) {
            func_name = "vmath_tan";
        } else if (is_math_abs) {
            func_name = "vmath_abs";
            ret_ir = ir::IrType::I64;
            dst_is_float = false;
        } else if (is_math_imin) {
            func_name = "vmath_min";
            expected_args = 2;
            ret_ir = ir::IrType::I64;
            dst_is_float = false;
        } else if (is_math_imax) {
            func_name = "vmath_max";
            expected_args = 2;
            ret_ir = ir::IrType::I64;
            dst_is_float = false;
        } else if (is_math_clamp) {
            func_name = "vmath_clamp";
            expected_args = 3;
            ret_ir = ir::IrType::I64;
            dst_is_float = false;
        }
        if (e->args.size() != expected_args) {
            return builtin_error(e->loc, std::string("'") + std::string(builtin_name(b)) + "': " +
                                             std::to_string(expected_args) + " arg(s)", out_value);
        }
        std::vector<ir::IrValueId> ops;
        ops.reserve(expected_args);
        for (auto &a : e->args) {
            ir::IrValueId v = lower_expr(a.get());
            if (v == ir::IR_NO_VALUE) {
                out_value = ir::IR_NO_VALUE;
                return true;
            }
            // ABI nativo: vmath_* recibe bits IEEE 754 como uint64_t.
            // Para floats el value YA esta en GP como bits (lower_expr
            // de un f64 produce un i64 en GP); pasamos tal cual via
            // BITCAST (NO cast_if_needed/FTOI, que convertiria VALOR).
            // Para int builtins (abs/imin/imax/clamp) un cast normal
            // i32->i64 es lo correcto.
            const ir::IrType vt = fn_->values[v].type;
            if ((vt == ir::IrType::F64 || vt == ir::IrType::F32) &&
                arg_ir == ir::IrType::I64) {
                if (vt == ir::IrType::F32) {
                    ir::IrValueId f64v =
                        emit_ir_unop(ir::IrOp::F32TOF64, v,
                                     ir::IrType::F64, e->loc.line);
                    v = f64v;
                }
                ir::IrValueId bits =
                    emit_ir_unop(ir::IrOp::BITCAST, v, ir::IrType::I64, e->loc.line);
                v = bits;
            } else {
                v = cast_if_needed(v, vt, arg_ir, e->loc.line);
            }
            ops.push_back(v);
        }
        const ir::IrValueId dst = emit_native_call(
            lib_math, func_name, std::move(ops), ret_ir, e->loc.line);
        (void)dst_is_float;
        out_value = dst;
        return true;
    }

    return false;
}

} // namespace vx
