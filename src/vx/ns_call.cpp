/**
 * @file ns_call.cpp
 * @brief La llamada a una funcion de un namespace importado: `ns.f(...)` y
 *        `a.b.f(...)`.
 *
 * Las dos formas eligen la funcion igual (@c select_ns_overload) y a partir de
 * ahi son la misma llamada, asi que se comprueban aqui, una vez.
 *
 * @see vx/type_checker.h
 */
#include "vx/type_checker.h"

#include <algorithm>
#include <string>

namespace vx {

Type TypeChecker::check_ns_call_(ast::CallExpr *e, ast::FieldAccessExpr *fa,
                                 uint32_t ns_idx, uint32_t picked,
                                 const std::string &shown) {
    const ImportedNamespace &ns = imported_namespaces_[ns_idx];
    fa->ns_sym = picked; // el bajado lee ESTE, no busca
    const ImportedNamespace::Sym &sym = ns.symbols[picked];
    /* La firma LOCAL cuando existe -- el modulo se fusiono y sus tipos son ya
     * los de aqui --; si no, la que viajo en el `.vxi`.  Un namespace inline
     * tiene la suya vacia hasta que se comprueba su funcion. */
    const FunctionSig *real_sig =
        sym.mangled_label.empty() ? nullptr
                                  : function_sig_by_name(sym.mangled_label);
    const FunctionSig &sig = real_sig ? *real_sig : sym.sig;

    const std::string what = shown + "." + fa->field_name;
    if (e->args.size() != sig.param_types.size()) {
        diags_.error(e->loc, "llamada a '" + what + "': se esperaban " +
                                 std::to_string(sig.param_types.size()) +
                                 " args, recibidos " +
                                 std::to_string(e->args.size()));
    }
    /* Cada argumento contra SU parametro, con la regla de una llamada local.
     * Si ninguna sobrecarga encajaba, la elegida es la primera y esto es lo que
     * lo dice, hablando de tipos -- en vez de llamarla con un valor que no es
     * el suyo --. */
    const size_t n = std::min(e->args.size(), sig.param_types.size());
    for (size_t i = 0; i < n; ++i)
        check_call_arg(e->args[i].get(), sig.param_types[i], i, what,
                       i < sig.param_dirs.size() ? sig.param_dirs[i]
                                                 : ParamDir::None,
                       i < 64 && (sig.param_by_ref_mask & (1ull << i)) != 0);
    for (size_t i = n; i < e->args.size(); ++i)
        (void)check_expr(e->args[i].get());

    // Para que el bajado la reconozca como llamada de namespace.
    fa->property_kind = 4;
    fa->ns_index = ns_idx;
    fa->result_type = Type::make_function(sig.param_types, sig.return_type);
    e->result_type = sig.return_type;
    return sig.return_type;
}

} // namespace vx
