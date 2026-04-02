#ifndef LOADER_H
#define LOADER_H

#include "runtime/vm_address_space.h"

#include <cstdint>   // uint8_t, uint32_t
#include <cstddef>   // size_t

#include "arena/arena_manager.h"

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

    class Loader {
    public:
        vm::ArenaManager arena_manager;

        explicit Loader(const vm::ArenaManager &arena_manager);
    };
}

#endif
