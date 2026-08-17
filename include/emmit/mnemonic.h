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
 * @file emmit/mnemonic.h
 * @brief El mnemonico de una instruccion `.vel` como TIPO, no como cadena.
 *
 * Sale de @c emmit/instr_list.def, que es la lista unica.  Nadie escribe este
 * enum a mano: si se escribiera, seria una tercera copia de la misma lista y se
 * separaria de las otras dos igual que ellas ya se separaron entre si -- cinco
 * instrucciones habian quedado en la tabla del emisor sin que el parser las
 * conociera, o sea inalcanzables, y eso no dio ningun error.
 *
 * Para que sirve: que el emisor del IR pueda decir QUE instruccion emite en vez
 * de escribir su nombre en una cadena.  Un mnemonico que no existe deja de
 * compilar, en lugar de descubrirse al ensamblar -- que es el final de la
 * cadena, lejos de donde se escribio.
 */
#ifndef EMMIT_MNEMONIC_H
#define EMMIT_MNEMONIC_H

#include <cstdint>
#include <string>

namespace emmit {

/**
 * @enum Mnemonico
 * @brief Las instrucciones que el `.vel` admite.
 */
enum class Mnemonico : uint16_t {
#define VX_INSTR(id, texto) id,
#include "emmit/instr_list.h"
#undef VX_INSTR
    /// Cuantas hay.  Va al final a proposito: sirve para dimensionar tablas
    /// planas indexadas por mnemonico, que es lo que hace que la busqueda sea
    /// un indice y no un hash de cadena.
    kCuantas
};

/**
 * @brief El texto del mnemonico @p m (`"jmp.je"`, `"mov"`, ...).
 *
 * Tabla plana indexada por el enum: el mnemonico ES el indice, asi que no hay
 * busqueda.  Es lo contrario de lo que habia -- una cadena que se hasheaba en
 * cada consulta.
 */
inline const char *texto_de(Mnemonico m) {
    static const char *const kTextos[] = {
#define VX_INSTR(id, texto) texto,
#include "emmit/instr_list.h"
#undef VX_INSTR
    };
    const auto i = static_cast<uint16_t>(m);
    return i < static_cast<uint16_t>(Mnemonico::kCuantas) ? kTextos[i] : "";
}

/// Cuantas instrucciones hay, como numero.
inline constexpr uint16_t cuantos_mnemonicos() {
    return static_cast<uint16_t>(Mnemonico::kCuantas);
}

} // namespace emmit

#endif // EMMIT_MNEMONIC_H
