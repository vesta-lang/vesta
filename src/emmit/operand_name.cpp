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
 * @file emmit/operand_name.cpp
 * @brief El pozo de nombres de los operandos del `.vel`.
 *
 * POR QUE EXISTE.  Un `Operand` puede llevar nombre -- una etiqueta o una
 * referencia a simbolo --, y guardarlo como `std::string` costaba 32 bytes en
 * CADA operando de CADA instruccion.  Medido sobre un proyecto de 144.000
 * lineas: de los millones de operandos que emite el compilador, solo unos
 * 24.000 llevan nombre.  Los demas pagaban treinta y dos bytes por una cadena
 * vacia, multiplicados por los cuatro operandos de cada instruccion.
 *
 * Con el nombre internado el operando guarda un puntero: ocho bytes en vez de
 * treinta y dos, y ademas se deja de pedir memoria por cada nombre repetido --
 * una etiqueta a la que saltan veinte instrucciones se aloja UNA vez.
 *
 * POR QUE NO EL POZO QUE YA HABIA.  `util::intern_name` dice en su cabecera que
 * toma cerrojo y que no se llame por nodo ni por token: esta pensado para
 * internar UNA vez por fichero.  Aqui se llama por operando con nombre, que son
 * decenas de miles por compilacion, asi que necesita ser su propio pozo -- y
 * mezclarlos acoplaria dos poblaciones que no tienen nada que ver ni en numero
 * ni en vida.
 *
 * EL CERROJO NO SE NOTA, y esto es lo que lo hace aceptable: se toma decenas de
 * miles de veces por compilacion, no millones.  La inmensa mayoria de los
 * operandos son registros e inmediatos y no pasan por aqui.
 *
 * NO SE VACIA, igual que el otro.  Los punteros tienen que seguir valiendo
 * mientras viva una instruccion que los apunte, y las instrucciones viven lo
 * que dura la emision entera.
 */

#include "emmit/operand.h"

#include <mutex>
#include <string>
#include <unordered_set>

namespace emmit {

namespace {

/* Fugados a proposito, con la misma razon que el resto de los almacenes de
 * este proyecto que se leen tarde: un estatico con destructor se destruye al
 * salir, y aqui hay punteros vivos hasta el final del proceso -- incluido lo
 * que escriba un informe desde la lista de salida.  Un almacen que se destruye
 * antes que sus lectores no falla: contesta basura. */
std::unordered_set<std::string> &pool() {
    static std::unordered_set<std::string> *const p =
        new std::unordered_set<std::string>();
    return *p;
}

std::mutex &lock() {
    static std::mutex *const m = new std::mutex();
    return *m;
}

} // namespace

const std::string *empty_name() {
    static const std::string *const v = new std::string();
    return v;
}

const std::string *intern_operand_name(const std::string &name) {
    if (name.empty()) return empty_name();
    std::lock_guard<std::mutex> lk(lock());
    /* `unordered_set` garantiza que la direccion de un elemento NO cambia al
     * crecer la tabla -- solo se recolocan los cubos, no los nodos --, que es
     * justo lo que hace utilizable el puntero que se devuelve.  Con un `vector`
     * habria que reservar de antemano o los punteros se quedarian colgando al
     * primer crecimiento. */
    return &*pool().insert(name).first;
}

} // namespace emmit
