/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file concept_shape_defaults.cpp
 * @brief El metodo por defecto de un concepto para quien lo cumple por FORMA
 *        sin declararlo.
 *
 * Quien DECLARA el concepto recibe sus metodos por defecto al montarse
 * (`concept_defaults.cpp`).  Quien lo cumple sin decirlo -- tiene los campos y
 * los metodos exigidos -- tambien puede llamarlos: `x.vacio()` sobre un tipo
 * que no escribe `vacio` pero cumple `Contable`.  Es la generica UFCS
 * `<Self: C<E>, E> R m(Self this, ...)` del diseno, instanciada para ESE tipo
 * en el momento en que se llama, que es cuando se sabe quien es `Self`.
 *
 * Instanciarla como metodo del tipo, y no como funcion libre, tiene dos
 * razones: el receptor va por REFERENCIA, como en cualquier metodo -- una
 * generica que tomara `Self` por valor perderia lo que un metodo por defecto
 * cambie en el (un `next()` que avanza) --, y el cuerpo se escribe con `this`,
 * que es como se escribe en el concepto.
 *
 * Los argumentos del concepto (`E`) se DEDUCEN del tipo: sus campos y sus
 * metodos exigidos se ligan contra los del tipo con el mismo deductor que las
 * genericas (@c generics::match_type_pattern).  Lo escrito gana: se llega aqui
 * solo si el tipo no tiene el metodo y ninguna funcion libre lo toma.
 */

#include "vx/type_checker.h"

#include "vx/generics/concepts.h"
#include "vx/generics/generic_clone.h"
#include "vx/generics/generic_infer.h"
#include "vx/generics/member_clone.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace vx {
namespace {

/**
 * @struct ShapeCandidate
 * @brief Un metodo por defecto que podria servir: de que concepto, cual, y
 *        con que argumentos se cumple.
 */
struct ShapeCandidate {
    const ast::ConceptDecl *concept_decl = nullptr;
    const ast::ClassMethodDecl *method = nullptr;
    std::vector<Type> args; ///< todos: el tipo y los argumentos deducidos
};

/**
 * @enum Deduction
 * @brief Como acabo la deduccion de los argumentos de un concepto.
 */
enum class Deduction : uint8_t {
    Deduced,    ///< todos ligados
    NoShape,    ///< al tipo le falta un campo o un metodo exigido
    Undeduced,  ///< tiene la forma, pero algun argumento no sale de ella
};

/**
 * @brief Los campos y los metodos de un tipo con layout.
 * @param tc     El comprobador.
 * @param t      El tipo.
 * @param fields Recibe sus campos.
 * @param methods Recibe sus metodos.
 * @return Falso si no es un struct ni una clase con layout.
 */
bool members_of(const TypeChecker &tc, const Type &t,
                const std::vector<StructFieldInfo> *&fields,
                const std::vector<ClassMethodInfo> *&methods) {
    if (t.kind == PrimitiveKind::STRUCT) {
        const auto it = tc.struct_layouts().find(t.struct_name);
        if (it == tc.struct_layouts().end()) return false;
        fields = &it->second.fields;
        methods = &it->second.methods;
        return true;
    }
    if (t.kind == PrimitiveKind::CLASS) {
        const auto it = tc.class_layouts().find(t.struct_name);
        if (it == tc.class_layouts().end()) return false;
        fields = &it->second.fields;
        methods = &it->second.methods;
        return true;
    }
    return false;
}

/**
 * @brief Deduce los argumentos de @p cd para el tipo @p t ligando los campos y
 *        los metodos EXIGIDOS del concepto contra los del tipo.
 * @param tc  El comprobador.
 * @param cd  El concepto.
 * @param t   El tipo (el primer parametro del concepto).
 * @param out Recibe todos los argumentos (el tipo primero).
 * @param missing Recibe el primer parametro que no salio (si @c Undeduced).
 * @return Como acabo.
 */
Deduction deduce_concept_args(TypeChecker &tc, const ast::ConceptDecl &cd,
                              const Type &t, std::vector<Type> &out,
                              std::string &missing) {
    const std::vector<StructFieldInfo> *fields = nullptr;
    const std::vector<ClassMethodInfo> *methods = nullptr;
    if (!members_of(tc, t, fields, methods)) return Deduction::NoShape;
    const std::vector<std::string> &vars = cd.type_params;
    out.assign(vars.size(), Type{});
    uint32_t bound = 0;
    if (!vars.empty()) {
        out[0] = t; // el primero es el tipo que lo cumple
        bound = 1u;
    }
    for (const ast::StructFieldDecl &f : cd.fields) {
        const StructFieldInfo *have = nullptr;
        for (const StructFieldInfo &fi : *fields)
            if (fi.name == f.name) {
                have = &fi;
                break;
            }
        if (have == nullptr ||
            !generics::match_type_pattern(tc, f.type.get(), have->type, vars,
                                          out.data(), bound))
            return Deduction::NoShape;
    }
    for (const auto &m : cd.methods) {
        if (!m || m->body) continue; // solo los exigidos dan forma
        bool matched = false;
        for (const ClassMethodInfo &mi : *methods) {
            if (mi.is_constructor || mi.is_destructor || mi.name != m->name ||
                mi.param_types.size() != m->params.size())
                continue;
            /* Se prueba sobre una copia: una sobrecarga que no casa no puede
             * dejar ligada una variable. */
            std::vector<Type> trial = out;
            uint32_t trial_bound = bound;
            bool ok = m->return_type
                          ? generics::match_type_pattern(
                                tc, m->return_type.get(), mi.return_type, vars,
                                trial.data(), trial_bound)
                          : mi.return_type.kind == PrimitiveKind::VOID;
            for (size_t i = 0; ok && i < m->params.size(); ++i)
                ok = generics::match_type_pattern(tc, m->params[i]->type.get(),
                                                  mi.param_types[i], vars,
                                                  trial.data(), trial_bound);
            if (!ok) continue;
            out.swap(trial);
            bound = trial_bound;
            matched = true;
            break;
        }
        if (!matched) return Deduction::NoShape;
    }
    for (size_t i = 0; i < vars.size(); ++i)
        if ((bound & (1u << i)) == 0) {
            missing = vars[i];
            return Deduction::Undeduced;
        }
    return Deduction::Deduced;
}

/**
 * @brief La procedencia de un metodo instanciado para quien cumple por forma.
 * @param tc   El comprobador (escribe el nombre como en el fuente).
 * @param cd   El concepto.
 * @param args Sus argumentos (el tipo primero).
 * @param m    El metodo del concepto.
 * @return La procedencia: el concepto con los argumentos DEDUCIDOS.
 */
ast::MemberOrigin shape_origin(const TypeChecker &tc, const ast::ConceptDecl &cd,
                               const std::vector<Type> &args,
                               const ast::ClassMethodDecl &m) {
    ast::MemberOrigin o;
    o.kind = ast::MemberOriginKind::Concept;
    o.via.name = util::InternedName::intern(tc.written_name(cd.name));
    o.via.loc = cd.loc;
    for (size_t i = 1; i < args.size(); ++i)
        o.via.args.push_back(std::shared_ptr<ast::TypeNode>(
            vxgen::type_node_from_type(args[i], cd.loc)));
    o.original = m.loc;
    return o;
}

} // namespace

TypeChecker::ConceptDefaultOutcome
TypeChecker::concept_default_for(const Type &recv, const std::string &name,
                                 size_t arity, const SourceLoc &loc,
                                 const ClassMethodInfo *&out) {
    out = nullptr;
    if (recv.kind != PrimitiveKind::STRUCT && recv.kind != PrimitiveKind::CLASS)
        return ConceptDefaultOutcome::NotApplicable;
    std::vector<ShapeCandidate> found;
    const ast::ConceptDecl *undeduced_in = nullptr;
    std::string undeduced_param;
    for (const auto &kv : concepts_) {
        const ast::ConceptDecl *cd = kv.second;
        if (cd == nullptr || cd->ckind != ast::ConceptKind::Structural)
            continue;
        const ast::ClassMethodDecl *m = nullptr;
        for (const auto &cm : cd->methods)
            if (cm && cm->body && cm->name == name &&
                cm->params.size() == arity && cm->method_type_params.empty()) {
                m = cm.get();
                break;
            }
        if (m == nullptr) continue;
        std::vector<Type> args;
        std::string missing;
        switch (deduce_concept_args(*this, *cd, recv, args, missing)) {
        case Deduction::NoShape: continue;
        case Deduction::Undeduced:
            undeduced_in = cd;
            undeduced_param = missing;
            continue;
        case Deduction::Deduced: break;
        }
        /* Deducidos, el concepto lo dice (su comprobacion es la unica). */
        const ConceptArgs cargs(args.begin() + (args.empty() ? 0 : 1),
                                args.end());
        if (!comptime_eval_concept(*this, cd->name, recv, cargs).satisfied)
            continue;
        found.push_back(ShapeCandidate{cd, m, std::move(args)});
    }
    if (found.empty()) {
        if (undeduced_in == nullptr) return ConceptDefaultOutcome::NotApplicable;
        diags_.diag(loc, DiagLevel::ERR, "VX2162",
                    {name, written_type_name(recv),
                     written_name(undeduced_in->name), undeduced_param});
        return ConceptDefaultOutcome::Reported;
    }
    if (found.size() > 1) {
        /* En orden de nombre: la tabla de conceptos no tiene orden, y el
         * mismo programa tiene que dar el mismo mensaje. */
        std::vector<std::string> names;
        names.reserve(found.size());
        for (const ShapeCandidate &c : found)
            names.push_back(written_name(c.concept_decl->name));
        std::sort(names.begin(), names.end());
        std::string which;
        for (const std::string &n : names) {
            if (!which.empty()) which += ", ";
            which += n;
        }
        diags_.diag(loc, DiagLevel::ERR, "VX2163",
                    {name, written_type_name(recv), which});
        return ConceptDefaultOutcome::Reported;
    }
    const ShapeCandidate &c = found.front();
    const vxgen::GenSubst g{&c.concept_decl->type_params, &c.args};
    auto nm = vxgen::clone_method_with_subst(*c.method, g,
                                             vxgen::MethodBodyCopy::Clone);
    nm->origin = shape_origin(*this, *c.concept_decl, c.args, *c.method);
    out = add_method_late(recv.struct_name, recv.kind, std::move(nm));
    return out != nullptr ? ConceptDefaultOutcome::Found
                          : ConceptDefaultOutcome::NotApplicable;
}

} // namespace vx
