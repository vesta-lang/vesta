/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/emmit/test_asm_loc.cpp
 * @brief Comprueba el empaquetado de la POSICION de un operando de un bloque asm.
 *
 * Son cuatro campos en dieciseis bits, y viajan en los argumentos de la llamada
 * -- no en una tabla en la pila.  Si el empaquetado y el desempaquetado no
 * coinciden, el runtime mueve el valor equivocado de banco o de registro, y eso
 * no falla: corrompe.  De ahi que se comprueben TODAS las combinaciones y no una
 * muestra.
 */

#include "vx/asm/asm_phys_reg.h"

#include <cstdio>

int main() {
    int fallos = 0;
    /* Todas: 16 registros x 32 ranuras x 2 bancos x 4 banderas = 4096 casos. */
    for (uint8_t r = 0; r < 16; ++r)
        for (uint8_t ph = 0; ph < 32; ++ph)
            for (uint8_t b = 0; b < 2; ++b)
                for (uint8_t f = 0; f < 4; ++f) {
                    const uint16_t p = vx::asm_pack_loc(r, ph, b, f);
                    if (vx::asm_loc_vm_reg(p) == r &&
                        vx::asm_loc_phys(p) == ph &&
                        vx::asm_loc_bank(p) == b &&
                        vx::asm_loc_flags(p) == f)
                        continue;
                    if (++fallos <= 3)
                        std::printf("  FALLA    r=%u ph=%u b=%u f=%u no vuelve\n",
                                    r, ph, b, f);
                }

    /* Y que cuatro posiciones caben en un entero sin pisarse: es lo que permite
     * que viajen como argumento en vez de por memoria. */
    uint64_t w = 0;
    for (int i = 0; i < (int)vx::kAsmLocsPerWord; ++i)
        w |= (uint64_t)vx::asm_pack_loc((uint8_t)i, (uint8_t)(i * 3),
                                        (uint8_t)(i & 1), (uint8_t)(i & 3))
             << (i * 16);
    for (int i = 0; i < (int)vx::kAsmLocsPerWord; ++i) {
        const uint16_t p = (uint16_t)((w >> (i * 16)) & 0xFFFFu);
        if (vx::asm_loc_vm_reg(p) == (uint8_t)i &&
            vx::asm_loc_phys(p) == (uint8_t)(i * 3) &&
            vx::asm_loc_bank(p) == (uint8_t)(i & 1) &&
            vx::asm_loc_flags(p) == (uint8_t)(i & 3))
            continue;
        std::printf("  FALLA    la posicion %d se pisa con las vecinas\n", i);
        ++fallos;
    }

    std::printf("[asm-loc] %s\n", fallos == 0 ? "TODO OK" : "CON FALLOS");
    return fallos == 0 ? 0 : 1;
}
