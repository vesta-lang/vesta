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
 * `u128 m = u128(0xFFFF...)` no deja ninguna llamada en runtime, solo los campos
 * ya escritos.  Es la base de los literales de tipo usuario (modelo Swift
 * @c ExpressibleByIntegerLiteral): un tipo de libreria puede construirse desde
 * un literal sin que el compilador conozca su representacion interna.
 *
 * El ctor comptime reutiliza integramente la maquinaria ya existente:
 *   - Se baja a `__macro_<T>__ctor_<aridad>` (en @c lower_struct_methods), igual
 *     que una comptime fn-VM, y se registra en el @c ComptimeRuntime.
 *   - En el call site `T(args)` se invoca con @c invoke_struct_macro, cuya
 *     convencion SRET encaja de forma natural: el buffer de retorno que el
 *     harness antepone ES el @c this que el cuerpo del ctor inicializa.
 *   - El struct resultante se reconstruye con @c fill_struct_fields_from_bytes y
 *     se emite como datos con @c materialize_comptime_struct -- identico en los
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

ir::IrValueId
Lowering::try_lower_comptime_ctor_call(ast::CallExpr *e,
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

    // A partir de aqui el ctor comptime SIEMPRE materializa el struct (con su
    // valor real o con un placeholder): un ctor comptime no tiene version
    // runtime, asi que caer al ctor `<T>__ctor_N` dejaria un simbolo sin
    // resolver en el linker.
    //
    // Marshalar los argumentos a la ComptimeVM.  Un argumento `expr` fue
    // capturado por el parser como @c StringLitExpr (el texto crudo del literal)
    // y se pasa como GcHandle a un StringObject; el resto se evalua en
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
        if (cr.invoke_struct_macro(comptime_ctor_ir_name(slay.name, arity),
                                   vm_args, slay.size_bytes, bytes))
            fill_struct_fields_from_bytes(tc_, slay, bytes, 0, res);
    }

    // Materializar el struct como datos constantes; en el binario aparece el
    // struct ya construido, sin llamada en tiempo de ejecucion.
    return materialize_comptime_struct(res, slay, e->loc.line);
}

} // namespace vx
