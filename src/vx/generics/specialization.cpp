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
 * @file specialization.cpp
 * @brief Especializacion total + parcial de structs genericos (#7).
 *
 * Tras el template PRIMARIO `struct Caja<T> { ... }`, el usuario puede
 * declarar especializaciones:
 *   - TOTAL:   `struct Caja<i64> { ... }`   (tipo concreto exacto)
 *   - PARCIAL: `struct Caja<T*> { ... }`    (patron; T es un param fresco)
 * Al instanciar `Caja<X>`, se elige la especializacion MAS ESPECIFICA que
 * matchee (exacta > patron > primario), y se clona ESA definicion.  Todo
 * compile-time; cero runtime.  Modulo separado de type_checker.cpp para
 * mantenerlo manejable.
 */

#include "vx/generics/generic_infer.h"
#include "vx/type_checker.h"

#include <vector>

namespace vx {

namespace {

/* La deduccion estructural vive aparte porque no es solo de aqui: la misma
 * pregunta -- "que tipo hay que poner en esta variable para que este patron
 * encaje con este tipo" -- la hacen tambien las funciones genericas y los
 * metodos genericos al inferir sus type-args.  Escrita en cada sitio serian
 * tres criterios distintos, y el dia que divergieran la especializacion
 * elegiria una cosa y la inferencia otra para la misma firma. */
using generics::match_type_pattern;

/// Nucleo generico de seleccion: dado un decl @c DeclT (struct/clase/funcion)
/// con @c is_specialization / @c spec_pattern / @c type_params, elige la
/// especializacion mas especifica que matchee @p args.  TOTAL (sin params
/// frescos) gana a PARCIAL; entre iguales, la primera declarada.
template <class DeclT>
const DeclT *select_spec_generic(
    TypeChecker &tc, const std::vector<size_t> &candidate_indices,
    const std::vector<std::unique_ptr<ast::Node>> &decls,
    const std::vector<Type> &args, std::vector<std::string> &out_params,
    std::vector<Type> &out_args) {
    const DeclT *best = nullptr;
    int best_score = -1;
    /* Las ligaduras van INDEXADAS por la posicion del param fresco, no en una
     * tabla por nombre: son una o dos, y a esa escala hashear una cadena corta
     * cuesta mas que el acceso que ahorra.  Ademas asi salen ya en el orden en
     * que hay que devolverlas, sin recorrer nada para recolocarlas.
     *
     * Y se REUSA entre candidatas: reservar uno por cada una seria pedir
     * memoria para dos elementos tantas veces como especializaciones haya. */
    std::vector<Type> bindings;

    for (size_t idx : candidate_indices) {
        if (idx >= decls.size()) continue;
        auto *spec = static_cast<const DeclT *>(decls[idx].get());
        if (!spec || !spec->is_specialization) continue;
        if (spec->spec_pattern.size() != args.size()) continue;

        bindings.assign(spec->type_params.size(), Type{});
        uint32_t bound = 0;
        bool ok = true;
        for (size_t i = 0; i < args.size(); ++i) {
            if (!match_type_pattern(tc, spec->spec_pattern[i].get(), args[i],
                                    spec->type_params, bindings.data(),
                                    bound)) {
                ok = false;
                break;
            }
        }
        if (!ok) continue;

        const int score = spec->type_params.empty() ? 100 : 50;
        if (score > best_score) {
            best_score = score;
            best = spec;
            // Directo a la salida: la mejor hasta ahora ya esta donde va.
            out_args = bindings;
        }
    }

    if (!best) return nullptr;
    out_params = best->type_params;
    out_args.resize(out_params.size());
    return best;
}

} // namespace

const ast::StructDecl *TypeChecker::select_struct_specialization(
    const std::string &base, const std::vector<Type> &args,
    std::vector<std::string> &out_params, std::vector<Type> &out_args) {
    auto it = struct_specializations_.find(base);
    if (it == struct_specializations_.end()) return nullptr;
    return select_spec_generic<ast::StructDecl>(*this, it->second, mod_.decls,
                                                args, out_params, out_args);
}

const ast::ClassDecl *TypeChecker::select_class_specialization(
    const std::string &base, const std::vector<Type> &args,
    std::vector<std::string> &out_params, std::vector<Type> &out_args) {
    auto it = class_specializations_.find(base);
    if (it == class_specializations_.end()) return nullptr;
    return select_spec_generic<ast::ClassDecl>(*this, it->second, mod_.decls,
                                               args, out_params, out_args);
}

const ast::FunctionDecl *TypeChecker::select_function_specialization(
    const std::string &base, const std::vector<Type> &args,
    std::vector<std::string> &out_params, std::vector<Type> &out_args) {
    auto it = function_specializations_.find(base);
    if (it == function_specializations_.end()) return nullptr;
    return select_spec_generic<ast::FunctionDecl>(*this, it->second, mod_.decls,
                                                  args, out_params, out_args);
}

} // namespace vx
