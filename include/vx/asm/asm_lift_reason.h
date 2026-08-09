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
 * Por eso el elevado no responde solo "no pude": dice EN QUE se atasco.  Sin
 * ese dato el aviso no se puede atender, y un aviso que no se puede atender solo
 * ensena a ignorar los avisos.  Con el, casi siempre es una sola instruccion y
 * reescribirla convierte el bloque entero en IR normal.
 */

#ifndef VESTA_VX_ASM_ASM_LIFT_REASON_H
#define VESTA_VX_ASM_ASM_LIFT_REASON_H

#include <string>

namespace vx {

/**
 * @brief La instruccion que impidio pasar el bloque a IR, y que de ella no se
 *        supo.
 *
 * Se rellena solo cuando el elevado falla.  @ref instruccion vacia significa que
 * el fallo no fue de una instruccion concreta (bloque vacio, por ejemplo).
 */
struct AsmMotivoOpaco {
    std::string instruccion; ///< texto de la instruccion, tal cual se escribio.
    std::string detalle;     ///< en una frase, que de ella no se supo pasar.

    /// @c true si consta la instruccion culpable.
    bool consta() const { return !instruccion.empty(); }

    /// Anota el motivo si hay donde; devuelve false para poder escribir
    /// `return anotar(...)` en el punto exacto en el que se abandona.
    static bool anotar(AsmMotivoOpaco *destino, const std::string &instruccion,
                       const char *detalle) {
        if (destino != nullptr && !destino->consta()) {
            destino->instruccion = instruccion;
            destino->detalle = detalle ? detalle : "";
        }
        return false;
    }
};

} // namespace vx

#endif // VESTA_VX_ASM_ASM_LIFT_REASON_H
