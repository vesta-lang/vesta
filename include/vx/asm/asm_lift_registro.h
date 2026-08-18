/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vx/asm/asm_lift_registro.h
 * @brief Que paso con cada bloque `asm` al bajarlo: se elevo a IR, se quedo
 * como micro asm, o no se elevo.
 *
 * Hace falta porque ESO NO SE PUEDE RECONSTRUIR DESPUES.  Una instruccion de
 * asm elevada a operaciones tipadas se convierte en una suma o un
 * almacenamiento como cualquier otro: nada la marca, y preguntarle al IR cuanto
 * asm se elevo es preguntarle por algo que ya no existe alli.  Lo sabe quien lo
 * eleva, en el momento de elevarlo, y si no lo dice se pierde.
 *
 * Se anota al bajar y lo lee el dominio del asm del ASA.  Un dato del que no se
 * puede hablar hace que un volcado diga "no hay" donde deberia decir "hubo y no
 * consta", que son cosas distintas.
 */
#ifndef VX_ASM_ASM_LIFT_REGISTRO_H
#define VX_ASM_ASM_LIFT_REGISTRO_H

#include <cstdint>
#include <string>
#include <vector>

namespace vx {

/// Donde acabo un bloque `asm`.
enum class DestinoAsm : uint8_t {
    ElevadoAIr, ///< a operaciones tipadas: el optimizador y el interprete lo
                ///< ven.
    MicroAsm,   ///< sigue siendo asm, pero dentro del IR y con sus efectos.
    SinElevar,  ///< bloque opaco: el IR solo sabe que esta ahi.
};

const char *nombre_destino_asm(DestinoAsm d);

/// Un bloque `asm` y lo que fue de el.
struct BloqueAsmBajado {
    std::string funcion;
    uint32_t linea = 0;
    uint32_t instrucciones = 0; ///< las del fuente, sin contar etiquetas.
    DestinoAsm destino = DestinoAsm::SinElevar;
};

/**
 * @brief Anota que paso con un bloque.  Lo llama el lowering en los tres sitios
 *        donde se decide, que son los unicos que lo saben.
 *
 * @param funcion       Funcion que lo contiene.
 * @param linea         Linea del fuente donde empieza.
 * @param cuerpo        Texto del bloque (de ahi se cuentan las instrucciones).
 * @param destino       Que fue de el.
 */
void anotar_bloque_asm(const std::string &funcion, uint32_t linea,
                       const std::string &cuerpo, DestinoAsm destino);

/// Lo anotado hasta ahora, en orden de bajada.
std::vector<BloqueAsmBajado> bloques_asm_bajados();

/// Vacia el registro.  Un proceso que compile muchos modulos lo llama entre
/// uno y otro; una compilacion suelta no necesita hacerlo.
void olvidar_bloques_asm();

} // namespace vx

#endif // VX_ASM_ASM_LIFT_REGISTRO_H
