/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vx/asm/asm_lift_reason.h
 * @brief Por que un bloque `asm` no se pudo pasar a IR.
 *
 * Un bloque que se queda opaco es inseguro por definicion: deja de optimizarse
 * y de lo que hace solo se sabe lo que diga su tabla de instrucciones -- ni que
 * memoria toca con que extension, ni que forma tiene su control.  Todo lo que
 * venga despues da por bueno un analisis que no llego, y eso no se manifiesta
 * como un error sino como una respuesta tranquila y equivocada, mas tarde y en
 * otro sitio.
 *
 * OJO con lo que significa: hoy lo rellena el ULTIMO elevado de la cadena, que
 * no es siempre el que mas cerca estuvo.  El elevado general de x86 modela
 * bastante mas -- `lea`, por ejemplo, ya se pasa a aritmetica de puntero --
 * pero todavia no dice en cual de sus noventa y seis salidas abandono, asi que
 * lo que se cuenta es el motivo del ultimo intento.  Es cierto, pero puede no
 * ser la razon mas util; cuando el elevado general tambien hable, se prefiere
 * la suya.
 *
 * Por eso el elevado no responde solo "no pude": dice EN QUE se atasco.  Sin
 * ese dato el aviso no se puede atender, y un aviso que no se puede atender
 * solo ensena a ignorar los avisos.  Con el, casi siempre es una sola
 * instruccion y reescribirla convierte el bloque entero en IR normal.
 */

#ifndef VESTA_VX_ASM_ASM_LIFT_REASON_H
#define VESTA_VX_ASM_ASM_LIFT_REASON_H

#include <string>
#include <vector>

namespace vx {

/**
 * @brief La instruccion que impidio pasar el bloque a IR, y que de ella no se
 *        supo.
 *
 * Se rellena solo cuando el elevado falla.  @ref instruccion vacia significa
 * que el fallo no fue de una instruccion concreta (bloque vacio, por ejemplo).
 *
 * El motivo se guarda como ENTRADA DEL CATaLOGO (@ref id) con sus parametros,
 * no como una frase ya escrita: el aviso que lo envuelve se sirve en el idioma
 * del usuario, y una frase compuesta aqui saldria siempre en el mismo -- media
 * linea en un idioma dentro de una linea en otro.
 */
struct AsmMotivoOpaco {
    std::string instruccion; ///< texto de la instruccion, tal cual se escribio.
    std::string id;          ///< entrada del catalogo con el motivo.
    std::vector<std::string> args; ///< parametros de esa entrada.

    /// @c true si consta la instruccion culpable.
    bool consta() const { return !instruccion.empty(); }

    /// Anota el motivo si hay donde; devuelve false para poder escribir
    /// `return anotar(...)` en el punto exacto en el que se abandona.
    static bool anotar(AsmMotivoOpaco *destino, const std::string &instruccion,
                       const char *id, std::vector<std::string> args = {}) {
        if (destino != nullptr && !destino->consta()) {
            destino->instruccion = instruccion;
            destino->id = id != nullptr ? id : "";
            destino->args = std::move(args);
        }
        return false;
    }
};

} // namespace vx

#endif // VESTA_VX_ASM_ASM_LIFT_REASON_H
