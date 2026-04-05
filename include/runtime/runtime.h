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
#include <vector>

#include "decode_instruction.h"
#include "arena/arena.h"
#include "arena/VirtualMemory.h"
#include "cli/sync_io.h"
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
         * @brief La VM está en fase de obtención de instrucción (fetch).
         *
         * @details
         * Se usa cuando:
         *  - La VM está leyendo la instrucción desde memoria virtual.
         *
         * Implica:
         *  - Se accede al TLB y al subsistema de memoria.
         *  - Se prepara la instrucción para decodificación.
         */
        FETCH,

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
         * @brief La instrucción ha sido obtenida correctamente desde memoria.
         *
         * @details
         * Se dispara cuando:
         *  - La fase FETCH termina de leer la instrucción desde memoria virtual.
         *
         * Implica:
         *  - La VM puede pasar a DECODE para interpretar la instrucción.
         */
        EVT_FETCH_DONE,

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
            case EVT_FETCH_DONE: return "EVT_FETCH_DONE";
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
        vm_state next;
        std::function<void(runtime::VM *)> action;

        /**
         * Constructor de transicion, si no es especifica el estado siempre
         * sera READY.
         * @param n estado de transicion
         * @param a accion de transicion.
         */
        Transition(vm_state n = READY, void (*a)(runtime::VM *) = nullptr)
            : next(n), action(std::move(a)) {
        }
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
            case FETCH: return "FETCH";
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
        THREAD_NO_ERROR = 0, /** Sin error */
        THREAD_UNKNOWN_ERROR, /** Error no clasificado */
        THREAD_SEGMENTATION_FAULT, /** Un hilo intento acceder a memoria a la cual no tiene permisos */
        THREAD_ILLEGAL_INSTRUCTION, /** Instruccion no reconocida o prohibida */
        THREAD_DIVISION_BY_ZERO, /** Division por cero */

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
        size_t size = 0;

        uint8_t *code = nullptr;
        err_shellcode err = NO_ERROR_SC;

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
        void * (*func)(void *arg) = nullptr; /** funcion nativa que llamar  */
        shellcode_t *arg = nullptr; /** argumentos para la funcion, formando un shellcode */
        void *result = nullptr; /** valor de retorno */
        bool finished = false; /** indica si la funcion fue ejecutada*/

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
        uint8_t opcode[2]{};
        uint8_t size = 0;

        /**
         * Indica si la instruccion tiene o no signo
         */
        uint8_t _signed_instruct = false;

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
                uint8_t reg; // registro destino
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
                uint8_t reg_base; // base
                uint8_t reg_index; // indice
                uint8_t reg_final;
                uint8_t scale; // escalar
            } mem_data;
        } data_instruction = {
            static_cast<uint64_t>(0),
            static_cast<uint64_t>(0)
        };

        /**
         * Modo de la instruccion, o tamaño tambien llamado en algunos casos.
         */
        uint8_t mode = 0;

        int64_t imm = 0; // inmediato (si existe)

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
        bool did_jump = false;

        /**
         * Indica si la instruccion requiere esperar un I/O o a alguna
         * accion desbloqueante.
         */
        bool blocking = false;

        /**
         * referencia a la instruccion descodificada con sus meta-datos, la funcion a ejecutar,
         * el metodo de descodificacion y otros campos utiles.
         */
        InstrFormat *metadata = nullptr;
    } DecodedInstr;

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
        static const int ICACHE_SIZE = 256;
        uint64_t icache_tag[ICACHE_SIZE];
        DecodedInstr icache[ICACHE_SIZE];

        inline uint32_t icache_index(uint64_t pc) {
            return pc & (ICACHE_SIZE - 1); // si ICACHE_SIZE es potencia de 2
        }

        /**
         * Contiene los datos de la instruccion descoficada.
         */
        DecodedInstr decoded;
        // -------------------------------------------------------------------------------

        /**
         * Cola de eventos pendientes, si alguna instruccion o algo externo
         * quiero generar algun evento, se debe poner a la cola y hasta que la VM
         * no termine su evento actual no se podra realizar los eventos de la cola.
         */
        //std::queue<vm_event> pending_events;

        /**
         * Tabla de transiciones
         */
        Transition fsm[NUM_STATES][NUM_EVENTS];

        /**
         * Cada Instancia gestiona su propio memoria (memoria aislada)
         */
        vm::ArenaManager manager_mem_priv{};

        tlb::LazyHybridTLB tlb{};
        vm::VirtualMemory vm_mem;

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


        pthread_t thread_for_vm{};
        VM_ID id{};

        // registros
        // -----------------------------------------------------------
        vm::MappedPtr stack_pointer{}; // puntero tope de la pila
        vm::MappedPtr base_pointer{}; // puntero base de la pila

        vm::MappedPtr rip{}; // puntero de instruccion
        RFlags_t flags{};

        /**
         * Registros de proposito general.
         */
        GeneralRegister regs[16];

        /**
         * Cada instancia tiene asignada un manager general de instancias
         */
        ManageVM &mgr_vm;

        uint64_t tsc{}; // cantidad de instrucciones ejecutadas
        // -----------------------------------------------------------


        uint64_t time_sleep{}; /** usado para almacenar un valor numerico, el cual es en ns, la hora
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

        std::string to_string();

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

        void join();

        std::string vm_summary() {
            std::ostringstream ss;
            // ID
            ss << "ID=" << vesta::hex64(static_cast<uint64_t>(id.id));

            // Estado
            ss << " st=" << vm_state_to_str(state);
            ss << std::endl;

            ss << " R00=" << vesta::hex64(regs[R00].qword());
            ss << " R01=" << vesta::hex64(regs[R01].qword());
            ss << " R02=" << vesta::hex64(regs[R02].qword());
            ss << " R03=" << vesta::hex64(regs[R03].qword());
            ss << std::endl;

            ss << " R04=" << vesta::hex64(regs[R04].qword());
            ss << " R05=" << vesta::hex64(regs[R05].qword());
            ss << " R06=" << vesta::hex64(regs[R06].qword());
            ss << " R07=" << vesta::hex64(regs[R07].qword());
            ss << std::endl;

            ss << " R08=" << vesta::hex64(regs[R08].qword());
            ss << " R09=" << vesta::hex64(regs[R09].qword());
            ss << " R10=" << vesta::hex64(regs[R10].qword());
            ss << " R11=" << vesta::hex64(regs[R11].qword());
            ss << std::endl;

            ss << " R12=" << vesta::hex64(regs[R12].qword());
            ss << " R13=" << vesta::hex64(regs[R13].qword());
            ss << " R14=" << vesta::hex64(regs[R14].qword());
            ss << " R15=" << vesta::hex64(regs[R15].qword());
            ss << std::endl;

            // IP/SP/BP (usar component_to_string para capturar representación)
            ss << " IP=" << vesta::component_to_string(rip);
            ss << " SP=" << vesta::component_to_string(stack_pointer);
            ss << " BP=" << vesta::component_to_string(base_pointer);

            // Flags compactas
            ss << " FLAGS="
                    << static_cast<unsigned>(flags.bits.DM)
                    << static_cast<unsigned>(flags.bits.CF)
                    << static_cast<unsigned>(flags.bits.OF)
                    << static_cast<unsigned>(flags.bits.SF)
                    << static_cast<unsigned>(flags.bits.ZF);

            // Thread / sleep
            ss << " Th=" << reinterpret_cast<void *>(thread_for_vm)
                    << " Sleep=" << time_sleep;

            return ss.str();
        }

        void fetch_instruction();

        void decode_instruction();

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

    private:
        /**
         * Contiene el estado de la maquina virtual. Nunca debe ser accesible directamente
         * ya que la VM usa un hilo para cambiar los estados, si otro hilo modifica el estado
         * en ejecuccion sin mas puede ocasionar problemas, por eso lo ponemos privado y creamos
         * un metodo que nos permita cambiar el estado de forma segura.
         */
        vm_state state = READY;

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


    static void dump_vm_region(VM *vm, uint64_t vaddr, size_t size) {
        uint64_t start = vaddr & ~0xFFFULL; // Alinear a página
        uint64_t end = (vaddr + size + 0xFFFULL) & ~0xFFFULL;

        vesta::SyncOStream out = vesta::scout(); // salida thread-safe

        out << "==================== VM Memory Dump ====================\n";
        out << "Virtual region: 0x" << std::hex << start
                << " - 0x" << end
                << "  (" << std::dec << size << " bytes)\n\n";

        for (uint64_t page = start; page < end; page += 0x1000) {
            void *host = vm->tlb.get_real_host_ptr_of_vptr(page);

            if (!host) {
                out << "  [0x" << std::hex << page << "]  ->  <not mapped>\n";
                continue;
            }

            out << "  [0x" << std::hex << page << "]  ->  host=" << host << "\n";

            // Mostrar primeros 16 bytes en formato hexdump
            uint8_t *p = reinterpret_cast<uint8_t *>(host);

            out << "       data: ";

            for (int i = 0; i < 16; i++) {
                char buf[4];
                std::snprintf(buf, sizeof(buf), "%02X", p[i]); // formateo manual
                out << buf << " ";
            }

            out << "\n";
        }

        out << "================== End VM Memory Dump ==================\n";
    }
}

#endif //RUNTIME_H
