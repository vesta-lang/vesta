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
 * @file proceso_runtime.h
 * @brief Declaracion del contexto de proceso virtual (ProcessVM) de VestaVM.
 *
 * Declara @c ProcessVM: registros generales (R00-R15), PC, SP, BP,
 * flags de estado, pila de llamadas, estado del proceso y campos del
 * planificador.  Unidad minima de ejecucion dentro de la VM.
 */#ifndef PROCESO_RUNTIME_H
#define PROCESO_RUNTIME_H

#include "scheduler.h"
#include "vm_registers.h"
#include "vm_state_event.h"

#include "runtime/pid.h"
#include "gc/gc_heap.h"
#include "gc/raw_allocator.h"
#include "loader/oop_types.h"

/**
 * @brief Numero de reducciones por defecto que se le asignan a cada proceso al entrar en ejecucion.
 *
 * Una reduccion equivale a ejecutar una instruccion o un paso costoso de la FSM.
 * Cuando el contador llega a cero el scheduler cambia de proceso (round-robin cooperativo).
 */
#define reductions_remaining_default 8192

namespace runtime {
    struct InstrFormat; ///< Formato de instruccion con metadatos de descodificacion/ejecucion
    class Scheduler;    ///< Gestor de procesos que ejecuta el run_loop

    /**
     * @brief Informacion generada durante la descodificacion de una instruccion.
     *
     * Todas las fases de la FSM (DECODE -> EXECUTE) leen y escriben este struct.
     * Contiene flags de control, los operandos descodificados y un puntero al
     * descriptor de la instruccion (InstrFormat).
     */
    typedef struct DecodedInstr {
        /**
         * @brief Campos de control descodificados del prefijo de la instruccion.
         *
         * Todos los campos se inicializan a cero/false en la declaracion del objeto.
         */
        struct {
            uint8_t is_not_extended: 8; ///< opcode1: 0x00 indica tabla extendida; otro valor indica tabla primaria

            uint8_t opcode_index: 8;    ///< opcode2: indice en la tabla de descodificacion

            uint8_t _signed_instruct: 1; ///< 1 si la instruccion opera con signo; 0 si es sin signo

            /**
             * @brief Modo de la instruccion (tamanyo del operando).
             *
             * Codifica el tamanyo del operando:
             *   0 -> 8 bits, 1 -> 16 bits, 2 -> 32 bits, 3 -> 64 bits.
             */
            uint8_t mode: 2;

            /**
             * @brief Indica si la instruccion ha modificado el PC manualmente.
             *
             * Las instrucciones de salto (JMP, CALL, RET) deben poner este campo a true.
             * Si es false, la fase EXECUTE incrementa el PC en size_instr bytes al terminar.
             * Si es true, se asume que la instruccion ya actualizo el PC y no se incrementa.
             */
            bool did_jump: 1;

            bool blocking: 1; ///< 1 si la instruccion requiere esperar una operacion de E/S desbloqueante

            /**
             * @brief 1 si la instruccion usa registros extendidos/especiales (rsp, rbp, rip, cur0..cur3).
             */
            bool reg_ext: 1;

            /**
             * @brief Direccion de la operacion de memoria.
             *
             * Para instrucciones con acceso a memoria:
             *   0: destino en registro, fuente en memoria  (ej. add r3, [r1 + r2*8])
             *   1: destino en memoria, fuente en registro  (ej. add [r1 + r2*8], r3)
             *
             * En instrucciones sin direccionalidad se usa como metadato de seleccion
             * de variante dentro de la misma familia.
             */
            uint8_t direction: 1;

            uint8_t size_instr: 4; ///< Tamanyo en bytes de la instruccion (maximo 15 bytes)
        } flags_info = {
            0, 0, 0, 0,
            false, false, false, 0, 0
        }; ///< Campos de control inicializados a cero/false

        /**
         * @brief Operandos descodificados de la instruccion (union de todos los formatos).
         *
         * Solo uno de los campos de la union es valido en cada instruccion; el campo
         * activo depende del tipo de instruccion indicado por flags_info.
         */
        union {
            /**
             * @brief Acceso crudo a los 16 bytes de datos de la instruccion.
             */
            struct {
                uint64_t raw1; ///< Primeros 8 bytes en crudo
                uint64_t raw2; ///< Segundos 8 bytes en crudo
            } raw_data;

            /**
             * @brief Operandos para instrucciones de tipo reg, reg.
             *
             * Ejemplo: add r0, r1
             */
            struct {
                uint8_t reg1; ///< Indice del registro destino (o primer operando)
                uint8_t reg2; ///< Indice del registro fuente (o segundo operando)
            } reg_data;

            /**
             * @brief Operandos para instrucciones con mezcla de registros generales y especiales.
             *
             * Usado por instrucciones como XCHG que pueden operar con registros extendidos
             * y generales en la misma instruccion.  Cada registro tiene un bit de flag que
             * indica si es extendido (1) o general (0).
             */
            struct {
                uint8_t reg1      : 6; ///< Indice del registro 1 (general o extendido)
                uint8_t reg1_flags: 1; ///< 1 si reg1 es extendido/especial
                uint8_t unused1   : 1; ///< Sin uso por ahora
                uint8_t reg2      : 6; ///< Indice del registro 2 (general o extendido)
                uint8_t reg2_flags: 1; ///< 1 si reg2 es extendido/especial
                uint8_t unused2   : 1; ///< Sin uso por ahora
            } regs_data_extent;

            /**
             * @brief Operandos para instrucciones de tipo reg, imm o imm, reg.
             *
             * Ejemplo: add r0, 42
             */
            struct {
                uint64_t inmmed; ///< Valor inmediato codificado en la instruccion
                uint8_t  reg;    ///< Indice del registro destino o fuente
            } inmmed_data;

            /**
             * @brief Operandos para instrucciones de acceso a memoria con SIB.
             *
             * Codifica la formula de direccion: [reg_base + reg_index * scale].
             * El campo mode en flags_info determina el tamanyo del acceso.
             *
             * Ejemplo: add [r1 + r2 * 2], r3b
             *   reg_base=r1, reg_index=r2, scale=2, reg_final=r3
             */
            struct {
                uint8_t reg_base;  ///< Registro base de la direccion de memoria
                uint8_t reg_index; ///< Registro indice de la formula SIB
                uint8_t reg_final; ///< Registro destino o fuente de la operacion
                uint8_t scale;     ///< Factor de escala del registro indice (1, 2, 4 u 8)
            } mem_data;
        } data_instruction = {
            static_cast<uint64_t>(0),
            static_cast<uint64_t>(0)
        }; ///< Operandos inicializados a cero

        InstrFormat *metadata = nullptr; ///< Descriptor de la instruccion: funcion de ejecucion, metadatos, etc.

        uint64_t pc = 0; ///< Direccion virtual del PC donde se encontro esta instruccion
    } DecodedInstr;


    /**
     * @brief Codigos de error que puede almacenar un hilo de proceso virtual.
     *
     * El campo err_thread de ProcessVM guarda el ultimo error ocurrido.
     * Un valor THREAD_NO_ERROR (0) indica que el proceso no ha fallado.
     */
    typedef enum state_err_thread {
        THREAD_NO_ERROR          = 0, ///< Sin error
        THREAD_UNKNOWN_ERROR,         ///< Error no clasificado
        THREAD_SEGMENTATION_FAULT,    ///< Acceso a memoria sin permisos suficientes
        THREAD_ILLEGAL_INSTRUCTION,   ///< Instruccion no reconocida o prohibida
        THREAD_DIVISION_BY_ZERO,      ///< Division entre cero

        /**
         * @brief El tope de pila (SP) alcanzo el limite inferior del segmento de pila.
         *
         * Ocurre cuando se realiza un PUSH mas alla del limite reservado para la pila.
         */
        THREAD_STACK_OVERFLOW,

        /**
         * @brief Se intento leer de una pila vacia o SP quedo por debajo de BP.
         *
         * El tope de pila siempre debe ser mayor o igual que el puntero de base.
         * Un POP de pila vacia o un decremento ilegal de SP genera este error.
         */
        THREAD_STACK_UNDERFLOW,

        THREAD_INVALID_SYSCALL, ///< Llamada al sistema invalida o no soportada
        THREAD_NULL_POINTER,    ///< UNWRAP encontro un valor nulo (NullPointerException)
    } state_err_thread;

    /**
     * @brief Numero de entradas de la cache de instrucciones descodificadas.
     *
     * Debe ser potencia de dos para que icache_index() pueda usar una mascara AND.
     */
    static constexpr uint32_t ICACHE_SIZE = 1024;

    /**
     * @brief Calcula el indice en la tabla icache para una direccion PC dada.
     *
     * Usa una mascara AND (valida solo si ICACHE_SIZE es potencia de dos) para
     * mapear cualquier PC a un indice dentro del array icache[] del proceso.
     *
     * @param pc Direccion del contador de programa de la instruccion.
     * @return   Indice en el array icache[] (0 .. ICACHE_SIZE-1).
     */
    inline uint32_t icache_index(uint64_t pc) {
        return pc & (ICACHE_SIZE - 1); // mascara AND aprovechando que ICACHE_SIZE es potencia de 2
    }

    /**
     * @class ProcessVM
     * @brief Proceso virtual de la maquina virtual VestaVM.
     *
     * Cada ProcessVM representa un hilo de ejecucion independiente dentro de
     * la VM.  Contiene su propio:
     *   - Conjunto de registros (context_registers_vm).
     *   - Espacio de memoria privado (ArenaManager + VirtualMemory + TLB).
     *   - Heap de GC y asignador raw.
     *   - Cache de instrucciones descodificadas (icache).
     *   - Estado de la FSM (vm_state).
     *
     * El scheduler propietario gestiona el ciclo de vida del proceso.
     * Para que el proceso sea elegible por el scheduler debe llamarse a
     * vm->make_ready(proc->pid) tras crearlo.
     */
    class ProcessVM {
    public:
        GlobalPID pid; ///< Identificador global del proceso (scheduler_id + local_pid)

        /**
         * @brief Contador de reducciones restantes antes del proximo cambio de contexto.
         *
         * Una reduccion equivale normalmente a ejecutar una instruccion.
         * Cuando llega a cero el scheduler elige otro proceso (planificacion round-robin).
         * Se reinicia al valor reductions_remaining_default en cada quantum.
         */
        uint64_t reductions_remaining = reductions_remaining_default;

        context_registers_vm registers; ///< Contexto completo de registros del proceso

        uint64_t tsc{}; ///< Contador de instrucciones ejecutadas (Time Stamp Counter virtual)

        /**
         * @brief Marca temporal en nanosegundos a la que debe despertar el proceso.
         *
         * Cuando el proceso entra en estado BLOCK (p.ej. por una instruccion sleep),
         * este campo almacena el instante futuro en el que debe pasar a READY.
         * Un valor 0 indica que no hay temporizador activo.
         */
        uint64_t time_sleep{};

        state_err_thread err_thread = THREAD_NO_ERROR; ///< Ultimo error ocurrido en el proceso

        /**
         * @brief Estado actual del proceso dentro de la FSM del scheduler.
         *
         * Nunca debe modificarse directamente desde fuera del scheduler ya que
         * los cambios de estado son concurrentes y deben coordinarse a traves
         * de on_event().
         */
        vm_state state = NEW;

        // --- Cache de instrucciones descodificadas (icache) ---
        DecodedInstr icache[ICACHE_SIZE] = {}; ///< Tabla de instrucciones descodificadas indexada por icache_index(PC)

        DecodedInstr *decoded_ptr = nullptr; ///< Puntero a la entrada icache activa durante DECODE/EXECUTE

        // --- Memoria privada del proceso ---
        vm::ArenaManager manager_mem_priv{}; ///< Gestor de arenas privadas de este proceso

        tlb::LazyHybridTLB tlb{};           ///< TLB privado del proceso
        vm::VirtualMemory  vm_mem;           ///< Interfaz de memoria virtual (combina TLB + ArenaManager)

        // --- GC del proceso ---
        gc::GcHeap       gc_heap{manager_mem_priv, 2 * 1024 * 1024, 8 * 1024 * 1024}; ///< Heap del GC (min 2 MiB, max 8 MiB)
        gc::RawAllocator raw_alloc{};                                                   ///< Asignador raw sin GC

        // --- Sistema de objetos (OOP) ---
        loader::FrameHeader *frame_stack = nullptr; ///< Cabeza de la cadena de FrameHeaders activos (push en CALLVIRT, pop en RET/THROW)

        uint64_t current_exception = 0; ///< Handle de la excepcion activa durante el unwinding (0 = sin excepcion)

        Scheduler &scheduler; ///< Referencia al scheduler propietario de este proceso

        /**
         * @brief Construye un proceso virtual y lo asocia al scheduler indicado.
         * @param scheduler Scheduler que gestionara este proceso.
         * @param pid       Identificador global unico del proceso.
         */
        ProcessVM(Scheduler &scheduler, GlobalPID pid);

        /**
         * @brief Invalida todas las entradas del icache del proceso.
         *
         * Debe llamarse cuando el espacio de codigo del proceso cambia (p.ej.
         * tras JIT o carga dinamica de codigo) para evitar ejecutar instrucciones
         * descodificadas sobre codigo antiguo.
         */
        void reset_cache();

        /**
         * @brief Carga codigo en bruto en la memoria virtual del proceso.
         *
         * Mapea @p code a partir de @p address en el espacio de memoria del
         * proceso y pone su estado a RUNNING para que el scheduler pueda
         * comenzar a ejecutarlo.
         *
         * @param address Direccion virtual donde se escribira el codigo.
         * @param code    Vector de bytes con las instrucciones a cargar.
         */
        void load_raw_code(uint64_t address, const std::vector<uint8_t> &code);

        /**
         * @brief Genera un resumen de estado del proceso en texto.
         * @return Cadena con los valores mas relevantes del proceso.
         */
        std::string vm_summary() const;

        /**
         * @brief Destructor: libera todos los recursos del proceso.
         */
        ~ProcessVM();

        /**
         * @brief Genera una representacion textual detallada del proceso.
         * @return Cadena con el volcado completo del estado del proceso.
         */
        [[nodiscard]] std::string to_string() const;

    private:
    };

} // namespace runtime

#endif // PROCESO_RUNTIME_H
