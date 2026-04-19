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

#ifndef PROCESO_RUNTIME_H
#define PROCESO_RUNTIME_H

#include "scheduler.h"
#include "vm_registers.h"
#include "vm_state_event.h"

#include "runtime/pid.h"

#define reductions_remaining_default 8192

namespace runtime {
    struct InstrFormat;
    class Scheduler;

    /**
     * Representa la informacion basica que se genera en la descodificacion
     * y que una instruccion necesita leer para ser ejecutada.
     */
    typedef struct DecodedInstr {
        struct {
            /**
             * opcode1
             */
            uint8_t is_not_extended: 8;

            /**
             * opcode2
             */
            uint8_t opcode_index: 8;

            /**
             * Indica si la instruccion tiene o no signo
             */
            uint8_t _signed_instruct: 1;


            /**
             * Modo de la instruccion, o tamaño tambien llamado en algunos casos.
             */
            uint8_t mode: 2;

            /**
             * Indica si la instrucción ha modificado manualmente el contador de programa (PC).
             *
             * Cuando una instrucción de control de flujo (por ejemplo: saltos, llamadas,
             * retornos o saltos condicionales) cambia explícitamente el valor del PC,
             * debe establecer este campo a `true`. Esto evita que la fase de ejecución
             * (EXECUTE) avance automáticamente el PC al finalizar la instrucción.
             *
             * Si el valor es `false`, EXECUTE incrementará el PC en función del tamaño
             * de la instrucción (`decoded.size`). Si es `true`, se asume que la instrucción
             * ya ha actualizado el PC y no se realizará el incremento automático.
             *
             * Este mecanismo evita avanzar el PC dos veces
             * en instrucciones que alteran el flujo de ejecución.
             */
            bool did_jump: 1;

            /**
             * Indica si la instruccion requiere esperar un I/O o a alguna
             * accion desbloqueante.
             */
            bool blocking: 1;

            /**
             * Se usa para indicar que se hace uso de registros extendidos/especiales
             * como son rbp, rsp, rip, cur0, cur1, ...
             */
            bool reg_ext: 1;

            /**
             * Algunas instrucciones tiene direcciones de operacion:
             *      adds [reg1 + reg2 * 8], reg3
             *      adds reg3, [reg1 + reg2 * 8]
             *
             * Si la instruccion no tiene direccionalidad, este campo se usa en algunos
             * casos como metadato de seleccion de otras variantes de la misma familia de instrucciones.
             */
            uint8_t direction: 1;

            /**
             * Tamaño de la instruccion, el maximo es 15 bytes
             */
            uint8_t size_instr: 4;
        } flags_info = {
            0, 0, 0, 0,
            false, false, false, 0, 0
        };

        union {
            /**
             * Datos en crudo
             */
            struct {
                uint64_t raw1;
                uint64_t raw2;
            } raw_data;

            /**
             * Permite guardar datos para las instrucciones tipo
             * reg, reg
             */
            struct {
                uint8_t reg1; // indica cual es el registro 1 que se codifica
                uint8_t reg2; // indica cual es el registro 2 que se codifica
            } reg_data;

            /**
             * Permite guardar datos de instrucciones del tipo
             * reg, inmmed o
             * inmmed, reg
             */
            struct {
                uint64_t inmmed; // valor inmediato
                uint8_t  reg;    // registro destino
            } inmmed_data;

            /**
             * Permite guardar datos para las instrucciones de tipo memoria como
             * son
             * add [reg2 * 2 + reg1], reg3
             * A estas instrucciones no les afecta el campo modo mas que al registro
             * destino u operando que no se usa para acceder a memoria.
             *
             * adds [r0 * 0 + r1], r3b // en este caso se accede a la direccion r1 y
             * se obtiene un byte que se guarda en r3.
             */
            struct {
                uint8_t reg_base;  // base
                uint8_t reg_index; // indice
                uint8_t reg_final;
                uint8_t scale; // escalar
            } mem_data;
        } data_instruction = {
            static_cast<uint64_t>(0),
            static_cast<uint64_t>(0)
        };

        /**
         * referencia a la instruccion descodificada con sus meta-datos, la funcion a ejecutar,
         * el metodo de descodificacion y otros campos utiles.
         */
        InstrFormat *metadata = nullptr;

        /**
         * Direccion PC donde se encontro la instruccion
         */
        uint64_t pc = 0;
    } DecodedInstr;


    /**
     * Estados de error de los hilos
     */
    typedef enum state_err_thread {
        THREAD_NO_ERROR = 0,        /** Sin error */
        THREAD_UNKNOWN_ERROR,       /** Error no clasificado */
        THREAD_SEGMENTATION_FAULT,  /** Un hilo intento acceder a memoria a la cual no tiene permisos */
        THREAD_ILLEGAL_INSTRUCTION, /** Instruccion no reconocida o prohibida */
        THREAD_DIVISION_BY_ZERO,    /** Division por cero */

        THREAD_STACK_OVERFLOW, /** Stack del hilo se desbordo:
                                         * El tope de pila(sp) se encontro con el limite de pila del hilo.
                                         * Push mas alla del limite de stack
                                         */

        THREAD_STACK_UNDERFLOW, /**
                                         * Stack se leyo cuando estaba vacio( Pop de stack vacio ),
                                         *  SP se intento decrementar a un valor inferios a BP,
                                         *  el tope de pila siempre debe de ser superior o igual a
                                         *  el puntero de la base de pila
                                         */
        THREAD_INVALID_SYSCALL, /** Llamada al sistema invalida o no soportada */
    } state_err_thread;

    /**
     * Tamaño de la tabla cache de instrucciones decodificadas.
     */
    static constexpr uint32_t ICACHE_SIZE = 1024;

    /**
     * Se usa para realizar el cacheado de las isntrucciones descodifcadas
     * @param pc Direccion PC de la instruccion descodificada.
     * @return entrada en la tabla cache.
     */
    inline uint32_t icache_index(uint64_t pc) {
        return pc & (ICACHE_SIZE - 1); // si ICACHE_SIZE es potencia de 2
    }

    class ProcessVM {
    public:
        /**
         * Id del proceso actual
         */
        GlobalPID pid;

        /**
         * ¿Qué es una “reducción”?
         * Una reducción es una unidad de trabajo, normalmente:
         *      - ejecutar 1 instrucción
         *      - o ejecutar 1 operación costosa
         *      - o avanzar 1 paso en la máquina de estados
         *
         * 1 instrucción ejecutada = 1 reducción
         */
        uint64_t reductions_remaining = reductions_remaining_default;


        // registros
        context_registers_vm registers;

        uint64_t tsc{}; // cantidad de instrucciones ejecutadas
        // -----------------------------------------------------------


        uint64_t         time_sleep{}; /** usado para almacenar un valor numerico, el cual es en ns, la hora
                                     *  a la que despertar el hilo.
                                     *  Al dormir el hilo, se indica que su estado es BLOCK
                                     */
        state_err_thread err_thread = THREAD_NO_ERROR; /**
                                             * Almcane el ultimo error ocurrido en el hilo.
                                             */


        /**
         * Contiene el estado de la maquina virtual. Nunca debe ser accesible directamente
         * ya que la VM usa un hilo para cambiar los estados, si otro hilo modifica el estado
         * en ejecuccion sin mas puede ocasionar problemas, por eso lo ponemos privado y creamos
         * un metodo que nos permita cambiar el estado de forma segura.
         */
        vm_state state = NEW;

        // -------------------------------------------------------------------------------
        //               sistema de cache para descodificacion de instrucciones.
        // -------------------------------------------------------------------------------
        // 256 entradas de cache maximo
        DecodedInstr icache[ICACHE_SIZE]     = {};

        /**
         * Contiene los datos de la instruccion descoficada.
         */
        DecodedInstr *decoded_ptr = nullptr;
        // -------------------------------------------------------------------------------

        /**
         * Cada Proceso gestiona su propio memoria (memoria aislada)
         */
        vm::ArenaManager manager_mem_priv{};

        tlb::LazyHybridTLB tlb{};
        vm::VirtualMemory  vm_mem;

        /**
         * Instancia global manejador de este proceso.
         */
        Scheduler &scheduler;

        ProcessVM(Scheduler &scheduler, GlobalPID pid);

        /**
         * Permite invalidar toda la cache del proceso.
         */
        void reset_cache();

        /**
         * Permite cargar codigo crudo a un proceso, pone el hilo en un
         * estado de RUNNING automaticamente.
         * @param address direccion virtual donde realizar la carga de las isntrucciones
         * @param code codigo a cargar en la direccion virtual.
         */
        void load_raw_code(uint64_t address, const std::vector<uint8_t> &code);

        std::string vm_summary() const;

        ~ProcessVM();

        [[nodiscard]] std::string to_string() const;

    private:
    };
}




#endif //PROCESO_RUNTIME_H
