/**
 * @file builtin_homonyms.cpp
 * @brief Una funcion de namespace y el builtin de su mismo nombre, en un solo
 *        conjunto de sobrecargas.
 *
 * Arriba del todo ya era asi: una funcion del usuario con el nombre de un
 * builtin se sobrecarga con el (`572_sobrecarga_builtin.vx`).  Dentro de un
 * namespace no, porque el aplanado renombra la funcion y sus llamadas antes de
 * comprobar nada -- `clamp` pasa a ser `std__math__clamp` -- y el builtin, que
 * vive con su nombre escrito, no llegaba a competir.  La funcion del namespace
 * lo OCULTABA: `clamp(x, 0.0, 1.0)` con un f64 iba a una `clamp` de enteros,
 * truncaba y daba 0 sin decir nada.
 *
 * @see vx/type_checker.h
 */
#include "vx/builtin_names.h"
#include "vx/type_checker.h"

namespace vx {

void TypeChecker::join_builtin_homonyms_(ast::FunctionDecl *fn,
                                         uint32_t sig_index) {
    // Solo las de un namespace declarado aqui: su nombre escrito es otro.
    const auto ns = declared_ns_symbols_.find(fn->name);
    if (ns == declared_ns_symbols_.end()) return;
    const std::string &written = ns->second.second;
    if (builtin_from_name(written) == Builtin::Unknown) return;

    /* Las firmas del builtin: todo su conjunto si esta sobrecargado (los
     * genericos numericos lo estan, una por tipo), o la unica que tenga. */
    OverloadSet builtin_sigs;
    const auto ov = overloads_.find(written);
    if (ov != overloads_.end()) {
        for (uint32_t idx : ov->second)
            if (function_sigs_[idx].is_builtin) builtin_sigs.push_back(idx);
    } else {
        const auto by_name = sig_by_name_.find(written);
        if (by_name != sig_by_name_.end() &&
            function_sigs_[by_name->second].is_builtin)
            builtin_sigs.push_back(by_name->second);
    }

    /* Al conjunto de la funcion, con su nombre aplanado: asi conserva su
     * simbolo y lo que exporta al `.vxi` no cambia.  Una con los MISMOS tipos
     * y ranuras que una del builtin es una redefinicion, como arriba del
     * todo. */
    for (uint32_t bidx : builtin_sigs) {
        if (!add_overload_candidate(fn->name, sig_index, bidx)) {
            report_redefinition(fn->name, fn->loc);
            return;
        }
    }
}

} // namespace vx
