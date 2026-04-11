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
#ifndef RUNTIME_H
#define RUNTIME_H
#include <atomic>
#include <cstdint>
#include <functional>
#include <queue>
#include <thread>
#include <vector>

#include "decode_instruction.h"
#include "arena/arena.h"
#include "arena/VirtualMemory.h"
#include "cli/sync_io.h"
#include "profiler/timer.h"
#include "runtime/rflags.h"

#define VERSION_VM 0

namespace loader {
    class Loader;
}

namespace runtime {
    struct InstrFormat;
    class ManageVM;
    class VM;

    typedef struct VM_ID {
        uint64_t id = 0;
    } VM_ID;

    /**
     * @brief Representa los posibles estados de los hilos de la VM.
     *
     * Cada hilo dentro de la VM puede encontrarse en uno de estos estados,
     * lo cual permite al planificador (scheduler) controlar su ejecución,
     * suspensión, bloqueo y finalización.
     */
    typedef enum vm_state {
        /**
         * @brief El hilo está listo para ejecutarse, pero no está corriendo.
         *
         * @details
         * Se usa cuando:
         *  - El hilo ha sido creado y ya tiene su contexto inicial preparado.
         *  - El hilo ha cedido voluntariamente la CPU (yield).
         *  - El hilo ha sido desbloqueado tras un evento o temporizador.
         *
         * Implica:
         *  - El scheduler puede seleccionarlo para ejecutarse?
         */
        READY,


        /**
         * @brief El hilo está actualmente ejecutándose.
         *
         * @details
         * Se usa cuando:
         *  - El scheduler selecciona este hilo en un tick.
         *
         * Implica:
         *  - El hilo tiene el control de la "CPU" virtual.
         *  - Puede cambiar a otro estado según avance la ejecución.
         */
        RUNNING,

        /**
         * @brief El hilo está bloqueado esperando un evento, tiempo o señal.
         *
         * @details
         * Se usa cuando:
         *  - El hilo recién creado aún no está listo para ejecutarse.
         *  - El hilo espera un evento externo (I/O, sincronización, etc.).
         *  - El hilo espera un temporizador o condición.
         *
         * Implica:
         *  - El hilo no será ejecutado hasta que sea desbloqueado.
         */
        BLOCKED,

        /**
         * @brief El hilo ha terminado su ejecución permanentemente.
         *
         * @details
         * Se usa cuando:
         *  - La función principal del hilo retorna.
         *  - El hilo finaliza por un error fatal.
         *  - El hilo se cierra explícitamente.
         *
         * Implica:
         *  - El hilo no volverá a ejecutarse.
         *  - Su stack y recursos pueden ser liberados.
         */
        DEAD,

        /**
         * @brief La VM está decodificando la instrucción obtenida.
         *
         * @details
         * Se usa cuando:
         *  - La instrucción ya fue leída y ahora se interpreta.
         *
         * Implica:
         *  - Se determina el opcode, operandos y modo de direccionamiento.
         *  - Se preparan los datos necesarios para la ejecución.
         */
        DECODE,

        /**
         * @brief La VM está ejecutando la instrucción decodificada.
         *
         * @details
         * Se usa cuando:
         *  - La instrucción ya fue decodificada y se está ejecutando.
         *
         * Implica:
         *  - Se modifican registros, memoria o estado interno.
         *  - Puede generar cambios de estado (WAIT_IO, BLOCKED, etc.).
         */
        EXECUTE,


        /**
         * @brief La VM está esperando una operación de entrada/salida.
         *
         * @details
         * Se usa cuando:
         *  - La instrucción requiere esperar un dispositivo o evento externo.
         *
         * Implica:
         *  - El hilo no puede continuar hasta que la operación I/O termine.
         *  - El scheduler puede ejecutar otros "hilos" mientras tanto.
         */
        WAIT_IO,

        /**
         * @brief La VM ha detenido su ejecución.
         *
         * @details
         * Se usa cuando:
         *  - La VM recibe una instrucción HALT.
         *  - Se detiene explícitamente por el usuario o el sistema.
         *
         * Implica:
         *  - La VM no ejecutará más instrucciones.
         *  - Puede ser reiniciada o destruida.
         */
        HALT,

        /**
         * Este no es un estado real, sino que se usa para obtener cuantos estados
         * existen en nuestra maquina de estados.
         */
        NUM_STATES
    } vm_state;

    /**
     * @brief Eventos que disparan transiciones en la máquina de estados de la VM.
     *
     * Cada evento representa un suceso interno del ciclo de ejecución
     * o una condición externa que obliga a cambiar de estado.
     */
    enum vm_event {
        /**
         * @brief El scheduler ha seleccionado este hilo para ejecutarse.
         *
         * @details
         * Este evento es generado por el planificador (scheduler) cuando decide
         * que la VM debe reanudar su ejecución. Normalmente se dispara cuando:
         *
         *  - La VM estaba en estado READY esperando turno.
         *  - La VM ha sido desbloqueada tras un WAIT_IO.
         *  - El sistema decide asignarle tiempo de CPU.
         *
         * Implica:
         *  - La VM pasa del estado READY al estado RUNNING.
         *  - El hilo virtual obtiene control de la "CPU" virtual.
         *  - El siguiente estado típico es FETCH, donde comienza el ciclo de instrucción.
         *
         * Este evento no es generado por la VM internamente, sino por el
         * scheduler externo que gestiona múltiples instancias de VM?
         */
        EVT_SCHEDULED,

        /**
         * @brief La instrucción ha sido decodificada correctamente.
         *
         * @details
         * Se dispara cuando:
         *  - La fase DECODE identifica el opcode, operandos y modo de direccionamiento.
         *
         * Implica:
         *  - La VM puede pasar a EXECUTE para ejecutar la instrucción.
         */
        EVT_DECODE_DONE,

        /**
         * @brief La instrucción ha sido ejecutada completamente.
         *
         * @details
         * Se dispara cuando:
         *  - La fase EXECUTE finaliza sin requerir I/O ni bloquearse.
         *
         * Implica:
         *  - La VM puede volver a FETCH para obtener la siguiente instrucción.
         */
        EVT_EXEC_DONE,

        /**
         * @brief La instrucción requiere esperar una operación de entrada/salida.
         *
         * @details
         * Se dispara cuando:
         *  - La ejecución detecta que necesita esperar un dispositivo o evento externo.
         *
         * Implica:
         *  - La VM debe pasar a WAIT_IO hasta que el evento se complete.
         */
        EVT_IO_WAIT,

        /**
         * @brief La operación de entrada/salida ha finalizado.
         *
         * @details
         * Se dispara cuando:
         *  - Un dispositivo, hilo externo o evento notifica que la I/O terminó.
         *
         * Implica:
         *  - La VM puede volver a READY para ser planificada nuevamente.
         */
        EVT_IO_READY,

        /**
         * @brief Se ha ejecutado una instrucción HALT o se solicita detener la VM.
         *
         * @details
         * Se dispara cuando:
         *  - La instrucción HALT es encontrada.
         *  - El sistema solicita detener la ejecución.
         *
         * Implica:
         *  - La VM pasa al estado HALT y no ejecutará más instrucciones.
         */
        EVT_HALT,

        /**
         * @brief Ha ocurrido un error fatal durante la ejecución.
         *
         * @details
         * Se dispara cuando:
         *  - Se detecta un error irrecuperable (memoria inválida, instrucción ilegal, etc.).
         *
         * Implica:
         *  - La VM pasa al estado DEAD.
         *  - Se liberan recursos y se detiene la ejecución.
         */
        EVT_ERROR,

        /**
         * @brief Número total de eventos (no es un evento real).
         */
        NUM_EVENTS
    };

    static const char *event_name(vm_event e) {
        switch (e) {
            case EVT_DECODE_DONE: return "EVT_DECODE_DONE";
            case EVT_EXEC_DONE: return "EVT_EXEC_DONE";
            case EVT_IO_WAIT: return "EVT_IO_WAIT";
            case EVT_IO_READY: return "EVT_IO_READY";
            case EVT_HALT: return "EVT_HALT";
            case EVT_ERROR: return "EVT_ERROR";
            case EVT_SCHEDULED: return "EVT_SCHEDULED";
            default: return "UNKNOWN_EVENT";
        }
    }

    /**
     * ID de registros de proposito general
     */
    enum RegID {
        R00, R01, R02, R03,
        R04, R05, R06, R07,
        R08, R09, R10, R11,
        R12, R13, R14, R15,

        /**
         * Se usa para contar cuantos registros de proposito general hay
         */
        COUNT
    };


    /**
     * Estructura que representa una transicion de la maquina de estados.
     */
    typedef struct Transition {
        vm_state                           next;
        std::function<void(runtime::VM *)> action;

        /**
         * Constructor de transicion, si no es especifica el estado siempre
         * sera READY.
         * @param n estado de transicion
         * @param a accion de transicion.
         */
        Transition(vm_state n = READY, void (*a)(runtime::VM *) = nullptr)
            : next(n), action(std::move(a)) {}
    } Transition;


    /**
     * Estados de la la FSM(Maquina de estasdos finitio) en la VM
     * @param state estado de la VM
     * @return representacion string
     */
    static const char *vm_state_to_str(vm_state state) {
        switch (state) {
            case READY: return "READY";
            case RUNNING: return "RUNNING";
            case BLOCKED: return "BLOCKED";
            case DECODE: return "DECODE";
            case EXECUTE: return "EXECUTE";
            case WAIT_IO: return "WAIT_IO";
            case HALT: return "HALT";
            case DEAD: return "DEAD";
            default: return "UNKNOWN";
        }
    }

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

    typedef enum err_shellcode {
        NO_ERROR_SC,
        REALLOC_ERROR,
        EmitIndirect_ERROR,
        EmitIndirectByteDisplaced_ERROR,
        EmitIndirectDisplaced_ERROR,
        EmitIndirectIndexed_ERROR,
        EmitIndirectIndexedDisplaced_ERROR,
        EmitIndirectIndexedByteDisplaced_ERROR
    } err_shellcode;

    typedef struct shellcode_t {
        size_t capacity = 0;
        size_t size     = 0;

        uint8_t *     code = nullptr;
        err_shellcode err  = NO_ERROR_SC;

        void (*Emit8)(struct shellcode_t *code, uint8_t byte);

        void (*Emit32)(struct shellcode_t *code, uint32_t bytes);

        void (*Emit64)(struct shellcode_t *code, uint64_t bytes);

        void (*expand)(struct shellcode_t *code);

        void (*free)(struct shellcode_t *code);

        void (*dump)(const struct shellcode_t *shell);
    } shellcode_t;

    /**
     * Estructura para llamar a funciones nativas/externas a la VM.
     */
    typedef struct PendingCall_t {
        void * (*    func)(void *arg) = nullptr; /** funcion nativa que llamar  */
        shellcode_t *arg              = nullptr; /** argumentos para la funcion, formando un shellcode */
        void *       result           = nullptr; /** valor de retorno */
        bool         finished         = false;   /** indica si la funcion fue ejecutada*/

        pthread_mutex_t lock; /** Proteccion de acceso para finished/result */
    } PendingCall_t;

    class GeneralRegister {
    private:
        uint64_t data = 0;

    public:
        // Lectura segura (zero-extend)
        [[nodiscard]] uint8_t byte_lo() const {
            return uint8_t(data & 0xFF);
        }

        [[nodiscard]] uint8_t byte_hi() const {
            return uint8_t((data >> 8) & 0xFF);
        }

        [[nodiscard]] uint16_t word_lo() const {
            return uint16_t(data & 0xFFFF);
        }

        [[nodiscard]] uint16_t word_hi() const {
            return uint16_t((data >> 48) & 0xFFFF);
        }

        [[nodiscard]] uint32_t dword_lo() const {
            return uint32_t(data & 0xFFFFFFFF);
        }

        [[nodiscard]] uint32_t dword_hi() const {
            return uint32_t((data >> 32) & 0xFFFFFFFF);
        }

        [[nodiscard]] uint64_t qword() const {
            return data;
        }

        // Escritura segura (zero-extend automático)
        GeneralRegister &byte_lo(uint8_t v) {
            data = (data & ~0xFFULL) | v;
            return *this;
        }

        GeneralRegister &byte_hi(uint8_t v) {
            data = (data & ~0xFF00ULL) | (static_cast<uint64_t>(v) << 8);
            return *this;
        }

        GeneralRegister &word_lo(uint16_t v) {
            data = (data & ~0xFFFFULL) | v;
            return *this;
        }

        GeneralRegister &word_hi(uint16_t v) {
            data = (data & ~0xFFFF000000000000ULL) | (static_cast<uint64_t>(v) << 48);
            return *this;
        }

        GeneralRegister &dword_lo(uint32_t v) {
            data = (data & 0xFFFFFFFF00000000ULL) | v;
            return *this;
        }

        GeneralRegister &dword_hi(uint32_t v) {
            data = (data & 0xFFFFFFFFULL) | (static_cast<uint64_t>(v) << 32);
            return *this;
        }

        GeneralRegister &qword(uint64_t v) {
            data = v;
            return *this;
        }

        // Acceso directo al raw
        [[nodiscard]] uint64_t raw() const {
            return data;
        }

        void raw(uint64_t v) {
            data = v;
        }
    };


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
            false, false, 0, 0
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
     * Estados de depuracion
     */
    enum class DebugStage {
        /**
         * realizar la depuracion en fase de descoficacion de instruccion.
         *
         */
        DecodeBegin,
        DecodeEnd,

        /**
         * permite realiza la depuracion en la fase previa a la ejecuccion de la
         * instruccion.
         */
        ExecuteBegin,

        /**
         * permite realizar la depuracion en la fase final de la ejecuccion de la instruccion.
         */
        ExecuteEnd,

        /**
         * Permite hacer el hook antes de hacer el cambio de un estado a taves de un
         * evento
         */
        OnEventBegin,

        /**
         * Permite hacer el hook despues del cambio de estado y el desencadenamiento del
         * evento.
         */
        OnEventEnd,
    };

    using DebugHook = void(*)(VM *vm, DebugStage stage);

    /**
     * Tamaño de la tabla cache de instrucciones decodificadas.
     */
    static const uint32_t ICACHE_SIZE = 256;

    /**
     * Esta clase representa una instancia de VM.
     * Cada instancia de VM usa un hilo real para ejecutar el codigo dado.
     * Por cada instancia de VM tendremos un hilo por tanto tendremos tantos
     * hilos como instancias, ademas de un hilo principal que sera el que envie y reciba
     * datos a maquina externas e internas.
     *
     * El hilo principal puede crear nuevas instancias o el usuario podra crear nuevas
     */
    class VM {
    public:
        // -------------------------------------------------------------------------------
        //               sistema de cache para descodificacion de instrucciones.
        // -------------------------------------------------------------------------------
        // 256 entradas de cache maximo
        uint64_t     icache_tag[ICACHE_SIZE];
        DecodedInstr icache[ICACHE_SIZE];

        /**
         * Contiene los datos de la instruccion descoficada.
         */
        DecodedInstr *decoded_ptr = nullptr;
        // -------------------------------------------------------------------------------

        /**
         * Cola de eventos pendientes, si alguna instruccion o algo externo
         * quiero generar algun evento, se debe poner a la cola y hasta que la VM
         * no termine su evento actual no se podra realizar los eventos de la cola.
         */
        //std::queue<vm_event> pending_events;

        Timer debug_timer{}; // mide la fase actual en el modo de depuracion

        /**
         * toiempo tardado en el decoder
         */
        uint64_t time_decode = 0;

        /**
         * tiempo de transicion de eventos.
         */
        uint64_t time_event = 0;

        /**
         * tiempo tardado en la ejecuccion
         */
        uint64_t time_exec = 0;

        // --- PROFILER POR HILO / VM---
        uint64_t profiler_sample        = 0; // contador de instrucciones
        uint64_t profiler_instr_counter = 0; // incrementa cada 256 instrucciones
        // --- PROFILER POR HILO / VM ---

        /**
         * Tabla de transiciones
         */
        Transition fsm[NUM_STATES][NUM_EVENTS];

        /**
         * Cada Instancia gestiona su propio memoria (memoria aislada)
         */
        vm::ArenaManager manager_mem_priv{};

        tlb::LazyHybridTLB tlb{};
        vm::VirtualMemory  vm_mem;

        /**
         * Manager de memoria "publico" del manager de instancias
         */
        vm::ArenaManager &manager_mem_public;

        /**
         * Cada instancia de loader permite manejar sus propias cargas
         */
        std::unique_ptr<loader::Loader> loader_priv;

        /**
         * Loader "publico" del manager de instancias.
         */
        loader::Loader &loader_public;


        /**
         * Se usa para realizar depuracion o realizar ciertas acciones antes
         * de una fase, especificada
         */
        std::vector<DebugHook> debug_hooks;

        /**
         * Permite activar y desactivar hooks, al añadir un hook
         * usando add_debug_hook automaticamente esta flag se activa,
         * se puede desactivar en cualquier momento para que los hooks
         * no interfieran en la ejecuccion de la VM.
         */
        bool has_hooks = false;

        pthread_t thread_for_vm{};
        VM_ID     id{};

        // registros
        // -----------------------------------------------------------
        GeneralRegister stack_pointer{}; // puntero tope de la pila
        GeneralRegister base_pointer{};  // puntero base de la pila

        GeneralRegister rip{}; // puntero de instruccion
        RFlags_t        flags{};

        /**
         * Registros de proposito general.
         */
        GeneralRegister regs[16];

        /**
         * Registros cursor, cur0, cur1, cur2 y cur3
         */
        GeneralRegister cur[4];

        /**
         * Cada instancia tiene asignada un manager general de instancias
         */
        ManageVM &mgr_vm;

        uint64_t tsc{}; // cantidad de instrucciones ejecutadas
        // -----------------------------------------------------------


        uint64_t         time_sleep{}; /** usado para almacenar un valor numerico, el cual es en ns, la hora
                                     *  a la que despertar el hilo.
                                     *  Al dormir el hilo, se indica que su estado es BLOCK
                                     */
        state_err_thread err_thread = THREAD_NO_ERROR; /**
                                             * Almcane el ultimo error ocurrido en el hilo.
                                             */

        PendingCall_t *pending_call{}; /** Llamada nativa en curso, si hay alguna */


        VM(ManageVM &mgr_vm, uint64_t id_vm);

        /**
         * Inicializa la tabla de transicicones.
         */
        void init_fsm();

        /**
         * Permite transiccionar de estado-
         *
         *  Ejemplo:
         *      1. Mira el estado actual -> EXECUTE
         *      2. Mira el evento recibido -> EVT_EXEC_DONE
         *      3. Busca en la tabla -> fsm[EXECUTE][EVT_EXEC_DONE]
         *
         *      Esto te da:
         *          - el siguiente estado
         *          - la acción a ejecutar
         *
         * @param e nuevo estado para la maquina virtual
         */
        void on_event(vm_event e);

        std::string to_string() const;

        /**
         * @brief Imprime estado completo de la VM (debug)
         */
        void print();

        /**
         * Permite matar a la VM cambiado su estado mediante un evento de tipo
         * EVT_ERROR. Esta funcion solo se permite llamar desde fuera.
         */
        void kill();

        /**
         * Permite poner un evento en la cola
         * @param e evento a realizar, se pondra a la cola y se realizara cuando sea
         * necesario.
         */
        //void emit_event(vm_event e);

        /**
         * @brief Inicia la ejecución de la VM en un hilo independiente.
         *
         * Este metodo crea un hilo real del sistema operativo y ejecuta dentro de él
         * el bucle principal de la VM (`run_loop()`), permitiendo que cada instancia
         * de VM funcione de manera concurrente y aislada.
         *
         * @details
         * - El hilo creado ejecuta exclusivamente `run_loop()`.
         * - El estado inicial debe ser READY o RUNNING según el scheduler.
         * - La VM continúa ejecutándose hasta que entra en un estado terminal
         *   (HALT o DEAD), momento en el cual el hilo finaliza automáticamente.
         * - Otros hilos pueden interactuar con la VM mediante eventos seguros
         *   (por ejemplo, para despertar WAIT_IO o forzar un error).
         *
         * Importante:
         *  - Este metodo no bloquea; simplemente lanza el hilo.
         *  - El hilo se almacena en `thread_for_vm` para permitir join, detach
         *    o gestión por parte del scheduler.
         */
        void start();

        /**
         * Permite hacer que el hilo que creo la VM espere a la finalizacion
         * de la VM a traves de algun error, la instruccion HLT u otro evento
         * o motivo que desencadene una finalizacion.
         */
        void join();

        /**
         * Permite agregar una nueva hook a la VM, esta funcion no es thread-safe,
         * asi que no se debe usar para añadir un hook mientras la VM se ejecuta, a
         * no ser que se haga uso de algun mecanismo de sincronizacion. Se recomienda usar
         * la version safe-thread la cual es "add_debug_hook" y añade un mutex que permite
         * añadir hooks en run time mientras la VM se ejecuta.
         * @param hook funcion hook que añadir a la cola de hooks.
         */
        void free_add_debug_hook(DebugHook hook);

        void add_debug_hook(DebugHook hook);

        std::string vm_summary() const {
            std::ostringstream ss;

            ss << "ID=" << vesta::hex64((uint64_t) id.id)
                    << " st=" << vm_state_to_str(state) << "\n";

            // Registros generales R00–R15
            for (int i = 0; i < 16; ++i) {
                ss << " R" << std::setw(2) << std::setfill('0') << i
                        << "=" << vesta::hex64(regs[i].qword());
                if (i % 2 == 1) ss << "\n";
            }
            ss << "\n";

            // CUR0–CUR3
            for (int i = 0; i < 4; ++i) {
                ss << " CUR" << i << "=" << vesta::hex64(cur[i].qword());
                if (i % 2 == 1) ss << "\n";
            }
            ss << "\n";

            // IP/SP/BP
            ss << " RIP=" << vesta::component_to_string(rip)
                    << " RSP=" << vesta::component_to_string(stack_pointer)
                    << " RBP=" << vesta::component_to_string(base_pointer)
                    << "\n";

            // FLAGS
            ss << " FLAGS=["
                    << "CF=" << (int) flags.bits.CF << " "
                    << "OF=" << (int) flags.bits.OF << " "
                    << "SF=" << (int) flags.bits.SF << " "
                    << "ZF=" << (int) flags.bits.ZF << " "
                    << "DM=" << (int) flags.bits.DM
                    << "]\n";

            // Thread / sleep
            ss << " Th=" << (void *) thread_for_vm
                    << " Sleep=" << time_sleep;

            return ss.str();
        }


        void decode_instruction();


        void load_raw_code(uint64_t address, const std::vector<uint8_t> &code);

        /**
         * Ejecuta la instrucción actualmente decodificada y devuelve el evento
         * que debe procesar la máquina virtual como resultado de dicha ejecución.
         *
         * En lugar de un valor booleano, este metodo devuelve directamente un
         * vm_event que representa la transición que debe realizar la FSM.
         *
         * Esto permite que una instrucción genere múltiples tipos de eventos:
         *  - EVT_EXEC_DONE  -> La instrucción terminó correctamente.
         *  - EVT_IO_WAIT    -> La instrucción es bloqueante y requiere esperar E/S.
         *  - EVT_HALT       -> La instrucción solicita detener la VM.
         *  - EVT_ERROR      -> Se produjo un error fatal durante la ejecución.
         *  - Otros eventos específicos según la arquitectura de la VM.
         *
         * El metodo también se encarga de avanzar el contador de programa (PC)
         * si la instrucción no ha modificado explícitamente su valor (campo did_jump).
         *
         * @return vm_event  Evento que la FSM debe procesar tras ejecutar la instrucción.
         */
        vm_event execute_instruction();


        /**
         * Permite indicar internamete que la VM debe morir o deberia estar
         * muerta, esto lo hacemos ya que no se puede llamar a on_event desde
         * dentro de la propio VM ni se puede llamar a kill() el cual llama a on_event.
         */
        bool should_kill = false;

        /**
         * Contiene el estado de la maquina virtual. Nunca debe ser accesible directamente
         * ya que la VM usa un hilo para cambiar los estados, si otro hilo modifica el estado
         * en ejecuccion sin mas puede ocasionar problemas, por eso lo ponemos privado y creamos
         * un metodo que nos permita cambiar el estado de forma segura.
         */
        vm_state state = READY;

        /**
         * permite indicar que se puede seguir ejecutado las funcionalidades
         * de "profiler" internas o externas a la VM.
         */
        std::atomic<bool> profiler_running = true;

    private:
        /**
         * Mutex unico para cada instancia de VM. Con esto tenemos seguridad de que solo un hilo
         * modifique el estado a la vez.
         */
        std::mutex state_lock;

        /**
         * @brief Bucle principal de ejecución de la máquina virtual.
         *
         * Este metodo implementa el ciclo de ejecución continuo de la VM utilizando
         * un modelo de máquina de estados finita (FSM) combinado con un mecanismo
         * de salto directo (dispatch table) para maximizar el rendimiento.
         *
         * @details
         * El bucle realiza repetidamente las siguientes operaciones:
         *
         *  1. Salta directamente al bloque de código asociado al estado actual
         *     mediante una tabla de despacho. Esto evita el uso de estructuras
         *     condicionales como `switch` o `if`, reduciendo la penalización por
         *     predicción de ramas y acelerando el intérprete.
         *
         *  2. Ejecuta la acción correspondiente al estado (por ejemplo: obtener
         *     una instrucción, decodificarla, ejecutarla o esperar I/O).
         *
         *  3. Determina qué evento ocurrió durante la ejecución del estado
         *     (por ejemplo: instrucción obtenida, decodificada, ejecutada,
         *     espera de I/O, finalización, etc.).
         *
         *  4. Llama a `on_event(evento)`, que consulta la tabla de transiciones
         *     `fsm[][]` y actualiza el estado de la VM según la lógica declarativa
         *     definida en la máquina de estados.
         *
         *  5. Tras aplicar la transición, el bucle vuelve a saltar al bloque
         *     correspondiente al nuevo estado, repitiendo el ciclo.
         *
         * El bucle finaliza únicamente cuando la VM entra en un estado terminal
         * como HALT o DEAD. La transición hacia dichos estados es gestionada por
         * `on_event()` y la tabla de transiciones, no por este metodo.
         *
         * Importante:
         *  - Este metodo **no modifica directamente el estado**. Toda transición
         *    ocurre exclusivamente dentro de `on_event()`, manteniendo la lógica
         *    de la FSM centralizada y coherente.
         *  - El metodo termina cuando el estado resultante es terminal, momento
         *    en el cual la VM deja de ejecutar instrucciones.
         */
        void run_loop();
    };

    /**
     * @brief Hilo de perfilado para una instancia de VM.
     *
     * Este hilo se ejecuta en paralelo a la VM y se encarga de:
     *   - Calcular las instrucciones por segundo (IPS) usando muestreo.
     *   - Calcular el porcentaje de CPU consumido por la VM.
     *   - Imprimir los resultados usando vesta::scout() (thread-safe).
     *
     * El cálculo funciona así:
     *   - Cada 256 instrucciones ejecutadas, la VM incrementa profiler_instr_counter.
     *   - Cada segundo, este hilo lee ese contador y calcula:
     *         IPS = delta * 256
     *   - El tiempo ocupado (busy time) se acumula en vm->time_exec (ns).
     *         CPU% = (time_exec / 1e9) * 100
     *
     *  IPS alto + CPU bajo -> la VM está idle o ejecuta pocas instrucciones por segundo.
     *  IPS alto + CPU alto -> la VM está ejecutando un bucle caliente.
     *  IPS bajo + CPU alto -> la VM está haciendo trabajo costoso por instrucción.
     *
     * Podemos ejecutar un profiler como se ve a continuacion:
     *
     * @code{.cpp}
     * std::thread(&runtime::profiler_thread, vm).detach();
     * @endcode
     *
     * profiler_thread depende de la flag interna vm->should_kill que indica
     * si la VM va a morir o deberia morir y de vm->profiler_running que
     * indica si la VM desea que se haga profiler no, en el caso de
     * esta funcion se debe cumplir la siguiente condicion:
     * @code{.cpp}
     *      !vm->should_kill && vm->profiler_running
     * @endcode
     * En caso de que alguna cambiem el profiler se detendra.
     *
     * @param vm Puntero a la instancia de VM que se está perfilando.
     */
    static void profiler_thread(VM *vm) {
        uint64_t last = 0;

        // mientras la vm no deba haber acabado
        while (!vm->should_kill && vm->profiler_running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            // --- IPS ---
            uint64_t now   = vm->profiler_instr_counter;
            uint64_t delta = now - last;
            last           = now;

            uint64_t ips = delta * 256;

            // --- CPU ---
            double cpu    = (vm->time_exec / 1e9) * 100.0;
            vm->time_exec = 0;

            // --- Salida thread-safe ---
            vesta::scout()
                    << "\r[VM " << vesta::hex64(vm->id.id) << "] "
                    << "IPS: " << ips
                    << " | CPU: " << cpu << "%\n";
        }
    }

    static void dump_vm_region(VM *vm, uint64_t vaddr, size_t size) {
        uint64_t start = vaddr & ~0xFFFULL; // Alinear a página
        uint64_t end   = (vaddr + size + 0xFFFULL) & ~0xFFFULL;


        vesta::scout() << "==================== VM Memory Dump ====================\n";
        vesta::scout() << "Virtual region: 0x" << std::hex << start
                << " - 0x" << end
                << "  (" << std::dec << size << " bytes)\n\n";

        for (uint64_t page = start; page < end; page += 0x1000) {
            void *host = vm->tlb.get_real_host_ptr_of_vptr(page);

            if (!host) {
                vesta::scout() << "  [0x" << std::hex << page << "]  ->  <not mapped>\n";
                continue;
            }

            vesta::scout() << "  [0x" << std::hex << page << "]  ->  host=" << host << "\n";

            // Mostrar primeros 16 bytes en formato hexdump
            uint8_t *p = reinterpret_cast<uint8_t *>(host);

            vesta::scout() << "       data: ";

            vesta::scout() << vesta::dump(p, size);

            vesta::scout() << "\n";
        }

        vesta::scout() << "================== End VM Memory Dump ==================\n";
    }

    static const char *debug_stage_name(DebugStage s) {
        switch (s) {
            case DebugStage::DecodeBegin: return "DECODE_BEGIN";
            case DebugStage::DecodeEnd: return "DECODE_END";
            case DebugStage::ExecuteBegin: return "EXEC_BEGIN";
            case DebugStage::ExecuteEnd: return "EXEC_END";
            case DebugStage::OnEventBegin: return "EVENT_BEGIN";
            case DebugStage::OnEventEnd: return "EVENT_END";
            default: return "UNKNOWN";
        }
    }

    /**
     * Se usa para realizar el cacheado de las isntrucciones descodifcadas
     * @param pc Direccion PC de la instruccion descodificada.
     * @return entrada en la tabla cache.
     */
    inline uint32_t icache_index(uint64_t pc) {
        return pc & (ICACHE_SIZE - 1); // si ICACHE_SIZE es potencia de 2
    }

    /**
     * @brief Ejecuta todos los hooks de depuración registrados para la máquina virtual.
     *
     * Esta función se invoca en distintos puntos del ciclo de ejecución de la VM
     * (fetch, decode, execute, eventos del FSM, etc.) y permite que múltiples
     * callbacks externos reaccionen a cada fase sin interferir con la lógica
     * interna del intérprete.
     *
     * Cada hook registrado en `vm->debug_hooks` recibe el puntero a la VM y el
     * estado de depuración actual, permitiendo implementar:
     *  - trazas de ejecución,
     *  - breakpoints,
     *  - stepping,
     *  - inspección del estado interno,
     *  - profiling por instrucción o por evento,
     *  - integración con debuggers externos.
     *
     * @param vm    Puntero a la máquina virtual que está ejecutando el ciclo.
     * @param stage Fase del pipeline o del FSM en la que se dispara el hook.
     *
     * @note Esta función no realiza comprobaciones adicionales: si un hook lanza
     *       una excepción o produce efectos secundarios inesperados, puede afectar
     *       al flujo de depuración. Se recomienda que los hooks sean seguros y
     *       no bloqueantes.
     */
    //#define PROFILE_FAST
#ifdef PROFILE_FAST
#   define vm_hook(...) do {} while(0)
#else
    inline void vm_hook(VM *vm, DebugStage stage) {
        if (!vm->has_hooks) return;

        for (auto &hook: vm->debug_hooks)
            hook(vm, stage);
    }
#endif
}

#endif //RUNTIME_H
