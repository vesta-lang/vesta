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

#ifndef VELB_LINKER_BYTECODE_H
#define VELB_LINKER_BYTECODE_H

/*
 * debemos usar "pack" para evitar el padding adicional de los compiladores
 * y que sea una estructura segura para serializar.
 */
#if defined(_MSC_VER)
#   pragma pack(push, 1)
#elif defined(__GNUC__)
#   define PACKED __attribute__((packed))
#else
#   define PACKED
#endif

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


#define MAGIC_NUMBER_VELB 0x424C4556

/**
 * A traves del numero magico, se puede conocer el endian de la maquina que
 * genero el bytecode. ENçn caso de ser little‑endian (LE) en el ejecutable pondra
 * "VELB", pero en caso de ser big-endian (BE), pondra BLEV
 */
typedef union velb_magic {
    uint32_t firma; // 0x424C4556 == "VELB" en LE
    uint8_t firma_byte[4];
} velb_magic;

typedef uint32_t velb_version_format;

typedef uint32_t max_vm_version;
typedef uint32_t min_vm_version;

typedef uint64_t velb_checksum;

typedef uint64_t velb_flags;
typedef uint64_t build_timestamp;
typedef uint32_t target_arch;

typedef uint32_t section_count;

typedef uint64_t section_table_offset;

typedef struct PACKED range_memory {
    uint64_t address_init;
    uint64_t address_final;
} range_memory;

typedef struct PACKED HeaderVELB {
    velb_magic magic;
    velb_version_format format_v;

    max_vm_version max_v;
    min_vm_version min_v;

    velb_checksum checksum;

    velb_flags flags;
    build_timestamp timestamp;
    target_arch arch;

    section_count count;

    section_table_offset table_offset;

    range_memory address_spaces[8]; // tabla de espacios de direcciones
} HeaderVELB;

typedef struct PACKED name_section {
    union {
        struct {
            uint64_t hi;
            uint64_t lo;
        } as_u64;

        char name[16];
    };
} name_section;

typedef struct PACKED section_range_memory {
    range_memory address;

    // nombre de la seccion, maximo 16 bytes.
    name_section name;
} section_range_memory;

#ifdef __cplusplus
}
#endif

#endif //VELB_LINKER_BYTECODE_H
