/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file child_slot.h
 * @brief La RANURA de un hijo del arbol: el sitio donde cuelga, para leerlo o
 *        sustituirlo sin saber como se guarda.
 *
 * Una reescritura (`x` -> `this.x`, envolver en `Some(...)`) necesita cambiar
 * el hijo, no solo verlo.  Hoy un hijo es un `std::unique_ptr` dentro de su
 * padre; manana sera un indice en un arena (plan PI4).  Si cada reescritura
 * toca el `unique_ptr` directamente, esa migracion toca cada reescritura.  Con
 * la ranura en medio solo cambia este fichero: el consumidor pregunta que hay
 * (@ref ChildSlot::get), pone otra cosa (@ref ChildSlot::replace) o se lleva
 * lo que habia para envolverlo (@ref ChildSlot::take).
 *
 * Una ranura es una vista: vale mientras el padre no cambie de forma (no se
 * guarda mas alla de la visita que la recibe).
 */

#ifndef VX_AST_CHILD_SLOT_H
#define VX_AST_CHILD_SLOT_H

#include <memory>
#include <utility>

namespace vx::ast {

/**
 * @class ChildSlot
 * @brief Ranura de un hijo del que el padre es UNICO dueno.
 * @tparam T Clase del hijo tal como la declara el padre (`Expr`, `Stmt`,
 *           `BlockStmt`, `TypeNode`, `ParamDecl`): lo que se ponga en la
 *           ranura tiene que ser de esa clase.
 */
template <class T> class ChildSlot {
  public:
    /**
     * @brief Ranura sobre el puntero que guarda el padre.
     * @param holder El miembro (o elemento de lista) del padre.
     */
    explicit ChildSlot(std::unique_ptr<T> &holder) : holder_(&holder) {}

    /**
     * @brief El hijo actual.
     * @return El hijo, o nulo si la ranura esta vacia.
     */
    T *get() const { return holder_->get(); }

    /**
     * @brief El hijo actual, que tiene que existir.
     * @return Referencia al hijo.
     */
    T &operator*() const { return **holder_; }

    /**
     * @brief Acceso a los miembros del hijo actual.
     * @return El hijo.
     */
    T *operator->() const { return holder_->get(); }

    /**
     * @brief Pone @p node en la ranura; el hijo anterior se destruye.
     * @param node Nuevo hijo (puede ser de una clase derivada de @p T).
     */
    void replace(std::unique_ptr<T> node) { *holder_ = std::move(node); }

    /**
     * @brief Se lleva el hijo y deja la ranura vacia, para envolverlo y
     *        devolverlo con @ref replace.
     * @return El hijo que habia.
     */
    std::unique_ptr<T> take() { return std::move(*holder_); }

  private:
    std::unique_ptr<T> *holder_; ///< donde cuelga el hijo en su padre
};

/**
 * @class SharedChildSlot
 * @brief Ranura de un hijo COMPARTIDO entre copias (los argumentos de un
 *        `ConceptRef`).
 *
 * No hay @c take: un hijo compartido no se le puede quitar a los demas duenos
 * para envolverlo.  Quien necesite cambiarlo pone uno nuevo.
 *
 * @tparam T Clase del hijo (`TypeNode`).
 */
template <class T> class SharedChildSlot {
  public:
    /**
     * @brief Ranura sobre el puntero compartido que guarda el padre.
     * @param holder El miembro (o elemento de lista) del padre.
     */
    explicit SharedChildSlot(std::shared_ptr<T> &holder) : holder_(&holder) {}

    /**
     * @brief El hijo actual.
     * @return El hijo, o nulo si la ranura esta vacia.
     */
    T *get() const { return holder_->get(); }

    /**
     * @brief El hijo actual, que tiene que existir.
     * @return Referencia al hijo.
     */
    T &operator*() const { return **holder_; }

    /**
     * @brief Acceso a los miembros del hijo actual.
     * @return El hijo.
     */
    T *operator->() const { return holder_->get(); }

    /**
     * @brief Pone @p node en ESTA ranura; las demas copias conservan el suyo.
     * @param node Nuevo hijo.
     */
    void replace(std::unique_ptr<T> node) {
        *holder_ = std::shared_ptr<T>(std::move(node));
    }

  private:
    std::shared_ptr<T> *holder_; ///< donde cuelga el hijo en su padre
};

} // namespace vx::ast

#endif // VX_AST_CHILD_SLOT_H
