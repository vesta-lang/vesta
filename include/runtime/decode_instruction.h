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

// para mediciones de tiempo ultraprecisas.
#ifdef VM_PROFILE
    #define PROFILE_START auto __t0 = clock::now();
    #define PROFILE_END(label) \
    do { \
    auto __t1 = clock::now(); \
    std::cout << "[" << label << "] " \
    << std::chrono::duration_cast<std::chrono::nanoseconds>(__t1 - __t0).count() \
    << " ns\n"; \
    } while(0)
#else
#define PROFILE_START
#define PROFILE_END(label)
#endif

/**
 * Permite indicar si se quiere realizar comprobaciones de seguridad o no.
 * En caso de VM_DEBUG_CHECKS == 1 se añadira comprobaciones en la compilacion
 * para hacer validaciones durante el desarollo de la VM.
 *
 * Se recomienda desactivar la flag poniendole el valor 0 para tener el maximo
 * rendimiento en las funciones que son Hot-path como la fase de descodificacion
 * y ejecuccion.
 */

#ifdef VM_DEBUG_CHECKS
#pragma message("VM_DEBUG_CHECKS fue activada")
#define VM_ASSERT(cond, msg, code) \
    do { \
    if (!(cond)) { \
    vesta::scout() << "[VM ASSERT] " << msg << "\n"; \
    code; \
    abort(); \
    } \
    } while (0)
#else
    #define VM_ASSERT(cond, msg) do {} while (0)
#endif

namespace runtime {
    struct DecodedInstr;
    class VM;

    /**
     * Contiene la informacion minima para descoficiar las instrucciones
     */
    typedef struct InstrFormat {
        /**
         * Nombre de la instruccion, HLT, MOV, ADD, ETC
         */
        const char *name;

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
    void decode_instr_two_op_reg(VM *vm, DecodedInstr &instr);

    /**
     * Se usa para descodificar instrucciones con un solo operando de tipo
     * registro, donde el opcode que indica el registro tiene tambien el
     * modo contenido como es el caso de INC y DEC:
     *
     * | Instrucción | opcode1 | byte (relleno o extensión o registro) | total bytes |
     * | :---------: | :-----: | :-----------------------------------: | :---------: |
     * |     INC     |   0x4   |             `0b00` `reg`              |      2      |
     *
     * | Instrucción | opcode1 | byte (relleno o extensión o registro) | total bytes |
     * | :---------: | :-----: | :-----------------------------------: | :---------: |
     * |     DEC     |   0x4   |             `0b01` `reg`              |      2      |
     *
     * En estos casos reg contiene 2 primeros bits para el "modo" (si el reg es de 1, 2, 4 u 8 bytes)
     * mientras que los otros 4 bits se usa para indicar el registro general a usar que puede ser de
     * r0 a r15.
     *
     * Estas instrucciones no deben usar 0x00 (extension de opcode), debe ser una instruccion
     * mono-opcode.
     *
     * @param vm maquina virtual al momento de descodificar la instruccion, requiere que los opcodes
     * hayan sido procesador y el apuntador de descodificacion contenga los metadatos necesarios
     * para esta instruccion.
     *
     * @param instr meta-datos de la instruccion, en caso de ser inc seran sus meta-datos, cada instruccion
     * tiene sus propios metadatos.
     */
    void decode_instr_one_op_reg(VM *vm, DecodedInstr &instr);

    /**
     * Se usa para descodificar instrucciones simples que no requieren de
     * ningun tipo de analisis profundo como son los NOP, HLT y instrucciones
     * similares que no toman parametros o no requieren descodificar los
     * parametros en los opcodes.
     * @param vm
     * @param instr
     */
    void decode_instr_simple(VM *vm, DecodedInstr &instr);
}
#endif //DECODE_INSTRUCTION_H
