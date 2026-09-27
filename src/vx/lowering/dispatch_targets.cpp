/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file dispatch_targets.cpp
 * @brief Quien puede estar detras de un despacho dinamico, y lo que eso
 *        permite afirmar o solo adivinar.
 *
 * Una sola enumeracion de la jerarquia y dos lecturas de ella: la APUESTA
 * (pocas clases posibles: se compara y se llama directo, con respaldo) y la
 * CERTEZA (todas resuelven a la misma funcion: llamada directa sin mas).  La
 * certeza viaja dentro del hecho que baja al intermedio; la apuesta, en su
 * tabla lateral.
 */

#include "vx/lowering.h"
#include "vx/method_names.h"
#include "vx/overload.h"
#include "lowering_internal.h"

namespace vx {

/**
 * @brief Si la clase @p cl puede estar detras de un receptor declarado como
 *        @p static_class.
 * @param layouts      Las clases del programa.
 * @param cl           Clase candidata.
 * @param static_class Tipo declarado.
 * @param is_interface Si el tipo declarado es una interfaz.
 * @return true si cumple la interfaz, o es el tipo o desciende de el.
 */
/// Hasta donde se sube por una cadena de herencia antes de darla por rota.
static constexpr int kMaxHierarchyDepth = 64;

/**
 * @brief Si la interfaz @p name es @p target o la extiende.
 * @param layouts Las clases e interfaces del programa.
 * @param name    Interfaz declarada.
 * @param target  Interfaz buscada.
 * @param depth   Profundidad recorrida.
 * @return true si @p name cumple @p target.
 */
static bool
interface_reaches(const std::unordered_map<std::string, ClassLayout> &layouts,
                  const std::string &name, const std::string &target,
                  int depth) {
    if (name == target) return true;
    if (depth >= kMaxHierarchyDepth) return false;
    const auto it = layouts.find(name);
    if (it == layouts.end()) return false;
    if (!it->second.super_name.empty() &&
        interface_reaches(layouts, it->second.super_name, target, depth + 1))
        return true;
    for (const auto &up : it->second.interface_names)
        if (interface_reaches(layouts, up, target, depth + 1)) return true;
    return false;
}

static bool
fits_static_type(const std::unordered_map<std::string, ClassLayout> &layouts,
                 const ClassLayout &cl, const std::string &static_class,
                 bool is_interface) {
    /* Se sube por la cadena de la clase: una interfaz se cumple tambien si la
     * declara un ANCESTRO.  Mirar solo las de la propia clase dejaba fuera a
     * las subclases -- `Perro : Animal`, con `Animal : Habla` -- y la
     * jerarquia "demostraba" el metodo de la base. */
    std::string cur = cl.name;
    for (int depth = 0; !cur.empty() && depth < kMaxHierarchyDepth; ++depth) {
        const auto itc = layouts.find(cur);
        if (!is_interface && cur == static_class) return true;
        if (itc == layouts.end()) return false;
        if (is_interface)
            for (const auto &in : itc->second.interface_names)
                if (interface_reaches(layouts, in, static_class, 0))
                    return true;
        cur = itc->second.super_name;
    }
    return false;
}

/**
 * @copydoc vx::Lowering::dispatch_impls
 */
std::vector<Lowering::DispatchImpl>
Lowering::dispatch_impls(const std::string &static_class,
                         const ClassMethodInfo &target,
                         bool is_interface) const {
    std::vector<DispatchImpl> impls;
    for (const auto &kv : tc_.class_layouts()) {
        const ClassLayout &cl = kv.second;
        if (cl.is_interface || cl.is_aspect) continue;
        if (!fits_static_type(tc_.class_layouts(), cl, static_class,
                              is_interface))
            continue;

        /* Quien DEFINE el metodo: puede estar heredado sin aplanar, asi que se
         * sube por la cadena.  No vale buscar el primero con ese nombre: un
         * constructor puede llamarse igual y dejaria sin encontrar al que se
         * busca, y con SOBRECARGA tampoco basta el nombre -- hay que dar con el
         * de la MISMA firma, o se resuelve hacia otro metodo --.  Firma
         * ENTERA, tipos y nombres de ranura: dos hermanas que solo se
         * distinguen por como se llaman sus ranuras empatan por tipos. */
        const ClassMethodInfo *impl = nullptr;
        for (const ClassMethodInfo &mm : cl.methods) {
            if (mm.is_constructor || mm.name != target.name) continue;
            if (!overload::same_signature(mm.param_types, mm.param_names,
                                          target.param_types,
                                          target.param_names))
                continue;
            impl = &mm;
            break;
        }
        if (impl == nullptr) continue;

        const std::string callee = method_symbol_of(*impl);
        DispatchImpl d;
        d.cls = util::InternedName::intern(cl.name);
        d.callee = util::InternedName::intern(callee);
        d.advice = advice_chains_.count(callee) != 0 ? DispatchAdvice::Woven
                                                     : DispatchAdvice::None;
        impls.push_back(d);
    }
    return impls;
}

/**
 * @copydoc vx::Lowering::spec_devirt_impls
 */
std::vector<Lowering::DispatchImpl>
Lowering::spec_devirt_impls(const std::string &static_class,
                            const ClassMethodInfo &target,
                            bool is_interface) const {
    std::vector<DispatchImpl> impls;
    /* Basta un aspecto que no se haya podido atribuir a un metodo concreto
     * para no adivinar en ningun sitio: podria apuntar a cualquiera. */
    if (!all_advices_attributed_) return impls;

    constexpr size_t K_MAX = 4;
    for (const DispatchImpl &d :
         dispatch_impls(static_class, target, is_interface)) {
        // Con aspectos, sus objetos van por el despacho normal.
        if (d.advice == DispatchAdvice::Woven) continue;
        impls.push_back(d);
        if (impls.size() > K_MAX) return {}; // demasiados: no compensa
    }
    return impls;
}

/**
 * @copydoc vx::Lowering::dispatch_hierarchy_closed
 */
bool Lowering::dispatch_hierarchy_closed(
    const std::string &static_class) const {
    return !tc_.is_imported(static_class);
}

/**
 * @copydoc vx::Lowering::proven_exact_callee
 */
util::InternedName
Lowering::proven_exact_callee(const std::string &callee) const {
    if (!all_advices_attributed_ || advice_chains_.count(callee) != 0)
        return util::InternedName();
    return util::InternedName::intern(callee);
}

/**
 * @copydoc vx::Lowering::note_same_value
 */
void Lowering::note_same_value(ir::IrValueId dst, ir::IrValueId src) {
    const auto conc = ssa_concrete_class_.find(src);
    if (conc != ssa_concrete_class_.end())
        ssa_concrete_class_[dst] = conc->second;
    const auto refl = reflect_origin_.find(src);
    if (refl != reflect_origin_.end()) reflect_origin_[dst] = refl->second;
}

/**
 * @copydoc vx::Lowering::reflect_proven_method
 */
util::InternedName
Lowering::reflect_proven_method(const std::string &cls,
                                const std::string &name) const {
    if (!dispatch_hierarchy_closed(cls)) return util::InternedName();
    const auto it = tc_.class_layouts().find(cls);
    if (it == tc_.class_layouts().end()) return util::InternedName();
    const ClassMethodInfo *found = nullptr;
    for (const ClassMethodInfo &m : it->second.methods) {
        if (m.is_constructor || m.name != name) continue;
        if (found != nullptr) return util::InternedName(); // compartido: lanza
        found = &m;
    }
    if (found == nullptr) return util::InternedName();
    return proven_exact_callee(method_symbol_of(*found));
}

/**
 * @copydoc vx::Lowering::proven_dispatch_callee
 */
util::InternedName
Lowering::proven_dispatch_callee(const std::string &static_class,
                                 const ClassMethodInfo &target,
                                 bool is_interface) const {
    /* Un aspecto sin atribuir podria apuntar a cualquier metodo; y una
     * jerarquia abierta puede tener detras clases que aqui no se ven. */
    if (!all_advices_attributed_ || !dispatch_hierarchy_closed(static_class))
        return util::InternedName();
    const std::vector<DispatchImpl> impls =
        dispatch_impls(static_class, target, is_interface);
    if (impls.empty()) return util::InternedName();
    for (const DispatchImpl &d : impls)
        if (d.callee != impls.front().callee) return util::InternedName();
    return proven_exact_callee(impls.front().callee.str());
}

} // namespace vx
