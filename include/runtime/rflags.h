/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**                                                                            \
 * @file rflags.h                                                              \
 * @brief Registro de flags de la maquina virtual de VestaVM.                  \
 *                                                                             \
 * Define @c RFlags: union que expone los flags SF, ZF, CF, OF, DM             \
 * tanto como bits individuales (a traves de un bitfield) como valor           \
 * completo de 64 bits.                                                        \
 */                                                                            \
#ifndef RFLAGS_H
#define RFLAGS_H

#include <cstdint>

/**
 * @brief Registro de banderas de la maquina virtual.
 *
 * Agrupa en una union el acceso bit a bit (campo bits) y el acceso
 * completo como entero de 64 bits (campo raw).  Los cinco bits
 * definidos son:
 *   - SF (bit 0): bandera de signo; 1 si el resultado es negativo.
 *   - ZF (bit 1): bandera de cero; 1 si el resultado es cero.
 *   - CF (bit 2): bandera de acarreo; 1 si hubo carry/borrow.
 *   - OF (bit 3): bandera de desbordamiento con signo.
 *   - DM (bit 4): bandera de modo distribuido.
 */
typedef struct RFlags_t {
    union {
        struct {
            uint8_t SF : 1; ///< Bandera de signo: 1 si el resultado es negativo
            uint8_t ZF : 1; ///< Bandera de cero: 1 si el resultado es cero
            uint8_t CF : 1; ///< Bandera de acarreo: 1 si hubo carry o borrow
            uint8_t OF : 1; ///< Bandera de desbordamiento: 1 si hubo overflow
                            ///< con signo
            uint8_t DM : 1; ///< Bandera de modo distribuido: 1 activa semantica
                            ///< distribuida
        } bits;             ///< Acceso individual a cada bandera

        uint64_t raw; ///< Acceso directo al valor entero de 64 bits de todas
                      ///< las banderas
    };
} RFlags_t;

/**
 * @defgroup COND_macros Macros de condicion sobre RFlags_t
 * @brief Evaluan combinaciones de banderas para implementar ramas
 * condicionales.
 *
 * Cada macro recibe el campo bits del RFlags_t y devuelve una expresion
 * booleana que refleja la condicion aritmetica correspondiente.
 * @{
 */

/** @brief Igual (==): ZF=1 -> el resultado fue cero -> lhs == rhs */
#define COND_EQ(flags) ((flags).ZF == 1)

/** @brief No igual (!=): ZF=0 -> el resultado no fue cero -> lhs != rhs */
#define COND_NE(flags) ((flags).ZF == 0)

/** @brief Carry set: CF=1 -> hubo acarreo -> lhs >= rhs (unsigned) */
#define COND_CS(flags) ((flags).CF == 1)

/** @brief Carry clear: CF=0 -> sin acarreo -> lhs < rhs (unsigned) */
#define COND_CC(flags) ((flags).CF == 0)

/** @brief Minus: SF=1 -> resultado negativo -> lhs < rhs (signed) */
#define COND_MI(flags) ((flags).SF == 1)

/** @brief Plus: SF=0 -> resultado no negativo -> lhs >= 0 */
#define COND_PL(flags) ((flags).SF == 0)

/** @brief Overflow set: OF=1 -> desbordamiento aritmetico con signo */
#define COND_VS(flags) ((flags).OF == 1)

/** @brief Overflow clear: OF=0 -> sin desbordamiento con signo */
#define COND_VC(flags) ((flags).OF == 0)

/** @brief Unsigned higher: CF=1 y ZF=0 -> lhs > rhs (unsigned) */
#define COND_HI(flags) ((flags).CF == 1 && (flags).ZF == 0)

/** @brief Unsigned lower or same: CF=0 o ZF=1 -> lhs <= rhs (unsigned) */
#define COND_LS(flags) ((flags).CF == 0 || (flags).ZF == 1)

/** @brief Signed greater or equal: SF==OF -> lhs >= rhs (signed) */
#define COND_GE(flags) ((flags).SF == (flags).OF)

/** @brief Signed less than: SF!=OF -> lhs < rhs (signed) */
#define COND_LT(flags) ((flags).SF != (flags).OF)

/** @brief Signed greater than: ZF=0 y SF==OF -> lhs > rhs (signed) */
#define COND_GT(flags) ((flags).ZF == 0 && (flags).SF == (flags).OF)

/** @brief Signed less or equal: ZF=1 o SF!=OF -> lhs <= rhs (signed) */
#define COND_LE(flags) ((flags).ZF == 1 || (flags).SF != (flags).OF)

/** @} */ // fin del grupo COND_macros

#endif // RFLAGS_H
