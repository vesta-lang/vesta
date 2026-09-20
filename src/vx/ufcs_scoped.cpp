/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 */

/**
 * @file ufcs_scoped.cpp
 * @brief Que se puede llamar sobre un tipo desde aqui: la enumeracion.
 *
 * Lo de fuera esta en @c vx/ufcs_scoped.h.  Aqui solo esta COMO se junta: los
 * metodos reales del tipo y las libres alcanzables, que las da el indice
 * (@c ufcs::Index::reachable_for) porque el criterio de alcance es suyo.
 *
 * Nada de aqui construye cadenas ni copia firmas: una entrada son cuatro
 * punteros y un indice.
 */

#include "vx/ufcs_scoped.h"

#include "vx/type_checker.h"
#include "vx/ufcs.h"

#include <algorithm>

namespace vx {

/**
 * @brief El orden en que se enumera: estable entre compilaciones.
 *
 * El indice que `scoped.method.name<T>(i)` recibe tiene que significar lo
 * mismo en dos compilaciones del mismo programa; si saliera del recorrido de
 * una tabla hash, un desenrollado en comptime produciria codigo distinto cada
 * vez.
 *
 * Por NOMBRE, y a igualdad por origen, por si es real y por su sitio en la
 * tabla de firmas -- que es el orden en que se declararon --.  La enumeracion
 * recorre SOBRECARGAS, asi que dos `f` con firmas distintas son dos entradas y
 * hay que separarlas con algo que no dependa de por donde se llego.
 *
 * Los nombres estan internados, asi que dos iguales lo son de una resta y el
 * texto solo se mira cuando difieren.
 *
 * @param a Una entrada.
 * @param b La otra.
 * @return Si @p a va antes.
 */
static bool scoped_before(const ScopedMethod &a, const ScopedMethod &b) {
    if (a.name != b.name) return *a.name < *b.name;
    if (a.origin != b.origin) {
        if (a.origin == nullptr) return true;
        if (b.origin == nullptr) return false;
        return *a.origin < *b.origin;
    }
    /* Los del tipo antes que los que solo se alcanzan desde aqui: lo que no
     * depende del ambito primero. */
    const bool a_real = a.real != nullptr;
    const bool b_real = b.real != nullptr;
    if (a_real != b_real) return a_real;
    if (a_real) return false; // entre reales manda el orden de declaracion
    return a.slot < b.slot;
}

void TypeChecker::collect_scoped_methods(const Type &recv,
                                         const std::string &site_prefix,
                                         std::vector<ScopedMethod> &out) const {
    out.clear();

    /* 1. Los metodos REALES del tipo.  Van primero porque son la parte de la
     *    respuesta que no depende de quien pregunte.
     *
     *    Las dos tablas, y no solo la de clases: un struct tiene sus metodos
     *    en la suya, asi que mirar solo `class_layouts_` devuelve CERO para un
     *    struct con metodos -- y eso no se ve como un fallo, se ve como un
     *    tipo que no tiene ninguno. */
    const std::vector<ClassMethodInfo> *own_methods = nullptr;
    if (recv.kind == PrimitiveKind::CLASS) {
        auto cl = class_layouts_.find(recv.struct_name);
        if (cl != class_layouts_.end()) own_methods = &cl->second.methods;
    } else if (recv.kind == PrimitiveKind::STRUCT) {
        auto st = struct_layouts_.find(recv.struct_name);
        if (st != struct_layouts_.end()) own_methods = &st->second.methods;
        if (own_methods == nullptr) {
            /* Una CLASE tambien llega con esta marca en algunos caminos; si
             * no estaba entre los structs se prueba alli antes de rendirse. */
            auto cl = class_layouts_.find(recv.struct_name);
            if (cl != class_layouts_.end()) own_methods = &cl->second.methods;
        }
    }
    if (own_methods != nullptr) {
        out.reserve(own_methods->size());
        for (const ClassMethodInfo &m : *own_methods) {
            ScopedMethod sm;
            sm.name = util::intern_name(m.name);
            sm.origin = nullptr; // nulo = metodo real
            sm.receiver = recv;
            sm.slot = kScopedNoSlot;
            sm.real = &m;
            out.push_back(sm);
        }
    }

    /* 2. Y las libres que este fichero ALCANZA.  Quien decide que alcanza es
     *    el indice, en el mismo fichero donde se decide al resolver: si el
     *    criterio viviera aqui habria dos, y el modo de fallar es que el
     *    editor ofrezca por el punto lo que la llamada rechaza. */
    std::vector<ufcs::Index::Reachable> free_fns;
    ufcs_.reachable_for(ufcs_key_type(recv), site_prefix, free_fns);
    out.reserve(out.size() + free_fns.size());
    for (const ufcs::Index::Reachable &r : free_fns) {
        const FunctionSig *sig = function_sig_at(r.slot);
        /* Sin parametros no hay receptor posible, asi que no es alcanzable por
         * el punto.  El indice no las mete, pero comprobarlo aqui cuesta una
         * rama y cubre a quien declare por otra via. */
        if (sig == nullptr || sig->param_types.empty()) continue;
        ScopedMethod sm;
        sm.name = r.name;
        sm.origin = r.origin;
        sm.receiver = recv;
        sm.slot = r.slot;
        sm.real = nullptr;
        out.push_back(sm);
    }

    /* Y en un orden que no dependa del recorrido de ninguna tabla hash: ver
     * @c scoped_before. */
    std::stable_sort(out.begin(), out.end(), scoped_before);
}

uint32_t scoped_arity(const TypeChecker &tc, const ScopedMethod &m) {
    if (m.real != nullptr) {
        /* `this` es implicito y no esta en la lista, asi que se cuenta: el
         * receptor es el parametro 0 tambien aqui. */
        return static_cast<uint32_t>(m.real->param_types.size()) + 1u;
    }
    const FunctionSig *sig = tc.function_sig_at(m.slot);
    if (sig == nullptr) return 0;
    /* Aqui el receptor YA es el primer parametro: la regla se cumple sola. */
    return static_cast<uint32_t>(sig->param_types.size());
}

Type scoped_param_type(const TypeChecker &tc, const ScopedMethod &m,
                       uint32_t j) {
    if (m.real != nullptr) {
        if (j == 0) return m.receiver;
        const uint32_t at = j - 1u;
        if (at >= m.real->param_types.size()) return Type{PrimitiveKind::VOID};
        return m.real->param_types[at];
    }
    const FunctionSig *sig = tc.function_sig_at(m.slot);
    if (sig == nullptr || j >= sig->param_types.size())
        return Type{PrimitiveKind::VOID};
    return sig->param_types[j];
}

Type scoped_return_type(const TypeChecker &tc, const ScopedMethod &m) {
    if (m.real != nullptr) return m.real->return_type;
    const FunctionSig *sig = tc.function_sig_at(m.slot);
    if (sig == nullptr) return Type{PrimitiveKind::VOID};
    return sig->return_type;
}

} // namespace vx
