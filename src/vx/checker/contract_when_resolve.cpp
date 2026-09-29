/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file contract_when_resolve.cpp
 * @brief Resolver los `@complexity` y contratos de huella con `when:` de una
 *        funcion o un metodo: los de una INSTANCIA con sus parametros de tipo
 *        ligados, y los de lo que no es generico solo contra el objetivo.
 *
 * El parser deja esas declaraciones sin resolver a proposito: hay que verlas
 * juntas para aplicar la prioridad por especificidad (`cwhen::resolve`), y un
 * atomo que habla de T solo tiene respuesta con T concreto.  Funcion libre y
 * metodo tienen los mismos campos de contrato: una sola resolucion para los
 * dos.
 */

#include "vx/type_checker.h"

#include "vx/contract_when.h"
#include "vx/generics/generic_clone.h" // GenSubst
#include "vx/parser.h"                 // target_expr_matches

#include <cstdint>
#include <cstdlib>
#include <string>

namespace vx {

namespace {

/**
 * @brief Da valor a un atomo de un `when:` de contrato con los parametros de
 *        tipo ligados.
 *
 * Los de target los resuelve el evaluador de @Target -- el mismo que usa la
 * anotacion @Target, asi que no hay dos gramaticas ni dos verdades.  Los que
 * hablan del parametro de tipo se responden aqui, que es donde T es concreto:
 * es lo que permite declarar que `atomic<i64>::fetch_add` es un `lock xadd`
 * (O(1)) y `atomic<f64>::fetch_add` un bucle CAS (O(n)), del mismo fuente.
 *
 * @param at El atomo.
 * @param g  La sustitucion (vacia fuera de una instancia).
 * @param ok Salida: falso si el atomo no tiene respuesta.
 * @return Su valor.
 */
bool eval_when_atom(const std::string &at, const vxgen::GenSubst &g,
                    bool &ok) {
    if (cwhen::atom_kind(at) == cwhen::AtomKind::TARGET)
        return target_expr_matches(at);

    // `pred<PARAM>()` [OP N].  La forma ya la valido `atom_kind`.
    const size_t lt = at.find('<');
    const size_t gt = at.find('>', lt == std::string::npos ? 0 : lt);
    if (lt == std::string::npos || gt == std::string::npos || gt < lt) {
        ok = false;
        return false;
    }
    const std::string pred = at.substr(0, lt);
    const std::string param = at.substr(lt + 1, gt - lt - 1);

    const Type *concreto = nullptr;
    if (g.params && g.args) {
        for (size_t i = 0; i < g.params->size() && i < g.args->size(); ++i) {
            if ((*g.params)[i] == param) {
                concreto = &(*g.args)[i];
                break;
            }
        }
    }
    if (!concreto) {
        ok = false; // el `when:` nombra un param que este tipo no tiene
        return false;
    }
    const PrimitiveKind k = concreto->kind;
    const bool es_float = (k == PrimitiveKind::F32 || k == PrimitiveKind::F64);
    const bool es_int = (k == PrimitiveKind::I8 || k == PrimitiveKind::I16 ||
                         k == PrimitiveKind::I32 || k == PrimitiveKind::I64 ||
                         k == PrimitiveKind::U8 || k == PrimitiveKind::U16 ||
                         k == PrimitiveKind::U32 || k == PrimitiveKind::U64);

    if (pred == "type.is_float") return es_float;
    if (pred == "type.is_integer") return es_int;
    if (pred == "type.is_pointer") return k == PrimitiveKind::PTR;
    if (pred == "type.is_signed")
        return (k == PrimitiveKind::I8 || k == PrimitiveKind::I16 ||
                k == PrimitiveKind::I32 || k == PrimitiveKind::I64);
    if (pred == "type.size") {
        size_t bytes = 0;
        switch (k) {
        case PrimitiveKind::I8:
        case PrimitiveKind::U8:
        case PrimitiveKind::BOOL: bytes = 1; break;
        case PrimitiveKind::I16:
        case PrimitiveKind::U16: bytes = 2; break;
        case PrimitiveKind::I32:
        case PrimitiveKind::U32:
        case PrimitiveKind::F32:
        case PrimitiveKind::CHAR: bytes = 4; break;
        case PrimitiveKind::I64:
        case PrimitiveKind::U64:
        case PrimitiveKind::F64:
        case PrimitiveKind::PTR: bytes = 8; break;
        default: ok = false; return false;
        }
        const size_t par = at.find(')', gt);
        if (par == std::string::npos) {
            ok = false;
            return false;
        }
        std::string resto = at.substr(par + 1);
        size_t a = resto.find_first_not_of(" \t");
        if (a == std::string::npos) {
            ok = false;
            return false;
        }
        resto = resto.substr(a);
        std::string op;
        while (!resto.empty() && (resto[0] == '=' || resto[0] == '!' ||
                                  resto[0] == '<' || resto[0] == '>')) {
            op.push_back(resto[0]);
            resto.erase(resto.begin());
        }
        a = resto.find_first_not_of(" \t");
        if (op.empty() || a == std::string::npos) {
            ok = false;
            return false;
        }
        /* Ancho FIJO, no `long`: en Windows mide 32 bits, asi que un tamano por
         * encima de 2 GB se recortaba SIN DECIRLO y el contrato se comprobaba
         * contra otro numero.  Mismo fallo que tenia el umbral del JIT: ver la
         * nota en `src/jit/auto_jit.cpp`. */
        const int64_t n =
            static_cast<int64_t>(std::strtoll(resto.c_str() + a, nullptr, 10));
        const int64_t b = static_cast<int64_t>(bytes);
        if (op == "==" || op == "=") return b == n;
        if (op == "!=") return b != n;
        if (op == "<") return b < n;
        if (op == "<=") return b <= n;
        if (op == ">") return b > n;
        if (op == ">=") return b >= n;
        ok = false;
        return false;
    }
    ok = false;
    return false;
}

/**
 * @struct SubstAtomEval
 * @brief Evalua un atomo de un `when:` con los parametros de tipo de una
 *        instancia ya ligados.
 */
struct SubstAtomEval {
    const vxgen::GenSubst &g; ///< la sustitucion de la instancia

    /**
     * @brief Evalua el atomo.
     * @param at El atomo.
     * @param ok Salida: si tuvo respuesta.
     * @return Su valor.
     */
    bool operator()(const std::string &at, bool &ok) const {
        return eval_when_atom(at, g, ok);
    }
};

/**
 * @struct TargetOnlyAtomEval
 * @brief Evalua un atomo de un `when:` de algo que NO es generico: solo los
 *        del objetivo tienen respuesta; uno que habla de un parametro de tipo
 *        no tiene a que referirse, y se dice.
 */
struct TargetOnlyAtomEval {
    /**
     * @brief Evalua el atomo.
     * @param at El atomo.
     * @param ok Salida: si tuvo respuesta.
     * @return Su valor.
     */
    bool operator()(const std::string &at, bool &ok) const {
        if (cwhen::atom_kind(at) == cwhen::AtomKind::TIPO) {
            ok = false;
            return false;
        }
        static const vxgen::GenSubst kNoTypes{};
        return eval_when_atom(at, kNoTypes, ok);
    }
};

/**
 * @struct ErrorAt
 * @brief Da un error de un `when:` en un sitio fijo.
 */
struct ErrorAt {
    Diagnostics &diags;   ///< donde se apunta
    const SourceLoc &loc; ///< el sitio

    /**
     * @brief Apunta el error.
     * @param msg El mensaje.
     */
    void operator()(const std::string &msg) const { diags.error(loc, msg); }
};

/**
 * @brief Vuelca en @p d el `@complexity` que casa de entre los pendientes de
 *        @p from, y vacia los de @p d.
 * @tparam Decl `ast::FunctionDecl` o `ast::ClassMethodDecl`.
 * @param d    Donde se escribe (puede ser @p from).
 * @param from De donde salen los pendientes.
 * @param ev   Evaluador de atomos.
 * @param err  Donde van los errores.
 */
template <class Decl>
void resolve_complexity_with(Decl &d, const Decl &from,
                             const cwhen::AtomEval &ev,
                             const cwhen::ErrFn &err) {
    if (from.complexity_pending.empty()) {
        d.complexity_pending.clear();
        return;
    }
    cwhen::Resolved r;
    cwhen::resolve(from.complexity_pending, ev, err, r);
    d.complexity_expr = std::move(r.expr);
    d.complexity_vars = std::move(r.vars);
    d.complexity_partial_pre = std::move(r.partial_pre);
    d.complexity_partial_post = std::move(r.partial_post);
    d.complexity_total_pre = std::move(r.total_pre);
    d.complexity_total_post = std::move(r.total_post);
    d.complexity_pending.clear();
}

/**
 * @brief Lo mismo para los contratos de HUELLA.  El punto de partida son los
 *        campos directos de @p d (los declarados SIN `when:`); un `when:` que
 *        casa gana sobre ellos por ser mas especifico.
 * @tparam Decl `ast::FunctionDecl` o `ast::ClassMethodDecl`.
 * @param d    Donde se escribe (puede ser @p from).
 * @param from De donde salen los pendientes.
 * @param ev   Evaluador de atomos.
 * @param err  Donde van los errores.
 */
template <class Decl>
void resolve_footprint_with(Decl &d, const Decl &from,
                            const cwhen::AtomEval &ev,
                            const cwhen::ErrFn &err) {
    if (from.footprint_pending.empty()) {
        d.footprint_pending.clear();
        return;
    }
    cwhen::ResolvedFP base;
    base.pure = d.contract_pure ? 1 : -1;
    base.nothrow_ = d.contract_nothrow ? 1 : -1;
    base.nopanic = d.contract_nopanic ? 1 : -1;
    base.alloc = d.contract_alloc;
    base.alloc_partial = d.contract_alloc_partial;
    base.stack = d.contract_stack;
    base.stack_partial = d.contract_stack_partial;
    cwhen::ResolvedFP r;
    cwhen::resolve_footprint(from.footprint_pending, base, ev, err, r);
    d.contract_pure = (r.pure == 1);
    d.contract_nothrow = (r.nothrow_ == 1);
    d.contract_nopanic = (r.nopanic == 1);
    d.contract_alloc = r.alloc;
    d.contract_alloc_partial = r.alloc_partial;
    d.contract_stack = r.stack;
    d.contract_stack_partial = r.stack_partial;
    d.footprint_pending.clear();
}

/**
 * @brief Los dos, para la copia @p nm de @p m en una instancia.
 * @tparam Decl `ast::FunctionDecl` o `ast::ClassMethodDecl`.
 * @param nm    La copia (con los campos directos ya copiados).
 * @param m     La plantilla.
 * @param g     La sustitucion de la instancia.
 * @param loc   Donde se pidio la instancia.
 * @param diags Donde se apuntan los errores.
 */
template <class Decl>
void resolve_instance_contracts(Decl &nm, const Decl &m,
                                const vxgen::GenSubst &g, const SourceLoc &loc,
                                Diagnostics &diags) {
    const cwhen::AtomEval ev = SubstAtomEval{g};
    const cwhen::ErrFn err = ErrorAt{diags, loc};
    resolve_complexity_with(nm, m, ev, err);
    resolve_footprint_with(nm, m, ev, err);
}

/**
 * @brief Los dos, sobre una declaracion que NO es de una plantilla.
 * @tparam Decl `ast::FunctionDecl` o `ast::ClassMethodDecl`.
 * @param d     La declaracion.
 * @param diags Donde se apuntan los errores.
 */
template <class Decl> void resolve_plain_contracts(Decl &d, Diagnostics &diags) {
    const cwhen::AtomEval ev = TargetOnlyAtomEval{};
    const cwhen::ErrFn err = ErrorAt{diags, d.loc};
    resolve_complexity_with(d, d, ev, err);
    resolve_footprint_with(d, d, ev, err);
}

} // namespace

void TypeChecker::resolve_pending_complexity_(ast::ClassMethodDecl &nm,
                                              const ast::ClassMethodDecl &m,
                                              const vxgen::GenSubst &g,
                                              const SourceLoc &loc) {
    resolve_instance_contracts(nm, m, g, loc, diags_);
}

void TypeChecker::resolve_pending_complexity_(ast::FunctionDecl &nm,
                                              const ast::FunctionDecl &m,
                                              const vxgen::GenSubst &g,
                                              const SourceLoc &loc) {
    resolve_instance_contracts(nm, m, g, loc, diags_);
}

void TypeChecker::resolve_complexity_decls_(
    std::vector<std::unique_ptr<ast::Node>> &decls) {
    for (auto &d : decls) {
        if (!d) continue;
        if (d->kind == ast::NodeKind::NamespaceDecl) {
            resolve_complexity_decls_(
                static_cast<ast::NamespaceDecl *>(d.get())->decls);
            continue;
        }
        if (d->kind == ast::NodeKind::FunctionDecl) {
            auto *fd = static_cast<ast::FunctionDecl *>(d.get());
            /* Una PLANTILLA se salta, como la de un struct o una clase: sus
             * `when:` sobre T los resuelve cada instancia al copiarla.  Aqui,
             * sin T, salian como "predicado mal formado". */
            if (!fd->type_params.empty() || fd->is_specialization) continue;
            resolve_plain_contracts(*fd, diags_);
        } else if (d->kind == ast::NodeKind::StructDecl) {
            auto *sd = static_cast<ast::StructDecl *>(d.get());
            if (!sd->type_params.empty() || sd->is_specialization) continue;
            for (auto &m : sd->methods)
                if (m) resolve_plain_contracts(*m, diags_);
        } else if (d->kind == ast::NodeKind::ClassDecl) {
            auto *cd = static_cast<ast::ClassDecl *>(d.get());
            if (!cd->type_params.empty()) continue;
            for (auto &m : cd->methods)
                if (m) resolve_plain_contracts(*m, diags_);
        }
    }
}

} // namespace vx
