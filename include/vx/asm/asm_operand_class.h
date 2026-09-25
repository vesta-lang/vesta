/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file asm_operand_class.h
 * @brief Con que CLASE se declaro cada operando de un bloque de asm.
 *
 * Viajaba como `std::vector<std::pair<std::string, std::string>>`: un par sin
 * nombres, en el que nada decia cual de las dos cadenas era el marcador y cual
 * la clase, y que tres consumidores reconstruian por su cuenta copiando las dos
 * cadenas de cada ligadura.  Ahora lo produce UNA vez quien resuelve las
 * ligaduras (@c analysis::compute_asm_bindings) y las cadenas van internadas:
 * los marcadores (`$0`, `rax`) y las clases (`reg`, `xmm`) son un punado, se
 * repiten en cada bloque y no hace falta copiarlas nunca.
 */

#ifndef VX_ASM_OPERAND_CLASS_H
#define VX_ASM_OPERAND_CLASS_H

#include <string>
#include <vector>

#include "util/name_pool.h" // los dos nombres, internados

namespace vx {

/**
 * @struct AsmOperandClass
 * @brief Un operando de asm tal como lo nombra el cuerpo, y como se declaro.
 */
struct AsmOperandClass {
    /// Como lo nombra el cuerpo: `$N` si lo elige el compilador, el registro
    /// canonico (`rax`) si es fijo.  Internado.
    const std::string *marker = nullptr;
    /// Clase declarada (`reg`, `xmm`, `rax`...): dice cuanto MIDE el operando.
    /// Internada.
    const std::string *declared_class = nullptr;
};

/// Las clases de los operandos de una funcion; puede ir vacia.
using AsmOperandClasses = std::vector<AsmOperandClass>;

/**
 * @brief Una entrada con sus dos nombres ya internados.
 *
 * Internar toma un cerrojo, asi que se llama al PRODUCIR las clases -- una vez
 * por ligadura --, nunca al consultarlas.
 *
 * @param marker         Nombre en el cuerpo (`$N` o registro canonico).
 * @param declared_class Clase declarada.
 * @return La entrada.
 */
inline AsmOperandClass asm_operand_class(const std::string &marker,
                                         const std::string &declared_class) {
    return AsmOperandClass{util::intern_name(marker),
                           util::intern_name(declared_class)};
}

/**
 * @brief La clase declarada del operando que el cuerpo llama @p marker.
 * @param classes Clases de la funcion.
 * @param marker  Nombre tal como aparece en el cuerpo.
 * @return La clase, o @c nullptr si ninguna ligadura responde a ese nombre.
 */
inline const std::string *asm_declared_class_of(const AsmOperandClasses &classes,
                                                const std::string &marker) {
    for (const AsmOperandClass &c : classes)
        if (*c.marker == marker) return c.declared_class;
    return nullptr;
}

} // namespace vx

#endif // VX_ASM_OPERAND_CLASS_H
