/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file scoped_assign.h
 * @brief Dar un valor a una variable mientras dura un ambito y devolverle el
 *        que tenia al salir.
 *
 * El patron "guardar, cambiar, restaurar" escrito a mano se rompe con el
 * primer `return` temprano que alguien anyada en medio: la variable se queda
 * con el valor de dentro y el error sale lejos, donde se lee.  Con esto la
 * restauracion la hace el destructor, en todos los caminos.
 */

#ifndef UTIL_SCOPED_ASSIGN_H
#define UTIL_SCOPED_ASSIGN_H

#include <utility>

namespace util {

/**
 * @brief Asigna @p value a @p slot al construirse y le devuelve su valor
 *        anterior al destruirse.
 * @tparam T Tipo de la variable.
 */
template <typename T> class ScopedAssign {
  public:
    /**
     * @brief Guarda el valor actual de @p slot y le pone @p value.
     * @param slot  La variable.
     * @param value El valor mientras dure el ambito.
     */
    ScopedAssign(T &slot, T value) : slot_(slot), saved_(std::move(slot)) {
        slot_ = std::move(value);
    }

    /** @brief Devuelve a la variable el valor que tenia. */
    ~ScopedAssign() { slot_ = std::move(saved_); }

    ScopedAssign(const ScopedAssign &) = delete;
    ScopedAssign &operator=(const ScopedAssign &) = delete;

  private:
    T &slot_;  ///< La variable.
    T saved_;  ///< Lo que tenia antes.
};

} // namespace util

#endif // UTIL_SCOPED_ASSIGN_H
