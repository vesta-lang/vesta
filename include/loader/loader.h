#ifndef LOADER_H
#define LOADER_H

#include "runtime/vm_address_space.h"

#include <cstdint>   // uint8_t, uint32_t
#include <cstddef>   // size_t

#include "arena/arena_manager.h"
#include "emmit/struct_context.h"
#include "runtime/manager_runtime.h"

namespace loader {
    /**
     * Modificadores de acceso
     */
    typedef enum FieldAccess {
        FIELD_PUBLIC,
        FIELD_PRIVATE,
        FIELD_PROTECTED,
        FIELD_DEFAULT
    } FieldAccess;

    /**
     * Tipos de campo
     * (pueden ser primitivos o clases definidas por el usuario,
     * estructuras, tipos de datos (typedef), o enumeraciones)
     */
    typedef enum FieldKind {
        FIELD_PRIMITIVE,
        FIELD_CLASS,
        FIELD_STRUCT,
        FIELD_TYPEDEF,
        FIELD_ENUM,
        FIELD_ASPECT
    } FieldKind;

    /**
     * representacion de un string simple
     */
    typedef struct stringx {
        uint8_t *data;
        uint32_t size;
    } stringx;

    struct ClassInfo; // forward declaration

    /**
     * Información de un campo
     */
    typedef struct FieldInfo {
        stringx name; // nombre del campo
        FieldAccess access; // public/private/protected/default
        FieldKind kind; // tipo de dato
        ClassInfo *type_class; // si es FIELD_CLASS o FIELD_STRUCT
        uint32_t size; // tamaño en bytes
        uint32_t offset; // offset dentro del objeto
        bool is_static; // si es un campo estático
    } FieldInfo;

    /**
     * Manejador de excepciones
     */
    typedef struct HandlerException {
        ClassInfo *type; // null = catch-all
        uint32_t start_pc;
        uint32_t end_pc;
        uint32_t handler_pc;
    } HandlerException;

    /**
     * Informacion del metodo
     */
    typedef struct MethodInfo {
        stringx name;
        HandlerException *handlers;
        size_t handler_count;
        uint8_t *code; // puntero al bytecode
        // aquí irían más cosas: num locals, tamaño de operand stack, etc.
    } MethodInfo;

    // Header de frame en la pila (en memoria de la VM)?
    typedef struct FrameHeader {
        FrameHeader *prev; // frame anterior (caller)
        MethodInfo *method; // metodo actual
        uint32_t return_pc; // PC al que volver si se hace return
        // después de esto, en memoria, irían locals, operand stack, etc.
    } FrameHeader;

    /**
     * @struct Executable
     * @brief Representa un ejecutable VELB completamente enlazado y listo para cargar en la VM.
     *
     * Esta estructura es el resultado final del linker y la entrada principal del loader.
     * Contiene toda la información necesaria para:
     *  - reservar espacios de memoria
     *  - copiar secciones ensambladas
     *  - resolver símbolos y labels
     *  - inicializar el PC
     *  - cargar metadatos y relocaciones
     *
     * Es equivalente a un "ELF ejecutable" pero adaptado al formato VELB.
     */
    typedef struct Executable {
        /**
         * @brief Identificador del formato del ejecutable.
         *
         * Siempre debe ser `"velb"`. Permite validar que el archivo cargado
         * corresponde al formato correcto.
         */
        std::string format = "velb";

        /**
         * @brief Versión del formato VELB.
         *
         * Permite compatibilidad futura entre versiones del loader y del linker.
         */
        velb_version_format version = 1;

        /**
         * @brief Espacios de direcciones definidos en el ejecutable.
         *
         * Cada `Space` representa un rango de direcciones virtuales:
         *  - `anonymous`
         *  - `stack`
         *  - `heap`
         *  - `data`
         *  - etc.
         *
         * El loader debe reservar memoria para cada uno. Se puede indicar si reservar el rango completo o
         * realizar reserva lazy.
         */
        std::vector<Assembly::Bytecode::Space> spaces;

        /**
         * @brief Secciones ensambladas del ejecutable.
         *
         * Cada `Section` contiene bytecode ya resuelto y listo para copiarse
         * en el espacio correspondiente. Equivalente a `.text`, `.data`, `.rodata` en ELF.
         */
        std::vector<Assembly::Bytecode::Section> sections;

        /**
         * @brief Símbolos globales con dirección absoluta.
         *
         * Cada `Label` contiene:
         *  - nombre del símbolo
         *  - dirección absoluta final tras el linking
         *
         * El loader los registra en la tabla de símbolos de la VM.
         */
        std::vector<Assembly::Bytecode::Label> labels;

        /**
         * @brief Bytecode final concatenado.
         *
         * Opcional: algunos loaders prefieren trabajar con `sections`,
         * otros con un buffer plano. Se mantiene por compatibilidad.
         */
        std::vector<uint8_t> bytecode;

        /**
         * @brief Dirección inicial del PC.
         *
         * Determinada por la directiva `@InitPc` o por el linker.
         * El loader debe asignarla al registro PC de la VM.
         */
        uint64_t init_pc = 0;

        /**
         * @brief Cabecera del ejecutable VELB.
         *
         * Contiene información adicional como:
         *  - tamaño del ejecutable
         *  - checksum
         *  - flags
         *  - versión del linker
         *  - punto de entrada
         *
         * Es redundante con algunos campos, pero útil para validación.
         */
        HeaderVELB header;

        /**
         * @brief Relocaciones aplicadas durante el linking.
         *
         * Normalmente solo se usa para debugging o herramientas de análisis.
         * El ejecutable final ya tiene todas las direcciones resueltas. En teoria, aunque
         * se puede llamar al linker dinamico.
         */
        std::vector<Assembly::Bytecode::Relocation> relocations;

        /**
         * @brief Metadatos arbitrarios en formato JSON.
         *
         * Puede incluir:
         *  - autor
         *  - timestamp
         *  - flags de compilación
         *  - información de build
         *  - dependencias
         */
        //Sqlite::json metadata;

        // Opciones del linker
        // Assembly::Bytecode::Linker::LinkerOptions options; ///< Opciones usadas para generar este ejecutable
    } Executable;

    class Loader {
    public:
        runtime::ManageVM &instance_manager;

        explicit Loader(
            runtime::ManageVM &instance_manager);

        void resolve_labels(Assembly::Bytecode::Section &section);

        void load_sections(Assembly::Bytecode::Label &label);

        void load_spaces(Assembly::Bytecode::Space &space);

        void build_runtime_context();

        void create_vm_instance();
    };
}

#endif
