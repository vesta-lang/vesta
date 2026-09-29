/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file children_walker.h
 * @brief El motor del recorrido de hijos: saca los hijos de un nodo de su
 *        lista de campos (`vx/ast/fields.h`) y los ambitos de
 *        `vx/ast/scopes.h`.  Solo lo incluye `vx/ast/children.h`.
 *
 * Que tipo de campo cuelga hijos lo decide `vx/ast/children_traits.h`; aqui
 * solo se recorre: los campos en el orden de la lista, cada hijo al visitante
 * (como nodo o como ranura) y, con ambito, los avisos de apertura, cierre y
 * nombres ligados.
 */

#ifndef VX_AST_CHILDREN_WALKER_H
#define VX_AST_CHILDREN_WALKER_H

#include "vx/ast/child_slot.h"
#include "vx/ast/children_traits.h"
#include "vx/ast/fields.h"
#include "vx/ast/kind_dispatch.h"
#include "vx/ast/scopes.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace vx::ast {

/**
 * @enum ChildSet
 * @brief Que hijos cuentan.
 */
enum class ChildSet : uint8_t {
    Parsed,             ///< solo los que escribio el programador (parser)
    ParsedAndAnnotated, ///< ademas los subarboles que cuelga el comprobador
};

/**
 * @struct BasicBinding
 * @brief Un nombre que se declara en el ambito abierto: cual, quien lo liga y
 *        donde se escribio.
 * @tparam Name `const std::string` al leer, `std::string` al reescribir (un
 *              renombrador cambia el nombre donde se declara).
 */
template <class Name> struct BasicBinding {
    Name &name;           ///< el nombre ligado
    const Node *binder;   ///< el nodo que lo liga (para una pieza, su dueno)
    const SourceLoc &loc; ///< donde se escribio quien lo liga
};

/// Nombre ligado visto desde un recorrido de lectura.
using Binding = BasicBinding<const std::string>;
/// Nombre ligado visto desde un recorrido que reescribe.
using BindingSlot = BasicBinding<std::string>;

namespace children_detail {

/**
 * @enum Access
 * @brief Si el recorrido lee los hijos o entrega sus ranuras.
 */
enum class Access : uint8_t { Read, Write };

/**
 * @enum Scoping
 * @brief Si el recorrido avisa de los ambitos y de los nombres que ligan.
 */
enum class Scoping : uint8_t { Plain, Scoped };

/**
 * @struct FieldStep
 * @brief Visitante de la lista de campos de un objeto: entrega al motor los
 *        miembros que admite @p Filter, cada uno segun su papel.
 */
template <class W, class Obj, class Filter> struct FieldStep {
    W &w;              ///< el motor
    Obj &obj;          ///< nodo o pieza cuyos campos se recorren
    const Node *owner; ///< el nodo al que pertenece (el mismo, si es nodo)

    /**
     * @brief Un campo del parser.
     * @param f El campo.
     */
    template <class C, class M>
    void operator()(const fields::Field<fields::Role::Parsed, C, M> &f) {
        if (Filter::admits(f.member)) w.parsed_member(obj.*f.member, owner);
    }

    /**
     * @brief Un campo anotado: solo cuenta si el recorrido lo pidio.
     * @param f El campo.
     */
    template <class C, class M>
    void operator()(const fields::Field<fields::Role::Annotated, C, M> &f) {
        if (Filter::admits(f.member)) w.annotated_member(obj.*f.member);
    }
};

/**
 * @struct ConcreteStep
 * @brief Visitante de `visit_*`: recorre los campos del nodo ya con su clase.
 */
template <class W> struct ConcreteStep {
    W &w; ///< el motor

    /**
     * @brief Un nodo con su clase concreta.
     * @param n El nodo.
     */
    template <class T> void operator()(T &n) { w.walk_fields(n, &n); }
};

/**
 * @struct EnclosingStep
 * @brief Visitante de `visit_stmt`: si la sentencia liga un nombre en el
 *        ambito que la contiene (@ref scopes::BindsInEnclosing), lo declara.
 */
template <class W> struct EnclosingStep {
    W &w; ///< el motor

    /**
     * @brief Una sentencia con su clase concreta.
     * @param s La sentencia.
     */
    template <class T> void operator()(T &s) {
        using U = std::remove_const_t<T>;
        if constexpr (scopes::BindsInEnclosing<U>::value)
            w.declare_name(s.*scopes::BindsInEnclosing<U>::name(), &s, s.loc);
    }
};

/**
 * @class Walker
 * @brief El recorrido de los hijos directos de un nodo.
 * @tparam A Lee los hijos o entrega sus ranuras.
 * @tparam S Avisa o no de ambitos y nombres.
 * @tparam V Visitante (ver `vx/ast/children.h` para lo que tiene que tener).
 */
template <Access A, Scoping S, class V> class Walker {
  public:
    /**
     * @brief Motor sobre un visitante.
     * @param v   Visitante.
     * @param set Que hijos cuentan.
     */
    Walker(V &v, ChildSet set) : v_(v), set_(set) {}

    /**
     * @brief Recorre los hijos directos de @p n.
     * @param n Nodo visto por su familia (se despacha por su etiqueta) o por
     *          su clase concreta (sin despacho).
     */
    template <class N> void node(N &n) {
        using U = std::remove_const_t<N>;
        ConcreteStep<Walker> st{*this};
        if constexpr (std::is_same<U, Expr>::value) {
            if (!visit_expr(n, st)) abort_unexpected_node(n, "Expr", kWho);
        } else if constexpr (std::is_same<U, Stmt>::value) {
            if (!visit_stmt(n, st)) abort_unexpected_node(n, "Stmt", kWho);
        } else if constexpr (std::is_same<U, TypeNode>::value) {
            if (!visit_type(n, st)) abort_unexpected_node(n, "TypeNode", kWho);
        } else {
            static_assert(std::is_base_of<Node, U>::value &&
                              fields::HasFields<U>::value,
                          "for_each_child recibe un nodo con lista de campos");
            walk_fields(n, &n);
        }
    }

    /**
     * @brief Recorre los campos de un nodo o pieza; con ambitos, primero los
     *        de fuera y despues cada ambito con sus nombres.
     * @param obj   Nodo o pieza.
     * @param owner Nodo al que pertenece (el mismo, si es un nodo).
     */
    template <class Obj> void walk_fields(Obj &obj, const Node *owner) {
        using T = std::remove_const_t<Obj>;
        if constexpr (S == Scoping::Plain) {
            FieldStep<Walker, Obj, AllFields> st{*this, obj, owner};
            fields::for_each_field<T>(st);
        } else {
            FieldStep<Walker, Obj, OutsideScopes<T>> st{*this, obj, owner};
            fields::for_each_field<T>(st);
            open_scopes(obj, owner,
                        std::make_index_sequence<std::tuple_size<
                            decltype(scopes::ScopesOf<T>::list())>::value>{});
        }
    }

    /**
     * @brief Un miembro que escribio el parser.
     * @param m     El miembro.
     * @param owner Nodo al que pertenece.
     */
    template <class M> void parsed_member(M &m, const Node *owner) {
        using U = std::remove_const_t<M>;
        if constexpr (IsOwnedNode<U>::value) {
            child(m);
        } else if constexpr (IsChildList<U>::value) {
            for (auto &e : m)
                parsed_member(e, owner);
        } else if constexpr (fields::HasFields<U>::value) {
            walk_fields(m, owner);
        } else {
            static_assert(!MentionsNode<U>::value,
                          "un campo del parser cuelga nodos de una forma que "
                          "el recorrido de hijos no sabe visitar");
        }
    }

    /**
     * @brief Un miembro anotado: sus subarboles DUENOS, si se pidieron.  Un
     *        puntero no dueno (el metodo resuelto) no es un hijo.
     * @param m El miembro.
     */
    template <class M> void annotated_member(M &m) {
        using U = std::remove_const_t<M>;
        if (set_ != ChildSet::ParsedAndAnnotated) return;
        if constexpr (IsOwnedNode<U>::value) {
            child(m);
        } else if constexpr (IsOwnedNodeList<U>::value) {
            for (auto &e : m)
                child(e);
        }
    }

    /**
     * @brief Declara un nombre en el ambito abierto (vacio: nada).
     * @param name   El nombre.
     * @param binder Quien lo liga.
     * @param loc    Donde.
     */
    template <class Str>
    void declare_name(Str &name, const Node *binder, const SourceLoc &loc) {
        if (name.empty()) return;
        if constexpr (A == Access::Read)
            v_.declare(Binding{name, binder, loc});
        else
            v_.declare(BindingSlot{name, binder, loc});
    }

  private:
    static constexpr const char *kWho = "for_each_child"; ///< para VXE943

    /**
     * @brief Un hijo: al visitante, como nodo o como ranura.
     * @param p El puntero que lo guarda.
     */
    template <class Ptr> void child(Ptr &p) {
        using U = std::remove_const_t<Ptr>;
        using P = typename U::element_type;
        using Fam = typename FamilyOf<P>::type;
        static_assert(!std::is_void<Fam>::value,
                      "un hijo que no es Expr, Stmt, TypeNode ni ParamDecl");
        if (!p) return;
        if constexpr (A == Access::Read) {
            const Fam &c = *p;
            v_(c);
        } else if constexpr (IsShared<U>::value) {
            v_(SharedChildSlot<P>(p));
        } else {
            v_(ChildSlot<P>(p));
        }
        if constexpr (S == Scoping::Scoped && std::is_same<Fam, Stmt>::value) {
            // La ranura pudo cambiar de hijo: se mira el que quedo.
            if (!p) return;
            EnclosingStep<Walker> st{*this};
            if constexpr (A == Access::Read) {
                const Stmt &s = *p;
                if (!visit_stmt(s, st)) abort_unexpected_node(s, "Stmt", kWho);
            } else {
                Stmt &s = *p;
                if (!visit_stmt(s, st)) abort_unexpected_node(s, "Stmt", kWho);
            }
        }
    }

    /**
     * @brief Abre, uno tras otro, los ambitos de @p obj.
     * @param obj   Nodo o pieza.
     * @param owner Nodo al que pertenece.
     */
    template <class Obj, size_t... I>
    void open_scopes([[maybe_unused]] Obj &obj,
                     [[maybe_unused]] const Node *owner,
                     std::index_sequence<I...>) {
        // Sin ambitos (la mayoria de nodos) el pliegue queda vacio.
        (open_scope<I>(obj, owner), ...);
    }

    /**
     * @brief Abre el ambito @p I de @p obj: declara sus nombres, recorre los
     *        campos que cubre y lo cierra.
     * @param obj   Nodo o pieza.
     * @param owner Nodo al que pertenece.
     */
    template <size_t I, class Obj> void open_scope(Obj &obj, const Node *owner) {
        using T = std::remove_const_t<Obj>;
        constexpr auto sc = std::get<I>(scopes::ScopesOf<T>::list());
        v_.enter_scope();
        if constexpr (!std::is_same<std::decay_t<decltype(sc.names)>,
                                    scopes::NoNames>::value)
            declare(obj.*sc.names, owner, obj.loc);
        FieldStep<Walker, Obj, InScope<T, I>> st{*this, obj, owner};
        fields::for_each_field<T>(st);
        v_.exit_scope();
    }

    /**
     * @brief Declara los nombres de un miembro que liga: uno, una lista, o
     *        los parametros de una lambda (cada uno ligado por su `ParamDecl`).
     * @param names  El miembro.
     * @param binder Quien liga.
     * @param loc    Donde.
     */
    template <class Names>
    void declare(Names &names, const Node *binder, const SourceLoc &loc) {
        using U = std::remove_const_t<Names>;
        if constexpr (std::is_same<U, std::string>::value) {
            declare_name(names, binder, loc);
        } else if constexpr (std::is_same<U, std::vector<std::string>>::value) {
            for (auto &n : names)
                declare_name(n, binder, loc);
        } else if constexpr (std::is_same<U, std::vector<std::unique_ptr<
                                                 ParamDecl>>>::value) {
            for (auto &p : names)
                if (p) declare_name(p->name, p.get(), p->loc);
        } else {
            static_assert(AlwaysFalse<U>::value,
                          "un ambito declara nombres de un miembro que el "
                          "recorrido no sabe leer");
        }
    }

    V &v_;         ///< el visitante
    ChildSet set_; ///< que hijos cuentan
};

} // namespace children_detail

} // namespace vx::ast

#endif // VX_AST_CHILDREN_WALKER_H
