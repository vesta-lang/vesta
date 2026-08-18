/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/rbank/lazy_fact.h
 * @brief LazyFact<T>: celda de hecho computado UNA VEZ, cacheado y
 * demand-driven.
 *
 * Encapsula el patron "compute-si-falta + cache" del query system del snapshot,
 * de modo que el @c mutable y el estado @c ready viven en UN SOLO SITIO (aqui)
 * en vez de estar dispersos por cada campo del snapshot.  Eso permite anadir el
 * dia de manana METRICAS (cuantas veces se pidio), INVALIDACION (@c reset) o
 * SINCRONIZACION (mutex/once) sin tocar cada Fact ni cada consumidor.
 *
 * Arquitectura (una celda por Fact; el snapshot es N celdas):
 *
 *      FunctionSnapshot
 *          LazyFact<LivenessResult>  live
 *          LazyFact<LoopFacts>       loops
 *          LazyFact<ProfileFacts>    profile
 *          LazyFact<...>             ...
 *
 *      fact.get([&]{ return compute(...); })
 *              |
 *        ready? --si--> value  (cache hit)
 *              |
 *              no --> produce() --> value --> ready=true
 *
 * El estado (@c ready_, @c value_) es @c mutable: @c get es @c const (una
 * consulta no cambia el conocimiento observable, solo lo materializa).  La
 * celda se guarda POR VALOR (no @c mutable) en el snapshot -> la mutabilidad
 * queda ENCAPSULADA.  No es thread-safe por ahora (compile por-funcion
 * single-thread); la sincronizacion se anadiria AQUI sin cambiar a los
 * consumidores.
 *
 * DIRECCION FUTURA (Query Engine): el siguiente paso natural es @c LazyFact<T,
 * Producer> o un @c Query<T> donde la dependencia NO se resuelva a mano, sino
 * que el QueryEngine conozca el productor REGISTRADO para cada T (@c
 * loops.get(*this)
 * -> el engine sabe como construir @c LoopFacts y de que depende).  Entonces el
 * compilador entero deja de tener funciones especiales: TODO son consultas
 * (@c snapshot.query<LoopFacts>()).  @c LazyFact ya aisla el ALMACENAMIENTO y
 * acerca mucho esa posibilidad.
 */

#ifndef VESTA_CODEGEN_RBANK_LAZY_FACT_H
#define VESTA_CODEGEN_RBANK_LAZY_FACT_H

#include <utility>

namespace codegen {
namespace rbank {

/**
 * @class LazyFact
 * @brief Celda demand-driven de un hecho @c T (compute-once + cache).
 */
template <typename T> class LazyFact {
  public:
    /**
     * @brief Devuelve el hecho, computandolo con @p produce la primera vez.
     * @param produce  functor sin args que devuelve @c T (se llama a lo sumo 1
     * vez).
     */
    template <typename Producer> const T &get(Producer &&produce) const {
        if (!ready_) {
            value_ = std::forward<Producer>(produce)();
            ready_ = true;
        }
        return value_;
    }

    /** @brief True si el hecho ya esta materializado. */
    bool ready() const noexcept { return ready_; }

    /**
     * @brief Valor cacheado SIN forzar computo, o @c nullptr si aun no esta
     *        materializado.  Devolver puntero (no referencia a un default)
     *        EVITA el error sutil de confundir "realmente cero" con "nunca
     *        calculado".
     */
    const T *peek() const noexcept { return ready_ ? &value_ : nullptr; }

    /**
     * @brief Referencia MUTABLE al valor (marca @c ready).  Nombre @c unsafe_
     *        deliberado: rompe la inmutabilidad del conocimiento; solo el
     *        constructor/tests deberian usarlo.  Deja claro QUIEN puede
     * modificar.
     */
    T &unsafe_mutable_ref() noexcept {
        ready_ = true;
        return value_;
    }

    /** @brief Fija el valor directamente (deserializacion / warm-load). */
    void set(T v) {
        value_ = std::move(v);
        ready_ = true;
    }

    /** @brief Construye el valor IN-PLACE con @p args (marca @c ready). */
    template <typename... Args> T &emplace(Args &&...args) {
        value_ = T(std::forward<Args>(args)...);
        ready_ = true;
        return value_;
    }

    /** @brief Invalida el hecho (futuro: recomputo tras cambiar el IR). */
    void reset() noexcept { ready_ = false; }

  private:
    mutable bool ready_ = false;
    mutable T value_{};
};

} // namespace rbank
} // namespace codegen

#endif // VESTA_CODEGEN_RBANK_LAZY_FACT_H
