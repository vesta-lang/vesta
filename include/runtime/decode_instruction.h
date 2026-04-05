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
#include "decode_table.h"
#include "runtime.h"
#include "emmit/emmit_decl.h"

namespace runtime {
    struct DecodedInstr;
    class VM;

    /**
     * Contiene la informacion minima para descoficiar las instrucciones
     */
    typedef struct InstrFormat {
        /**
        * Modo de direccionamiento de la instruccion. Usamos COUNT por defecto para indicar que la metadata
         * de la instruccion no esta bien formada. Ya que COUNT no es un modo valido por lo que si se
         * encuentro al descodificar la instruccion, posiblemente fue por que la tabla de metainformacion
         * decode_table_primary y decode_table_extended no estaban bien formadas o falto definir alguna
         * entrada de forma correcta.
         */
        Assembly::Bytecode::AddressingMode mode = Assembly::Bytecode::AddressingMode::COUNT;

        /**
         * Tamaño de la instruccion
         */
        Assembly::Bytecode::InstrSizeMode size = Assembly::Bytecode::InstrSizeMode::FIXED_1;

        /**
         * Puntero a la función que implementa la semántica de la instrucción.
         *
         * Esta función se invoca durante la fase de ejecución (EXECUTE) y es la
         * responsable de aplicar los efectos de la instrucción sobre el estado de la
         * máquina virtual: modificar registros, memoria, banderas, el contador de
         * programa (PC) o cualquier otro componente interno.
         *
         * La función recibe un puntero a la VM para acceder y modificar su estado.
         * No debe realizar tareas de decodificación ni avanzar el PC; estas acciones
         * son responsabilidad de otras fases del pipeline. Si la instrucción modifica
         * explícitamente el PC (por ejemplo, saltos, llamadas o retornos), debe
         * establecer el campo `did_jump` en el `DecodedInstr` para evitar que EXECUTE
         * avance automáticamente el PC.
         *
         * @param vm Puntero a la máquina virtual sobre la que se ejecuta la instrucción.
         */
        void (*exec)(VM *, const DecodedInstr &) = nullptr;

        /**
         * Metodo que permite descodificar una instruccion.
         */
        void (*decode)(VM *, DecodedInstr &) = nullptr;
    } InstrFormat;


    /**
     * Se usa para descodificar instrucciones del tipo:
     *      reg1, reg2 o
     *      reg2, reg1
     */
     void decode_instr_reg(VM *vm, DecodedInstr &instr);
}
#endif //DECODE_INSTRUCTION_H
