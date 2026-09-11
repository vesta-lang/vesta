/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file util/shared_mutex.h
 * @brief Cerrojo de lector/escritor que NO pasa por la emulacion de pthreads.
 *
 * POR QUE NO `std::shared_mutex`.  En MinGW se implementa sobre
 * `pthread_rwlock_t` de winpthreads, y esa pieza SE ROMPE cuando hay hilos que
 * nacen y mueren -- que es exactamente lo que hace el compilador, un lote de
 * hilos por nivel de modulos.  Medido con una sonda de sesenta lineas, sin nada
 * de Vesta dentro: el MISMO programa, cambiando solo el tipo del cerrojo,
 *
 *     std::shared_mutex   5 de 5 corridas mueren (violacion de segmento o
 *                         cuelgue con los 23 hilos parados y NINGUNO dentro)
 *     std::mutex          5 de 5 correctas
 *
 * En el compilador se veia igual: 8 escritores esperando el exclusivo, 8
 * esperando el de despues de la fabrica y 8 lectores, con la seccion critica
 * VACIA.  Un cerrojo correcto no puede dejar eso.
 *
 * QUE SE USA EN SU LUGAR.  En Windows, `SRWLOCK`: es el cerrojo de
 * lector/escritor del sistema, del tamano de un puntero, sin reservar memoria y
 * sin nada que destruir.  Fuera de Windows, `std::shared_mutex`, que ahi si se
 * apoya en un `pthread_rwlock_t` de verdad.
 *
 * INTERFAZ ESTaNDAR a proposito: ofrece `lock`/`try_lock`/`unlock` y
 * `lock_shared`/`try_lock_shared`/`unlock_shared`, que es lo que piden
 * `std::unique_lock`, `std::shared_lock` y `std::lock_guard`.  Asi cambiar de
 * cerrojo es cambiar UN tipo, no reescribir cada sitio que lo toma.
 *
 * NO ES REENTRANTE ni se puede ascender de lectura a escritura, igual que
 * `std::shared_mutex` y que `SRWLOCK`.  Pedir dos veces desde el mismo hilo
 * cuelga.
 *
 * NO INCLUYE `windows.h`.  Esa cabecera define `VOID` como macro y rompe
 * cualquier `enum class` que use ese nombre -- ya obligo a aislar
 * `ThreadPool.h` en su propio `.cpp` --, asi que el estado se guarda como un
 * puntero opaco y la implementacion vive en el `.cpp`.
 */
#ifndef VESTA_UTIL_SHARED_MUTEX_H
#define VESTA_UTIL_SHARED_MUTEX_H

/* Fuera del `#if`: las guardas MEDIDAS de abajo son iguales en los dos sistemas
 * y las necesitan siempre.  Lo que depende del sistema es el estado del cerrojo,
 * no quien lo cronometra. */
#include "util/reloj.h" // el reloj del proyecto, no el de la biblioteca

#include <atomic>
#include <cstdint>

#if !defined(_WIN32)
#include <shared_mutex>
#endif

namespace util {

/**
 * @brief Cerrojo de lector/escritor.
 *
 * Muchos lectores a la vez, o un escritor solo.  Se declara como cualquier
 * miembro; no hay que inicializarlo ni destruirlo a mano.
 */
class SharedMutex {
  public:
    SharedMutex() noexcept = default;
    SharedMutex(const SharedMutex &) = delete;
    SharedMutex &operator=(const SharedMutex &) = delete;

    /// Toma el cerrojo en EXCLUSIVA.  Espera a que no haya nadie dentro.
    void lock() noexcept;
    /// Intenta la exclusiva sin esperar.  @return false si no pudo.
    bool try_lock() noexcept;
    /// Suelta la exclusiva.
    void unlock() noexcept;

    /// Toma el cerrojo COMPARTIDO.  Otros lectores pueden entrar a la vez.
    void lock_shared() noexcept;
    /// Intenta el compartido sin esperar.  @return false si no pudo.
    bool try_lock_shared() noexcept;
    /// Suelta el compartido.
    void unlock_shared() noexcept;

  private:
#if defined(_WIN32)
    /* Es un `SRWLOCK`.  En la cabecera del sistema es una estructura con UN
     * puntero, y su valor inicial -- `SRWLOCK_INIT` -- es ese puntero a nulo,
     * asi que inicializarlo a cero aqui es exacto.  Se declara como bytes para
     * no arrastrar `windows.h`; el `.cpp` comprueba con `static_assert` que el
     * tamano y la alineacion coinciden.
     *
     * BYTES y no un `void *`: si fuera un `void *` habria ahi un objeto de ese
     * tipo, y leerlo como `SRWLOCK` seria comportamiento indefinido -- con
     * `-fstrict-aliasing` el compilador puede suponer que los dos accesos no se
     * pisan --.  Un array de `unsigned char` es almacenamiento en bruto, que es
     * lo que hace falta para que el sistema construya el suyo encima. */
    alignas(void *) unsigned char state_[sizeof(void *)] = {};
#else
    std::shared_mutex impl_;
#endif
};

/**
 * @brief Toma el cerrojo COMPARTIDO apuntando cuanto costo ENTRAR.
 *
 * Contar cuantas veces se toma un cerrojo dice si se usa; medir la ESPERA dice
 * si se pelea.  Son preguntas distintas: un millon de tomas con buen reparto no
 * cuesta nada, y las mismas con veinticuatro hilos encima pueden ser la mitad
 * del CPU.  Sin medir la entrada no se distingue una cosa de la otra.
 *
 * Medir es OPCIONAL: con @p ns nulo no se lee el reloj ni una vez, asi que el
 * camino normal no paga nada.  Quien mide lo decide; el cerrojo no sabe de
 * banderas.
 */
class TimedSharedLock {
  public:
    /**
     * @param m  El cerrojo.
     * @param ns Donde sumar los nanosegundos de espera, o nulo para no medir.
     */
    TimedSharedLock(SharedMutex &m, std::atomic<long long> *ns) : m_(m) {
        if (ns == nullptr) {
            m_.lock_shared();
            return;
        }
        const uint64_t t0 = reloj::ahora();
        m_.lock_shared();
        ns->fetch_add(static_cast<long long>(reloj::a_ns(reloj::ahora() - t0)),
                      std::memory_order_relaxed);
    }
    ~TimedSharedLock() { m_.unlock_shared(); }
    TimedSharedLock(const TimedSharedLock &) = delete;
    TimedSharedLock &operator=(const TimedSharedLock &) = delete;

  private:
    SharedMutex &m_;
};

/**
 * @brief Igual, en EXCLUSIVA, y con soltar/volver a tomar a mano.
 *
 * Las dos cosas hacen falta porque hay trabajo que NO se puede hacer con el
 * cerrojo puesto -- el caso que lo motiva es una fabrica de analisis que pide
 * otros analisis, y reentrar se autobloquearia --, asi que la secuencia real es
 * tomar, soltar, calcular y volver a tomar.  La SEGUNDA toma se mide igual que
 * la primera: es la mitad del coste en ese patron.
 */
class TimedUniqueLock {
  public:
    TimedUniqueLock(SharedMutex &m, std::atomic<long long> *ns)
        : m_(m), ns_(ns) {
        lock();
    }
    ~TimedUniqueLock() {
        if (held_) m_.unlock();
    }
    TimedUniqueLock(const TimedUniqueLock &) = delete;
    TimedUniqueLock &operator=(const TimedUniqueLock &) = delete;

    void lock() {
        if (ns_ == nullptr) {
            m_.lock();
            held_ = true;
            return;
        }
        const uint64_t t0 = reloj::ahora();
        m_.lock();
        held_ = true;
        ns_->fetch_add(static_cast<long long>(reloj::a_ns(reloj::ahora() - t0)),
                       std::memory_order_relaxed);
    }
    void unlock() {
        m_.unlock();
        held_ = false;
    }

  private:
    SharedMutex &m_;
    std::atomic<long long> *ns_;
    bool held_ = false;
};

} // namespace util

#endif // VESTA_UTIL_SHARED_MUTEX_H
