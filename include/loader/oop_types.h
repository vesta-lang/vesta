/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (c) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 */

/**
 * @file oop_types.h
 * @brief Estructuras de metadatos del sistema de objetos de VestaVM.
 *
 * Este header es intencionalmente ligero (solo <cstdint> y <cstddef>) para
 * poder incluirse desde proceso_runtime.h sin crear dependencias circulares
 * con loader.h -> runtime.h -> proceso_runtime.h.
 */

#ifndef OOP_TYPES_H
#define OOP_TYPES_H

#include <cstdint>
#include <cstddef>

namespace loader {

    // -------------------------------------------------------------------------
    //  Flags de objeto en memoria (ObjectHeader::flags)
    // -------------------------------------------------------------------------
    static constexpr uint32_t OBJ_FLAG_GC_OWNED  = (1u << 0); ///< gestionado por GcHeap
    static constexpr uint32_t OBJ_FLAG_RAW_OWNED = (1u << 1); ///< gestionado por RawAllocator
    static constexpr uint32_t OBJ_FLAG_LOCKED    = (1u << 2); ///< monitor bloqueado

    // -------------------------------------------------------------------------
    //  Flags de clase (ClassInfo::flags)
    //
    //  bits 0-1 : visibilidad  (0=default, 1=public, 2=private, 3=protected)
    //  bit  2   : is_abstract
    //  bit  3   : is_interface
    //  bit  4   : is_final
    //  bit  5   : is_enum
    //  bit  6   : is_static
    //  bit  7   : is_exception  (hereda de Throwable -> habilita throw/catch)
    //  bit  8   : is_native     (implementada en C/C++ del runtime)
    // -------------------------------------------------------------------------
    static constexpr uint64_t CLASS_VIS_DEFAULT   = 0x0ULL;
    static constexpr uint64_t CLASS_VIS_PUBLIC    = 0x1ULL;
    static constexpr uint64_t CLASS_VIS_PRIVATE   = 0x2ULL;
    static constexpr uint64_t CLASS_VIS_PROTECTED = 0x3ULL;
    static constexpr uint64_t CLASS_FLAG_ABSTRACT  = (1ULL << 2);
    static constexpr uint64_t CLASS_FLAG_INTERFACE = (1ULL << 3);
    static constexpr uint64_t CLASS_FLAG_FINAL     = (1ULL << 4);
    static constexpr uint64_t CLASS_FLAG_ENUM      = (1ULL << 5);
    static constexpr uint64_t CLASS_FLAG_STATIC    = (1ULL << 6);
    static constexpr uint64_t CLASS_FLAG_EXCEPTION = (1ULL << 7);
    static constexpr uint64_t CLASS_FLAG_NATIVE    = (1ULL << 8);

    // -------------------------------------------------------------------------
    //  Flags de metodo (MethodInfo::flags)
    //
    //  bits 0-1 : visibilidad  (mismos valores que CLASS_VIS_*)
    //  bit  2   : is_static
    //  bit  3   : is_abstract
    //  bit  4   : is_final
    //  bit  8   : is_native
    //  bit  9   : is_constructor
    //  bit  10  : is_virtual
    //  bit  11  : is_override
    //  bit  12  : is_synchronized
    // -------------------------------------------------------------------------
    static constexpr uint64_t METHOD_FLAG_STATIC       = (1ULL << 2);
    static constexpr uint64_t METHOD_FLAG_ABSTRACT     = (1ULL << 3);
    static constexpr uint64_t METHOD_FLAG_FINAL        = (1ULL << 4);
    static constexpr uint64_t METHOD_FLAG_NATIVE       = (1ULL << 8);
    static constexpr uint64_t METHOD_FLAG_CONSTRUCTOR  = (1ULL << 9);
    static constexpr uint64_t METHOD_FLAG_VIRTUAL      = (1ULL << 10);
    static constexpr uint64_t METHOD_FLAG_OVERRIDE     = (1ULL << 11);
    static constexpr uint64_t METHOD_FLAG_SYNCHRONIZED = (1ULL << 12);

    // -------------------------------------------------------------------------
    //  Modificadores de acceso de campo
    // -------------------------------------------------------------------------
    typedef enum FieldAccess {
        FIELD_PUBLIC,
        FIELD_PRIVATE,
        FIELD_PROTECTED,
        FIELD_DEFAULT
    } FieldAccess;

    // -------------------------------------------------------------------------
    //  Tipos de campo
    // -------------------------------------------------------------------------
    typedef enum FieldKind {
        FIELD_PRIMITIVE,
        FIELD_CLASS,
        FIELD_STRUCT,
        FIELD_TYPEDEF,
        FIELD_ENUM,
        FIELD_ASPECT
    } FieldKind;

    // -------------------------------------------------------------------------
    //  String ligero (sin heap de C++)
    // -------------------------------------------------------------------------
    typedef struct stringx {
        uint8_t *data;
        uint32_t size;
    } stringx;

    // Declaraciones adelantadas
    struct ClassInfo;
    struct MethodInfo;
    struct FieldInfo;

    // -------------------------------------------------------------------------
    //  AttrEntry - par clave/valor para anotaciones arbitrarias
    //
    //  Permite adjuntar metadatos extensibles a ClassInfo, MethodInfo y FieldInfo:
    //    @Deprecated, @Since, @Warning, @Author, @See, etc.
    //
    //  sizeof(AttrEntry) == 32 bytes (dos stringx de 12B c/u + padding de alineacion)
    // -------------------------------------------------------------------------
    typedef struct AttrEntry {
        stringx key;    ///< nombre de la anotacion  ("Deprecated", "Since", ...)
        stringx value;  ///< valor de la anotacion   ("true", "1.4", ...)
    } AttrEntry;

    // -------------------------------------------------------------------------
    //  FieldInfo - descripcion de un campo de instancia o estatico
    // -------------------------------------------------------------------------
    typedef struct FieldInfo {
        stringx     name;
        FieldAccess access;
        FieldKind   kind;
        ClassInfo  *type_class; ///< si kind == FIELD_CLASS o FIELD_STRUCT
        uint32_t    size;       ///< bytes que ocupa el campo
        uint32_t    offset;     ///< offset dentro del payload del objeto
        bool        is_static;
        // --- reflexion/documentacion (offsets 48-79) ---
        stringx     doc;        ///< docstring del campo
        AttrEntry  *attrs;      ///< tabla de anotaciones clave/valor
        size_t      attr_count;
    } FieldInfo;

    // -------------------------------------------------------------------------
    //  HandlerException - entrada de la tabla de handlers de un metodo
    // -------------------------------------------------------------------------
    typedef struct HandlerException {
        ClassInfo *type;       ///< clase de la excepcion (nullptr = catch-all)
        uint32_t   start_pc;   ///< offset inicio del rango try (relativo a code_vaddr)
        uint32_t   end_pc;     ///< offset fin del rango try (relativo a code_vaddr)
        uint32_t   handler_pc; ///< offset del bloque catch (relativo a code_vaddr)
    } HandlerException;

    // -------------------------------------------------------------------------
    //  MethodInfo - metadatos de un metodo
    // -------------------------------------------------------------------------
    typedef struct MethodInfo {
        stringx           name;
        stringx           descriptor;   ///< firma legible "(int,String)int"
        uint64_t          flags;        ///< METHOD_FLAG_*
        ClassInfo        *owner_class;  ///< clase a la que pertenece

        FieldInfo        *args;         ///< descripcion de parametros (reflexion)
        size_t            arg_count;
        FieldInfo        *return_type;  ///< tipo de retorno (nullptr = void)

        uint64_t          code_vaddr;   ///< direccion virtual del inicio del bytecode
        size_t            code_size;    ///< bytes de bytecode

        HandlerException *handlers;
        size_t            handler_count;

        void             *jit_code;     ///< nullptr si no esta compilado JIT
        // --- reflexion/documentacion (offsets 112-143) ---
        stringx           doc;          ///< docstring del metodo
        AttrEntry        *attrs;        ///< tabla de anotaciones clave/valor
        size_t            attr_count;
    } MethodInfo;

    // -------------------------------------------------------------------------
    //  ClassInfo - descriptor completo de una clase
    // -------------------------------------------------------------------------
    typedef struct ClassInfo {
        // --- identidad ---
        stringx    name;
        uint64_t   flags;          ///< CLASS_FLAG_*
        uint32_t   instance_size;  ///< bytes totales del objeto (incluye ObjectHeader)

        // --- jerarquia ---
        ClassInfo **supers;
        size_t      super_count;
        ClassInfo **interfaces;
        size_t      interface_count;

        // --- campos de instancia ---
        FieldInfo  *fields;
        size_t      field_count;

        // --- vtable ---
        MethodInfo **vtable;
        size_t       vtable_size;

        // --- campos estaticos ---
        uint8_t    *static_data;
        FieldInfo  *static_fields;
        size_t      static_field_count;

        // --- todos los metodos (para reflexion) ---
        MethodInfo *methods;
        size_t      method_count;
        // --- reflexion/documentacion (offsets 136-167) ---
        stringx     doc;        ///< docstring de la clase
        AttrEntry  *attrs;      ///< tabla de anotaciones clave/valor
        size_t      attr_count;
    } ClassInfo;

    // -------------------------------------------------------------------------
    //  ObjectHeader - cabecera que precede al payload de todo objeto
    //
    //  Layout en memoria:
    //    [GcHeader (8B)]      <- solo en objetos GC (antes del payload)
    //    [ObjectHeader (16B)] <- inicio del payload; aqui apunta GcHeap::deref()
    //    [campos del objeto]
    // -------------------------------------------------------------------------
    struct alignas(8) ObjectHeader {
        ClassInfo *class_ptr;  ///< 8 bytes - puntero a los metadatos de la clase
        uint32_t   flags;      ///< 4 bytes - OBJ_FLAG_*
        uint32_t   hash_code;  ///< 4 bytes - identidad del objeto (lazy)
    };
    static_assert(sizeof(ObjectHeader) == 16,
                  "ObjectHeader debe medir exactamente 16 bytes");

    // -------------------------------------------------------------------------
    //  FrameHeader - frame en la cadena de llamadas (para throw/catch)
    // -------------------------------------------------------------------------
    typedef struct FrameHeader {
        FrameHeader *prev;        ///< frame del llamante (linked list)
        MethodInfo  *method;      ///< metodo en ejecucion
        uint64_t     return_pc;   ///< PC virtual al que volver al hacer ret
        uint64_t     frame_base;  ///< SP en el momento de la llamada
    } FrameHeader;

} // namespace loader

#endif // OOP_TYPES_H
