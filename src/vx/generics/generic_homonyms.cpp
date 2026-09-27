/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file generic_homonyms.cpp
 * @brief Plantillas genericas HOMONIMAS: cual encaja con una llamada, y que
 *        separa el nombre de sus instancias.
 *
 * Una funcion libre y un metodo generico pueden tener homonimas con otra
 * aridad o forma (`f<T>(u64)` y `f<T>(u64, u64)`).  La funcion libre ya sabia
 * elegir y nombrar; el metodo cogia la PRIMERA y las dos instancias salian con
 * la misma etiqueta.  Lo comun a las dos vive aqui.
 */

#include "vx/type_checker.h"

#include "util/alloc/small_vector.h"
#include "vx/generics/generic_clone.h"
#include "vx/generics/generic_infer.h"
#include "vx/overload.h"

namespace vx {

namespace {

/**
 * @brief Si esta plantilla tiene TODAS las ranuras que la llamada nombra, y
 *        cada una una sola vez.
 *
 * Es la misma regla que @c overload::select aplica a las sobrecargas normales,
 * pero aqui no se puede delegar en ella: sus candidatas se comparan por TIPOS,
 * y los de una plantilla no existen hasta instanciarla.  Lo que si se puede
 * comparar antes de instanciar nada son los nombres, y eso es justo lo que
 * separa a dos plantillas que solo se distinguen por como llaman a sus
 * parametros.
 *
 * @param params Parametros de la plantilla.
 * @param names  Con que nombre se escribio cada argumento; vacio donde fue
 *               posicional.
 * @return true si todas las nombradas existen y ninguna se repite.
 */
bool template_has_named_slots(
    const std::vector<std::unique_ptr<ast::ParamDecl>> &params,
    const ParamNames &names) {
    const size_t np = params.size();
    // En pila: una firma de hasta ocho parametros no toca el monton.
    util::SmallVector<uint8_t, 8> taken;
    taken.resize(np, 0);
    for (const auto &want : names) {
        if (want.empty()) continue;
        size_t at = np;
        for (size_t j = 0; j < np; ++j)
            if (params[j] && params[j]->name == want.str()) {
                at = j;
                break;
            }
        if (at == np || taken[at] != 0) return false;
        taken[at] = 1;
    }
    return true;
}

} // namespace

TypeChecker::GenericPick
TypeChecker::pick_generic_candidate(ast::CallExpr *e,
                                    const GenericCandidate *cands, size_t n,
                                    size_t &out) {
    /* Lo que las separa es la FORMA de sus parametros o como se llaman sus
     * ranuras, y eso no se ve comparando tipos -- los suyos no existen hasta
     * instanciarlas --.  Se ve intentando DEDUCIR cada una: solo liga sus
     * variables la que de verdad encaja.
     *
     * Ni lista de las que encajan ni nada que reservar: un contador y el indice
     * de la primera bastan para las tres respuestas posibles. */
    size_t picked = 0;
    unsigned fits = 0;       // cuantas empatan en lo mas especifico
    uint32_t best = 0;       // cuanta forma pide la mejor hasta ahora
    std::vector<Type> targs; // reusado entre candidatas
    for (size_t i = 0; i < n; ++i) {
        const GenericCandidate &c = cands[i];
        // La aridad las separa sin tocar un solo argumento.
        if (c.params->size() != e->args.size()) continue;
        /* Si la llamada NOMBRA alguna ranura, una candidata que no la tenga no
         * es viable aunque sus tipos cuadraran. */
        if (!e->arg_names.empty() &&
            !template_has_named_slots(*c.params, e->arg_names))
            continue;
        if (!e->type_args.empty()) {
            // Con los type-args escritos, lo que separa es cuantos pide.
            if (c.type_params->size() != e->type_args.size()) continue;
        } else if (!deduce_call_type_args(e, c.key, *c.type_params, *c.params,
                                          targs)) {
            continue;
        }
        /* Encaja.  Entre las que encajan gana la que mas forma pide: la que
         * pide menos las habria cogido todas, asi que quedarse con ella
         * volveria inutil a la especifica. */
        uint32_t spec = 0;
        for (const auto &p : *c.params)
            if (p && p->type)
                spec +=
                    generics::shape_specificity(p->type.get(), *c.type_params);
        if (fits == 0 || spec > best) {
            best = spec;
            picked = i;
            fits = 1;
        } else if (spec == best) {
            ++fits;
        }
    }
    if (fits == 1) {
        out = picked;
        return GenericPick::Picked;
    }
    return fits == 0 ? GenericPick::NoneFits : GenericPick::Ambiguous;
}

std::string TypeChecker::homonym_discriminator(
    const std::vector<std::unique_ptr<ast::ParamDecl>> &params,
    const std::vector<std::string> &type_params,
    const std::vector<Type> &args) {
    /* Lo que las separa se pregunta donde ya estaba escrito: el mismo
     * discriminante que usan las sobrecargas, sobre los parametros YA
     * sustituidos, y con los nombres de sus ranuras, que es lo unico que
     * distingue a dos de la misma firma. */
    vxgen::GenSubst g{&type_params, &args};
    std::vector<Type> ps;
    ParamNames pn;
    ps.reserve(params.size());
    for (const auto &p : params) {
        auto ct = vxgen::clone_type_with_subst(p->type.get(), g);
        ps.push_back(type_from_node(ct.get()));
        pn.push_back(p->name);
    }
    return overload::discriminator(ps, &pn);
}

} // namespace vx
