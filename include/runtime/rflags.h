/*
 * VestaVM - Máquina Virtual Distribuida
 * 
 * Copyright © 2026 David López.T (DesmonHak) (Castilla y León, ES)
 * Licencia VMProject
 * 
 * USO LIBRE NO COMERCIAL con atribución obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 * 
 * Descargo: Autor no responsable por modificaciones.
 */

#ifndef RFLAGS_H
#define RFLAGS_H

#include <cstdint>

/**
 * Registros de bandera para la VM.
 */
typedef struct RFlags_t {
    union {
        struct Bits {
            uint8_t SF: 1; // Negative flag/Bandera de signo
            uint8_t ZF: 1; // Zero flag
            uint8_t CF: 1; // Carry flag
            uint8_t OF: 1; // Overflow flag
            uint8_t DM: 1; // Modo distribuido
        } bits;

        uint8_t raw;
    };
} RFlags_t;

#define COND_EQ(flags) ((flags).ZF == 1)
// Igual (==): el resultado fue cero -> lhs == rhs

#define COND_NE(flags) ((flags).ZF == 0)
// No igual (!=): el resultado NO fue cero -> lhs != rhs

#define COND_CS(flags) ((flags).CF == 1)
// Carry Set: hubo acarreo (en suma) o NO hubo prestamo (en resta) -> para comparaciones unsigned: lhs ≥ rhs

#define COND_CC(flags) ((flags).CF == 0)
// Carry Clear: no hubo acarreo -> para comparaciones unsigned: lhs < rhs

#define COND_MI(flags) ((flags).SF == 1)
// Minus: el resultado es negativo -> lhs < rhs (signed)

#define COND_PL(flags) ((flags).SF == 0)
// Plus: el resultado es positivo o cero -> lhs ≥ 0

#define COND_VS(flags) ((flags).OF == 1)
// Overflow Set: ocurrio desbordamiento aritmetico con signo

#define COND_VC(flags) ((flags).OF == 0)
// Overflow Clear: no hubo desbordamiento con signo

#define COND_HI(flags) ((flags).CF == 1 && (flags).ZF == 0)
// Unsigned Higher: hubo carry y no es cero -> lhs > rhs (unsigned)

#define COND_LS(flags) ((flags).CF == 0 || (flags).ZF == 1)
// Unsigned Lower or Same: no hubo carry o el resultado es cero -> lhs ≤ rhs (unsigned)

#define COND_GE(flags) ((flags).SF == (flags).OF)
// Signed Greater or Equal: signo y overflow son iguales -> lhs ≥ rhs (signed)

#define COND_LT(flags) ((flags).SF != (flags).OF)
// Signed Less Than: signo y overflow son distintos -> lhs < rhs (signed)

#define COND_GT(flags) ((flags).ZF == 0 && (flags).SF == (flags).OF))
// Signed Greater Than: no es cero y signo = overflow -> lhs > rhs (signed)

#define COND_LE(flags) ((flags).ZF == 1 || (flags).SF != (flags).OF))
// Signed Less or Equal: es cero o signo ≠ overflow -> lhs ≤ rhs (signed)


#endif //RFLAGS_H
