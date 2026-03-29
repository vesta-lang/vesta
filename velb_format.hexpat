// ============================================================
//   VELB Executable Format - Compatible con ImHex 1.38.1
// ============================================================

// No se permite union → usamos bytes crudos
struct velb_magic {
    char firma[4];     // "VELB"
};

// Rango de memoria
struct range_memory {
    u64 address_init;
    u64 address_final;
};

// Header principal VELB
struct HeaderVELB {
    velb_magic magic;      // "VELB"
    u32 format_v;

    u32 max_v;
    u32 min_v;

    u64 checksum;

    u64 flags;
    u64 timestamp;
    u32 arch;

    u32 count;             // número de secciones

    u64 table_offset;      // offset a la tabla de secciones

    range_memory address_spaces[8];
};

// Sección individual
struct section_range_memory {
    range_memory address;
    char name[16];
};

// ============================================================
//   Archivo completo
// ============================================================

// Header en offset 0
HeaderVELB header @ 0;

// Tabla de secciones
section_range_memory sections[header.count] @ header.table_offset;

// Bytecode final
// ImHex 1.38.1 NO permite sizeof(), así que usamos tamaño fijo: 32 bytes por sección
u8 bytecode[] @ (header.table_offset + header.count * 32);