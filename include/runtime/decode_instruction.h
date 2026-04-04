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
#ifndef DECODE_INSTRUCTION_H
#define DECODE_INSTRUCTION_H
#include <cstdint>


namespace runtime {
    class VM;


    /**
     * Representa la informacion basica que se genera en la descodificacion
     * y que una instruccion necesita leer para ser ejecutada.
     */
    typedef struct DecodedInstr {
        uint8_t opcode[2]{};
        uint8_t size = 0;

        uint8_t rd = 0; // registro destino
        uint8_t rs1 = 0; // registro fuente 1
        uint8_t rs2 = 0; // registro fuente 2

        int32_t imm; // inmediato (si existe)

        // puntero a la instruccion real a ejecutar
        void (*exec)(runtime::VM *);
    } DecodedInstr;
}
#endif //DECODE_INSTRUCTION_H
