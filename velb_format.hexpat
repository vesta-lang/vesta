import std.mem;

// ============================================================
//   VELB Executable Format - Compatible con ImHex 1.38.1
// ============================================================

// Variables globales para el bytecode
u64 g_bytecode_offset;
u64 g_bytecode_size;

struct velb_magic {
    char firma[4];     // "VELB"
};

struct range_memory {
    u64 address_init;
    u64 address_final;
};

struct HeaderVELB {
    velb_magic magic;
    u32 format_v;

    u32 max_v;
    u32 min_v;

    u64 checksum;

    u64 flags;
    u64 timestamp;
    u32 arch;

    u32 count;
    u64 table_offset;
    u64 n_spaces;

    range_memory address_spaces[n_spaces];

    // Aquí calculamos el offset y tamaño del bytecode
    u64 sections_end = table_offset + count * 32;
    u64 file_size    = std::mem::size();

    // Exportamos a globales para usarlos fuera
    g_bytecode_offset = sections_end;
    g_bytecode_size   = (file_size > sections_end) ? (file_size - sections_end) : 0;
};

struct section_range_memory {
    range_memory address;
    char name[16];
};

// Struct para el bytecode
struct Bytecode {
    u8 data[g_bytecode_size];
};

// ============================================================
//   Archivo completo
// ============================================================

HeaderVELB header @ 0;

// Tabla de secciones
section_range_memory sections[header.count] @ header.table_offset;

// Bytecode final
Bytecode bytecode @ g_bytecode_offset;