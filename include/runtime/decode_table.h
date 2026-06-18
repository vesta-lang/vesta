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

/**
 * @file decode_table.h
 * @brief Declaracion de las tablas de despacho de instrucciones del bytecode
 * VestaVM.
 *
 * Expone dos tablas de 256 entradas (InstrFormat[0x100]):
 *
 *   - decode_table_primary:  tabla de opcodes de un solo byte (0x01-0xFF).
 *     El byte 0x00 actua como prefijo de extension y redirige a la segunda
 * tabla.
 *
 *   - decode_table_extended: tabla de opcodes extendidos; se accede cuando el
 *     primer byte de la instruccion es 0x00, usando el segundo byte como
 * indice.
 *
 * Las definiciones de las entradas se encuentran en decode_table.cpp.
 */

#ifndef DECODE_TABLE_H
#define DECODE_TABLE_H

#include "decode_instruction.h"

namespace runtime {

struct InstrFormat; ///< Declaracion adelantada del formato de instruccion

/**
 * @brief Tabla de opcodes primarios (primer byte != 0x00).
 *
 * Indexada directamente por el primer byte de la instruccion.
 * La entrada [0x00] (EXT_OPCODE) no se ejecuta; solo indica que se debe
 * redirigir a decode_table_extended.
 */
extern InstrFormat decode_table_primary[0x100];

/**
 * @brief Tabla de opcodes extendidos (primer byte == 0x00).
 *
 * Indexada por el segundo byte de la instruccion cuando el primero es 0x00.
 * Contiene instrucciones ALU, MOV, SIB, MOVC, saltos relativos, OOP y GC.
 */
extern InstrFormat decode_table_extended[0x100];

} // namespace runtime

#endif // DECODE_TABLE_H
