import std.mem;
import std.core;
import std.io;
import std.mem;
import type.magic;

// https://docs.werwolv.net/pattern-language/core-language/functions

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

struct table_spaces_address {
    range_memory address;
    char *name[] : u64 ;
    u8  *offset_bytecode[] : u64;
};



struct HeaderVELB {
    velb_magic magic;
    u32 format_v;

    u32 max_v;
    u32 min_v;

    u64 checksum; // suma de comprobacion del ejecutable

    u64 flags; // flags del ejecutable
    u64 timestamp; // fecha de tiempo en la que se genero el ejecutable
    u32 arch; // arch donde se genero el ejecutable

    u32 count; // cantidad de secciones de la tabla de secciones
    u64 table_offset; // offset a la tabla de secciones
    u64 n_spaces; // numero de direcciones de espacios
    u64 offset_section_strings; // offset a todos los strings
 
    // direccion IP incial
    u64 start_pc;

    // offset a la tabla de importacion
    u64 offset_import_table;

    // offset a la tabla de labels
    u64 offset_label_table;
    
    // cantidad de entradas en la tabla de importacion
    u32 size_import_table;
    
    // cantidad de labels en la tabla de labels
    u32 size_label_table;
    
    // el header siempre se alinea a  16 bytes
    u64 padding_;
    
    // final del header, el offset debe estar alineado o el pattern
    // dara error.
    
    table_spaces_address address_spaces[n_spaces];
 
    // Aquí calculamos el offset y tamaño del bytecode
    u64 sections_end = table_offset + count * 24;
    u64 file_size    = std::mem::size();

    // Exportamos a globales para usarlos fuera
    g_bytecode_offset = sections_end;
    g_bytecode_size   = (file_size > sections_end) ? ((offset_import_table) - sections_end) : 0;
    
    

};

struct entry_import_table {
    /**
     * Offset al nombre del modulo, Ejemplo: kernel32.dll
     */
     char *module_string[] : u32 ;
     //u32 offset_module_string;

    /**
     * offset al nombre del metodo, Ejemplo: GetTickCount()
     */
    //u32 offset_function_string;
    char *function_string[] : u32 ;
    
    /**
     * La firma sirve para describir los tipos de argumentos y el tipo de retorno de la función nativa.
     *      - (i32,i32)->i32
     *      - (f64)->f64
     *      - (str)->void
     *      - (ptr,i32)->i32
     */
    u32 offset_signature_string;

    /**
     * offset de la instruccion a parchear.
     */
    u32 offset_bytecode;
    //u64 *offset_bytecode[1] : u32 ;
};

struct section_range_memory {
    range_memory address;
    char *name[] : u64 ;
};

// Struct para el bytecode
struct Bytecode {
    u8 data[g_bytecode_size];
};

// ============================================================
//   Archivo completo
// ============================================================

HeaderVELB header @ 0;

//u8 *section_strings[table_spaces_address] : u64 @ 0x40;

// Tabla de secciones
section_range_memory sections[header.count] @ header.table_offset;

// Bytecode final
Bytecode bytecode @ g_bytecode_offset;

// tabal de importacion, calculamos el tamaño de la tabla de importacion que es:
// tamaño del archivo (std::mem::size()) - offset final del bytecode = 
// inicio del bytecode + tamaño del bytecode.
// esta operacion nos devuelve el tamaño de la tabla de importacion, se divide entre 16 que es
// el tamaño de cada entrada y eso nos da el numero de entradas de la tabla.
// luego usamos  @ header.offset_import_table para indicar cual es el offset de incio de la tabla de
// importacion
entry_import_table import_table[
    (std::mem::size() - (g_bytecode_offset + g_bytecode_size)) / 16
] @ header.offset_import_table;

std::print("tamaño del bytecode: {}",g_bytecode_size);
std::print("tamaño del archivo: {}",  (std::mem::size())); 
std::print("cantidad de entradas en la tabla de importacion: {}", 
    (std::mem::size() - (g_bytecode_offset + g_bytecode_size)) / 16); 
    
for (auto i = 0, i < std::core::member_count(import_table), i += 1) {
    import_table[i].offset_bytecode = import_table[i].offset_bytecode + g_bytecode_offset;
    std::print("{}", import_table[i].offset_bytecode);
}