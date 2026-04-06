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
#ifndef DECODE_TABLE_H
#define DECODE_TABLE_H
#include "decode_instruction.h"

namespace runtime {
    struct InstrFormat;

    extern InstrFormat decode_table_primary[0x100];
    extern InstrFormat decode_table_extended[0x100];
};


#endif //DECODE_TABLE_H
