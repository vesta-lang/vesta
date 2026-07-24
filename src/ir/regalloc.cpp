/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file regalloc.cpp
 * @brief Nombres de los registros de la VM.
 *
 * Aqui vivia el asignador del interprete (barrido lineal propio).  Se jubilo:
 * los tres modos usan @c codegen::rbank, y el interprete entra por
 * @c codegen::vm_allocate.  Dos asignadores para el mismo problema obligan a
 * arreglar cada fallo dos veces -- y el de aqui guardaba conocimiento que no
 * estaba dicho en ningun sitio (que R0 hay que esquivar), escondido en el orden
 * de su pool de libres.  Ver @c codegen/vm_isa_facts.h.
 */

#include "ir/regalloc.h"

namespace ir {

static const char *REG_NAMES[16] = {"r0",  "r1",  "r2",  "r3", "r4",  "r5",
                                    "r6",  "r7",  "r8",  "r9", "r10", "r11",
                                    "r12", "r13", "r14", "r15"};

const char *reg_name(int reg) {
    if (reg >= 0 && reg < 16) return REG_NAMES[reg];
    return "r?";
}

} // namespace ir
