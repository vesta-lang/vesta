/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file literal_ctor.cpp
 * @brief Constructor `comptime T(expr)` de structs value-type (F1b).
 *
 * Un struct puede declarar un constructor @c comptime:
 *
 *     struct u128 {
 *         u64 hi;
 *         u64 lo;
 *         comptime u128(expr texto) { ... }
 *     }
 *
 * que se ejecuta EN TIEMPO DE COMPILACION (en la ComptimeVM, por interprete o
 * JIT) y cuyo resultado se MATERIALIZA como un struct constante en el binario:
 * `u128 m = u128(0xFFFF...)` no deja ninguna llamada en runtime, solo los
 * campos ya escritos.  Es la base de los literales de tipo usuario (modelo
 * Swift
 * @c ExpressibleByIntegerLiteral): un tipo de libreria puede construirse desde
 * un literal sin que el compilador conozca su representacion interna.
 *
 * El ctor comptime reutiliza integramente la maquinaria ya existente:
 *   - Se baja a `__macro_<T>__ctor_<aridad>` (en @c lower_struct_methods),
 * igual que una comptime fn-VM, y se registra en el @c ComptimeRuntime.
 *   - En el call site `T(args)` se invoca con @c invoke_struct_macro, cuya
 *     convencion SRET encaja de forma natural: el buffer de retorno que el
 *     harness antepone ES el @c this que el cuerpo del ctor inicializa.
 *   - El struct resultante se reconstruye con @c fill_struct_fields_from_bytes
 * y se emite como datos con @c materialize_comptime_struct -- identico en los
 *     tres modos (interp/JIT/AOT).
 *
 * Estos son metodos de @c vx::Lowering (declarados en @c vx/lowering.h) pero se
 * definen en este TU separado para mantener la funcionalidad de literales
 * agrupada y no inflar @c lowering.cpp (mismo criterio que @c vectorize.cpp).
 */

#include "vx/lowering.h"

#include "vx/ast.h"
#include "vx/comptime/comptime_introspect.h"
#include "vx/comptime/comptime_vm.h"
#include "vx/type_checker.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vx {

std::string Lowering::comptime_ctor_ir_name(const std::string &struct_name,
                                            size_t arity) const {
    // Un ctor comptime baja como codigo comptime: prefijo `__macro_` sobre el
    // mismo esquema de aridad que el ctor runtime (`<T>__ctor_<aridad>`), que
    // discrimina los overloads.
    return "__macro_" + struct_name + "__ctor_" + std::to_string(arity);
}

ir::IrValueId Lowering::try_lower_comptime_ctor_call(ast::CallExpr *e,
                                                     const StructLayout &slay) {
    // Localizar un ctor `comptime` cuya aridad case con la llamada.  Si no hay,
    // no aplica: el caller sigue con el ctor runtime.
    const size_t arity = e->args.size();
    bool has_comptime_ctor = false;
    for (const auto &m : slay.methods) {
        if (m.is_constructor && m.is_comptime &&
            m.param_types.size() == arity) {
            has_comptime_ctor = true;
            break;
        }
    }
    if (!has_comptime_ctor) return ir::IR_NO_VALUE;

    /* Dentro de codigo comptime la llamada NO se materializa: se LLAMA.
     *
     * Materializar es lo correcto en un sitio de ejecucion -- el struct se
     * calcula al compilar y en el binario solo quedan sus campos ya escritos --
     * pero aqui el sitio es el cuerpo de OTRA funcion comptime, que todavia se
     * esta bajando.  La maquina de compilacion no puede ejecutar un ctor cuyo
     * bytecode aun se esta emitiendo, asi que se caia al relleno de mas abajo:
     * un bufer SIN INICIALIZAR del que luego se leen los campos.  Lo que salia
     * no era cero -- era lo que hubiera en el monton, o sea una direccion
     * cualquiera leida como si fuera el struct -- y salia sin decir nada.
     *
     * En el artefacto comptime el ctor SI existe, con su nombre `__macro_`, y
     * ahi cualquier funcion puede llamar a cualquier otra.  Asi que se emite la
     * llamada igual que la de un constructor normal -- bufer del tamano del
     * struct como `this`, y los argumentos detras -- y la ejecuta la maquina
     * cuando le toque, con el resto del cuerpo. */
    if (fn_ != nullptr && ir::es_cuerpo_comptime(fn_->name)) {
        const uint64_t buf_bytes =
            (static_cast<uint64_t>(slay.size_bytes) + 7ULL) & ~7ULL;
        const ir::IrValueId v_buf = fn_->new_value(ir::IrType::PTR);
        ir::IrInstr al{};
        al.op = ir::IrOp::ALLOCA;
        al.type = ir::IrType::I8;
        al.imm = buf_bytes;
        al.dst = v_buf;
        al.host_alloca = true;
        al.source_line = e->loc.line;
        emit(current_block_, std::move(al));
        fn_->values[v_buf].is_host_ptr = true;

        std::vector<ir::IrValueId> operands;
        operands.reserve(e->args.size() + 1);
        operands.push_back(v_buf); // `this`: el bufer que el ctor inicializa
        for (auto &a : e->args) {
            const ir::IrValueId av = lower_expr(a.get());
            if (av == ir::IR_NO_VALUE) return ir::IR_NO_VALUE;
            operands.push_back(av);
        }
        ir::IrInstr ins{};
        ins.op = ir::IrOp::CALL;
        ins.type = ir::IrType::VOID;
        ins.dst = ir::IR_NO_VALUE;
        ins.func_name = comptime_ctor_ir_name(slay.name, arity);
        ins.operands = std::move(operands);
        ins.source_line = e->loc.line;
        emit(current_block_, std::move(ins));
        return v_buf;
    }

    // A partir de aqui el ctor comptime SIEMPRE materializa el struct (con su
    // valor real o con un placeholder): un ctor comptime no tiene version
    // runtime, asi que caer al ctor `<T>__ctor_N` dejaria un simbolo sin
    // resolver en el linker.
    //
    // Marshalar los argumentos a la ComptimeVM.  Un argumento `expr` fue
    // capturado por el parser como @c StringLitExpr (el texto crudo del
    // literal) y se pasa como GcHandle a un StringObject; el resto se evalua en
    // compile-time a su valor entero.  En el pass 1 del two-phase la VM aun no
    // esta lista (el marshalling/evaluacion puede diferir): en ese caso se cae
    // al placeholder de abajo.
    ComptimeRuntime &cr = const_cast<TypeChecker &>(tc_).comptime_runtime();
    std::vector<uint64_t> vm_args;
    vm_args.reserve(arity);
    bool args_ok = true;
    for (const auto &a : e->args) {
        if (a && a->kind == ast::NodeKind::StringLitExpr) {
            auto *sl = static_cast<ast::StringLitExpr *>(a.get());
            uint64_t handle = 0;
            if (!cr.marshal_string(sl->value, handle)) {
                args_ok = false;
                break;
            }
            vm_args.push_back(handle);
        } else {
            const ComptimeEvalResult ev = comptime_eval_expr(tc_, a.get());
            if (!ev.ok || ev.deferred) {
                args_ok = false;
                break;
            }
            vm_args.push_back(static_cast<uint64_t>(ev.value));
        }
    }

    // Ejecutar el ctor en la ComptimeVM.  @c invoke_struct_macro antepone el
    // buffer de retorno (SRET) como primer argumento -- que en un ctor ES el
    // `this` que el cuerpo inicializa -- ejecuta el bytecode y devuelve los
    // bytes del struct construido.  Si los args no estan listos o el bytecode
    // aun no esta cargado (pass 1), @c res queda como PLACEHOLDER (buffer del
    // tamano del struct sin inicializar): el .velb del pass 1 solo sirve para
    // cargar los macros, y el pass 2 recompila con el valor real.
    ComptimeEvalResult res;
    res.is_struct = true;
    if (args_ok) {
        std::vector<uint8_t> bytes;
        // El buffer de eval incluye la cola de campos `comptime` (si los hay):
        // el cuerpo del ctor puede escribir/leer `this.<campo_comptime>` en su
        // slot temporal.  fill_struct_fields_from_bytes solo lee los campos
        // runtime (primeros @c size_bytes); la cola comptime se descarta.
        const uint32_t buf_sz = slay.comptime_size_bytes > slay.size_bytes
                                    ? slay.comptime_size_bytes
                                    : slay.size_bytes;
        // Sembrar el buffer con los defaults de los campos ANTES de correr el
        // ctor: la semantica es "defaults primero, cuerpo del ctor encima".
        // Sin esto, un campo que el ctor no toca salia a cero en vez de con su
        // valor declarado (`struct S { u64 n = 3; comptime S(...) {} }` daba
        // n=0).  Solo se siembran los defaults ESCALARES comptime-evaluables;
        // los que no lo son (una referencia a funcion, un string) quedan a cero
        // aqui y los emite el lowering en runtime.
        bytes.assign(buf_sz, 0);
        for (const auto &fi : slay.fields) {
            if (!fi.default_init) continue;
            if (fi.bit_width != 0) continue; // bit field: fuera de alcance
            if (fi.size == 0 || fi.size > 8) continue;
            if (static_cast<size_t>(fi.offset) + fi.size > bytes.size())
                continue;
            const ComptimeEvalResult dv =
                comptime_eval_expr(tc_, fi.default_init);
            if (!dv.ok || dv.deferred || dv.is_str || dv.is_array ||
                dv.is_struct || dv.is_type)
                continue;
            const uint64_t raw = static_cast<uint64_t>(dv.value);
            for (uint32_t b = 0; b < fi.size; ++b)
                bytes[fi.offset + b] =
                    static_cast<uint8_t>((raw >> (8 * b)) & 0xFF);
        }
        if (cr.invoke_struct_macro(comptime_ctor_ir_name(slay.name, arity),
                                   vm_args, buf_sz, bytes))
            fill_struct_fields_from_bytes(tc_, slay, bytes, 0, res);
    }

    // Materializar el struct como datos constantes; en el binario aparece el
    // struct ya construido, sin llamada en tiempo de ejecucion.
    return materialize_comptime_struct(res, slay, e->loc.line);
}

} // namespace vx
