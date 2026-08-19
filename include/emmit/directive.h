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
 * @file emmit/directive.h
 * @brief La anotacion del `.vel` como TIPO, y como se escribe su argumento.
 *
 * Sale de @c emmit/directive_list.h, que es la lista unica.  Nadie escribe
 * este enum a mano: seria otra copia de la misma lista y se separaria de las
 * demas igual que ellas ya se separaron entre si.
 *
 * Para que sirve: que el emisor diga QUE anotacion escribe en vez de teclear
 * su nombre y sus comillas.  Una anotacion que no existe deja de compilar, y
 * la forma del argumento -- comillas, identificador o numero -- la pone la
 * lista, no cada sitio de emision.
 *
 * ## Nada se busca
 *
 * La anotacion ES un indice, asi que su nombre y su forma salen de tablas
 * planas.  Todo @c constexpr: cuando se conoce al compilar, la pregunta no
 * llega a ejecutarse.
 */
#ifndef EMMIT_DIRECTIVE_H
#define EMMIT_DIRECTIVE_H

#include <cstdint>
#include <cstring>

namespace emmit {

/**
 * @brief Como se escribe el argumento de una anotacion.
 *
 * Ver la cabecera de @c directive_list.h: esto es lo unico que depende SOLO de
 * la anotacion.  Donde puede aparecer, no -- la misma anotacion sale suelta,
 * dentro de un bloque o como operando de una instruccion.
 */
enum class ArgForm : uint8_t {
    Block,   ///< Sin argumento; abre llaves.  `@Section { ... }`
    Quoted,  ///< Entre comillas.  `@Format("elf")`
    Bare,    ///< Identificador pelado.  `@Module(mi_modulo)`
    Numeric, ///< Numero, escrito en hexadecimal.  `@Align(0x1000)`
};

/// Las anotaciones del `.vel`.  El orden es el de @c directive_list.h.
enum class Directive : uint8_t {
#define VX_DIRECTIVE(id, text, form) id,
#include "emmit/directive_list.h"
#undef VX_DIRECTIVE
    kCount ///< Centinela: cuantas hay.  No es una anotacion.
};

/// Cuantas anotaciones hay.
inline constexpr uint8_t directive_count() {
    return static_cast<uint8_t>(Directive::kCount);
}

/// El nombre de una anotacion, SIN la arroba.  Indexado, no buscado.
inline constexpr const char *text_of(Directive d) {
    constexpr const char *kText[] = {
#define VX_DIRECTIVE(id, text, form) text,
#include "emmit/directive_list.h"
#undef VX_DIRECTIVE
        "?"};
    return kText[static_cast<uint8_t>(d)];
}

/// Como se escribe su argumento.
inline constexpr ArgForm form_of(Directive d) {
    constexpr ArgForm kForm[] = {
#define VX_DIRECTIVE(id, text, form) ArgForm::form,
#include "emmit/directive_list.h"
#undef VX_DIRECTIVE
        ArgForm::Block};
    return kForm[static_cast<uint8_t>(d)];
}

/// Si @p d es una anotacion de verdad y no el centinela del final.
inline constexpr bool is_valid(Directive d) {
    return static_cast<uint8_t>(d) < directive_count();
}

/**
 * @brief La anotacion que se llama @p text, o @c kCount si no hay ninguna.
 *
 * Recorrido lineal a proposito: son diecisiete y la pregunta se hace al leer
 * un fichero, no en un bucle caliente.  Montar un indice ordenado para esto
 * costaria mas de lo que ahorra.
 *
 * @param text Nombre SIN la arroba.
 */
inline Directive directive_from_text(const char *text) {
    if (text == nullptr) return Directive::kCount;
    for (uint8_t i = 0; i < directive_count(); ++i) {
        const Directive d = static_cast<Directive>(i);
        if (std::strcmp(text_of(d), text) == 0) return d;
    }
    return Directive::kCount;
}

} // namespace emmit

#endif // EMMIT_DIRECTIVE_H
