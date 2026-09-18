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
 * @file util/name_pool.cpp
 * @copydoc util/name_pool.h
 */

#include "util/name_pool.h"

#include "util/thread_owned.h" // una ranura por hilo, sin `thread_local`

#include <mutex>
#include <unordered_set>

namespace util {

namespace {

/**
 * @brief El pozo.
 *
 * Conjunto y no vector porque lo que se pide es "dame EL puntero de este
 * nombre", y por nodos porque los elementos no se mueven al crecer: un puntero
 * repartido hace diez ficheros tiene que seguir valiendo.  Un vector ordenado
 * seria mas amable con la cache al buscar, pero al crecer REUBICA, y entonces
 * los punteros ya repartidos apuntarian a memoria liberada -- que no daria un
 * error, daria otro nombre.
 */
std::unordered_set<std::string> &pool() {
    static std::unordered_set<std::string> p;
    return p;
}

std::mutex &pool_mutex() {
    static std::mutex m;
    return m;
}

} // namespace

const std::string *intern_name(const std::string &name) {
    /* Lo ULTIMO que pidio este hilo, para no tomar el cerrojo global cuando la
     * respuesta es la misma que la vez anterior -- que es lo normal --.
     *
     * El bajado pide el nombre del fichero una vez por CONVERSION (desde
     * `SourceLoc::set_file`, que llama `cast_if_needed`), o sea decenas de
     * miles de veces seguidas, y siempre el mismo: un hilo baja un fichero cada
     * vez. Medido con VTune sobre 441.000 lineas, ese mutex era el **10,8 % del
     * CPU del compilador** y el **72 % de la fase de bajado**, con todos los
     * hilos haciendo cola en el.
     *
     * Comparar la cadena es mas barato que tomar un mutex, y ademas casi
     * siempre corta en el primer paso (`std::string::operator==` mira el tamano
     * antes que los bytes).
     *
     * El puntero se puede guardar porque el pozo es un `unordered_set`: sus
     * nodos no se mueven al crecer, asi que lo internado vive lo que el
     * proceso.
     *
     * Por RANURA de hilo y NUNCA `thread_local`: en MinGW la TLS es emulada y
     * cuelga con hilos que nacen y mueren, que es justo lo que hace el reparto
     * del compilador. */
    struct Ultimo {
        const std::string *ptr = nullptr;
    };
    static util::ThreadOwned<Ultimo> ultimo;
    Ultimo &u = ultimo.get();
    if (u.ptr != nullptr && *u.ptr == name) return u.ptr;

    std::lock_guard<std::mutex> guard(pool_mutex());
    const std::string *p = &*pool().insert(name).first;
    u.ptr = p;
    return p;
}

// Definido en la cabecera (variable en linea): ver `empty_name`.

} // namespace util
