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
#include <vector>

#include "arena/arena.h"
#include "cli/sync_io.h"
#include "runtime/rflags.h"
#include "loader/loader.h"

namespace runtime {
    class ManageVM;

    typedef struct VM_ID {
        uint64_t id;
    } VM_ID;

    /**
     * representa los posibles estados de los hilos de la VM
     */
    typedef enum vm_state {
        /**
         * Significado: El hilo esta listo para ejecutarse, pero actualmente no esta corriendo.
         *    Cuando se usa:
         *          Despues de ser creado y preparado (inicializado su stack, IP, etc.).
         *          Despues de ceder la CPU (por ejemplo, tras un yield o cambio de contexto).
         *          Lo que implica: El planificador puede seleccionarlo para ejecutarse.
         */
        READY,

        /**
         * Significado: Es el hilo que esta actualmente ejecutandose.
         *      Cuando se usa:
         *          Al ser elegido por el planificador (por scheduler_tick() lo selecciona).
         *          Lo que implica: El hilo tiene el control de la CPU (de la VM).
         */
        RUNNING,

        /**
         * Significado: El hilo esta bloqueado, hasta que se de un tiempo determinado, ocurra
         * un evento de activacion, o sea activado por un hilo externo.
         *      Cuando se usa:
         *          Todo los hilos que se crean, pasan a estar bloqueados, hasta que se
         *          indiquen que estan listos (READY), esto supone que el hilo se alla
         *          configurado correctamente, para luego proceder a la ejcucion.
         *
         */
        BLOCKED,

        /**
         * Significado: El hilo ha terminado su ejecucion.
         *      Cuando se usa:
         *          Cuando el hilo termina su funcion (retorna).
         *          Cuando ocurre un error fatal o sale explicitamente.
         *      Lo que implica:
         *          El hilo ya no volvera a ejecutarse.
         *          Su espacio de stack puede ser liberado o reciclado.
         */
        DEAD
    } vm_state;

    static const char *vm_state_to_str(vm_state state) {
        // Ajusta según tu enum real
        switch (state) {
            case READY: return "READY";
            case RUNNING: return "RUNNING";
            case BLOCKED: return "BLOCKED";
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
        size_t capacity;
        size_t size;

        uint8_t *code;
        err_shellcode err;

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
        void * (*func)(void *arg); /** funcion nativa que llamar  */
        shellcode_t *arg; /** argumentos para la funcion, formando un shellcode */
        void *result; /** valor de retorno */
        bool finished; /** indica si la funcion fue ejecutada*/

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
        /**
         * Cada Instancia gestiona su propio memoria (memoria aislada)
         */
        vm::ArenaManager manager_mem_priv;

        /**
         * Manager de memoria "publico" del manager de instancias
         */
        vm::ArenaManager &manager_mem_public;

        /**
         * Cada instancia de loader permite manejar sus propias cargas
         */
        loader::Loader loader_priv;

        /**
         * Loader "publico" del manager de instancias.
         */
        loader::Loader &loader_public;

        vm_state state;
        pthread_t thread_for_vm{};
        VM_ID id{};

        // registros
        // -----------------------------------------------------------
        vm::MappedPtrHost stack_pointer{}; // puntero tope de la pila
        vm::MappedPtrHost base_pointer{}; // puntero base de la pila

        vm::MappedPtrHost rip{}; // puntero de instruccion
        RFlags_t flags{};

        GeneralRegister r00;
        GeneralRegister r01;
        GeneralRegister r02;
        GeneralRegister r03;
        GeneralRegister r04;
        GeneralRegister r05;
        GeneralRegister r06;
        GeneralRegister r07;
        GeneralRegister r08;
        GeneralRegister r09;
        GeneralRegister r10;
        GeneralRegister r11;
        GeneralRegister r12;
        GeneralRegister r13;
        GeneralRegister r14;
        GeneralRegister r15;

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
        state_err_thread err_thread; /**
                                             * Almcane el ultimo error ocurrido en el hilo.
                                             */

        PendingCall_t *pending_call{}; /** Llamada nativa en curso, si hay alguna */


        VM(ManageVM &mgr_vm, uint64_t id_vm);

        std::string to_string() const;

        /**
         * @brief Imprime estado completo de la VM (debug)
         */
        void print() const;
    };

    static std::string vm_summary(const runtime::VM *vm) {
        if (!vm) return "<null>";

        std::ostringstream ss;
        // ID
        ss << "ID=" << vesta::hex64(static_cast<uint64_t>(vm->id.id));

        // Estado
        ss << " st=" << vm_state_to_str(vm->state);
        ss << std::endl;

        ss << " R00=" << vesta::hex64(vm->r00.qword());
        ss << " R01=" << vesta::hex64(vm->r01.qword());
        ss << " R02=" << vesta::hex64(vm->r02.qword());
        ss << " R03=" << vesta::hex64(vm->r03.qword()); ss << std::endl;

        ss << " R04=" << vesta::hex64(vm->r04.qword());
        ss << " R05=" << vesta::hex64(vm->r05.qword());
        ss << " R06=" << vesta::hex64(vm->r06.qword());
        ss << " R07=" << vesta::hex64(vm->r07.qword()); ss << std::endl;

        ss << " R08=" << vesta::hex64(vm->r08.qword());
        ss << " R09=" << vesta::hex64(vm->r09.qword());
        ss << " R10=" << vesta::hex64(vm->r10.qword());
        ss << " R11=" << vesta::hex64(vm->r11.qword()); ss << std::endl;

        ss << " R12=" << vesta::hex64(vm->r12.qword());
        ss << " R13=" << vesta::hex64(vm->r13.qword());
        ss << " R14=" << vesta::hex64(vm->r14.qword());
        ss << " R15=" << vesta::hex64(vm->r15.qword()); ss << std::endl;

        // IP/SP/BP (usar component_to_string para capturar representación)
        ss << " IP=" << vesta::component_to_string(vm->rip);
        ss << " SP=" << vesta::component_to_string(vm->stack_pointer);
        ss << " BP=" << vesta::component_to_string(vm->base_pointer);

        // Flags compactas
        ss << " FLAGS="
                << static_cast<unsigned>(vm->flags.bits.DM)
                << static_cast<unsigned>(vm->flags.bits.CF)
                << static_cast<unsigned>(vm->flags.bits.OF)
                << static_cast<unsigned>(vm->flags.bits.SF)
                << static_cast<unsigned>(vm->flags.bits.ZF);

        // Thread / sleep
        ss << " Th=" << reinterpret_cast<void *>(vm->thread_for_vm)
                << " Sleep=" << vm->time_sleep;

        return ss.str();
    }
}

#endif //RUNTIME_H
