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

#include "emmit/bytewriter.h"


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

/**
 * ``4 bytes`` para la versión del formato.
 */
typedef uint32_t velb_version_format;

/**
 * ``4 bytes`` con la máxima versión compatible en la
 * que el código puede ser ejecutado en una VM.
 */
typedef uint32_t max_vm_version;

/**
 *  ``4 bytes`` con la mínima versión
 *  compatible en la que el código puede ser ejecutado en una VM.
 */
typedef uint32_t min_vm_version;

/**
 * ``8 bytes`` para indicar un checksum del código, sino
 * coincide, no se ejecuta.
 */
typedef uint64_t velb_checksum;

/**
 * para indicar características del ejecutable
 * (meta-información adicional del ejecutable).
 */
typedef uint64_t velb_flags;

/**
 * ``8 bytes`` de marca de tiempo para indicar la fecha de compilación.
 */
typedef uint64_t build_timestamp;

/**
 * ``4 bytes`` para indicar la
 * arquitectura en la que se realizo la compilación del ejecutable.
 */
typedef uint32_t target_arch;

/**
 * ``4 bytes`` para el numero de secciones en la tabla de secciones.
 */
typedef uint32_t section_count;

/**
 * ``8 bytes`` de desplazamiento respecto a la
 * dirección base para indicar la tabla de secciones.
 */
typedef uint64_t section_table_offset;

/**
 * 8 bytes para indicar la cantidad de espacio de
 * direcciones de la tabla que viene a continuación.
 */
typedef uint64_t number_spaces_address;

typedef struct PACKED range_memory {
    uint64_t address_init;
    uint64_t address_final;
} range_memory;

typedef struct PACKED table_spaces_address {
    range_memory address; // 16 bytes

    // contiene el offset a la tabla de strings,
    // en caso de estar el ejecutable cargado en la VM,
    // este campo contendra la direccion virtual con el offset
    // aplicado.
    uint64_t offset_section_strings;
    uint64_t padding;
} table_spaces_address;

typedef struct PACKED HeaderVELB {
    velb_magic magic; // 4, 4
    velb_version_format format_v; // 4, 8

    max_vm_version max_v; // 4, 12
    min_vm_version min_v; // 4, 16

    velb_checksum checksum; // 8, 24

    velb_flags flags; // 8, 32
    build_timestamp timestamp; // 8, 40
    target_arch arch; // 4, 44

    section_count count; // 4, 48

    section_table_offset table_offset; // 8, 56

    number_spaces_address n_spaces; // 8, 64

    // seccion con cadenas espaciales que pone el linker.
    // "code\0data\0stack\0main\0foo\0bar\0"
    uint64_t offset_section_strings; // 8, 72
    uint64_t start_pc; // 8, 80

    table_spaces_address *address_spaces; // tabla de espacios de direcciones
} HeaderVELB;

typedef struct PACKED HeaderVELA {
    char magic[4]; // "VELA"
    uint32_t version; // versión del formato
    uint32_t module_count; // número de módulos
    uint64_t module_table_offset;
} HeaderVELA;

typedef struct PACKED section_range_memory {
    range_memory address;   // aqui deberia contener offsets a los
                            // espacios de direcciones, al cargar el ejecutable,
                            // se indica la direccion real,

    // nombre de la seccion, maximo 16 bytes.
    uint64_t offset_string; // donde empieza el bytecode o los datos dentro del archivo
} section_range_memory;

typedef struct PACKED VELA_ModuleEntry {
    uint64_t offset; // offset al módulo
    uint64_t size; // tamaño del módulo
    uint32_t symbol_count;
    uint64_t symbol_table_offset;
    uint32_t relocation_count;
    uint64_t relocation_table_offset;
} VELA_ModuleEntry;

typedef struct PACKED VELA_Symbol {
    uint64_t offset; // offset dentro del módulo
    uint8_t type; // FUNC, DATA, GLOBAL, LOCAL
    char name[32];
} VELA_Symbol;

typedef struct PACKED VELA_Relocation {
    uint64_t offset; // dónde aplicar la relocación
    uint8_t type; // ABS64, REL32, REL64
    char symbol[32]; // símbolo a resolver
} VELA_Relocation;

/**
 * Nos permite comprobar si dos rangos de memoria se solapan ("se pisan")
 * @param a rango de memoria 1
 * @param b rango de memoria 2
 * @return si hay solapacion entonces devuelve true.
 */
static bool ranges_overlap(const range_memory *a, const range_memory *b) {
    return !(a->address_final <= b->address_init ||
             b->address_final <= a->address_init);
}


#ifdef __cplusplus
}
#endif


//#ifdef __cplusplus
// Código exclusivo de C++ (clases, métodos, namespaces, etc.)

#include <unordered_map>
#include <vector>
#include "emmit/struct_context.h"
#include "optimizer/optimizer.h"
#include "emmit/annotations.h"

namespace Assembly::Bytecode::Linker {
    /**
     * Opciones que controlan el comportamiento del linker.
     */
    struct LinkerOptions {
        bool optimize_bytecode = true; // aplicar optimizaciones
        bool allow_undefined_symbols = false;
        bool generate_map_file = false; // archivo .map opcional
        bool verbose = false; // logs detallados

        std::string output_path; // ruta del ejecutable final
        std::string map_file_path; // donde generar el map
    };

    /**
     * Errores estructurados
     */
    struct LinkerError {
        enum class Type {
            FileNotFound,
            InvalidFormat,
            DuplicateSymbol,
            UndefinedSymbol,
            RelocationError,
            IOError,
            InternalError
        };

        Type type;
        std::string message;
    };


    /**
     * Un informe final con estadísticas del linking.
     */
    struct LinkerReport {
        size_t modules_linked = 0;
        size_t symbols_resolved = 0;
        size_t relocations_applied = 0;
        size_t optimizations_applied = 0;

        std::vector<std::string> warnings;
        std::vector<std::string> errors;

        bool success() const { return errors.empty(); }
    };

    /**
     * Una librería estática es simplemente un contenedor de módulos.
     */
    struct StaticLibrary {
        std::string path;
        std::vector<std::vector<uint8_t> > object_modules;
    };


    /**
     * Cada módulo puede tener relocaciones pendientes:
     */
    struct Relocation {
        uint64_t offset; // dónde aplicar la relocación
        std::string symbol; // símbolo a resolver
        enum class Type {
            Absolute64,
            Relative32,
            Relative64
        } type;
    };


    /**
     * Cada Module tiene su contexto, su bytecode, sys simbolos, sus relocalizaciones.
     */
    struct Module {
        std::string name;
        std::vector<uint8_t> bytecode;
        Context *ctx;
        bool is_object = false;
        std::vector<Relocation> relocations;
        std::unordered_map<std::string, uint64_t> local_symbols;
    };

    /**
     * Tabla para simbolos, los simbolos globales usan otra tabla aparte
     */
    struct SymbolInfo {
        std::string space;
        std::string section;
        uint64_t relative; // offset dentro de la sección
        uint64_t absolute; // dirección virtual absoluta
        uint64_t file_offset; // se rellena después en build_section_table()
        std::string module; // módulo donde se definió
        uint64_t size; // tamaño del label, algunas no tienen
    };

    typedef struct section_info_linker{
        section_range_memory memory;
        std::string name;
    } section_info_linker;

    /**
     * clase Linker va a cumplir dos roles muy distintos:
     *
     * - Linker tradicional
     *      Recibe varios ejecutables parciales (objetos, módulos, librerías) y
     *      resuelve símbolos, funciones externas, imports, etc.
     *
     * - Builder de ejecutables VELB
     *      Recibe un bytecode crudo + contexto generado por
     *      ensamblador y produce un ejecutable final VELB.
     *
     * Linker tiene 3 fases:
     *      1. Entrada
     *          - Cargar módulos objeto (.velo)
     *          - Cargar librerías estáticas (.vela)
     *          - Cargar ensamblados crudos (bytecode + contexto)
     *
     *      2. Proceso
     *          - Resolver símbolos
     *          - Seleccionar módulos necesarios
     *          - Aplicar relocaciones
     *          - Optimizar bytecode
     *          - Fusionar secciones y espacios de direcciones
     *
     *      3. Salida
     *          - Construir ejecutable VELB
     *          - Escribir archivo final
     *          - Escribir archivo .velb-map
     *
     */
    class Linker {
    public:
        explicit Linker(const LinkerOptions &opts = {})
            : options(opts) {
            this->result = new ByteWriter;
        }

        /**
         * Carga un archivo .velo o .velb parcial.
         *  Responsabilidades
         *      - Abrir archivo
         *      - Validar formato
         *      - Leer header
         *      - Leer secciones
         *      - Leer tabla de símbolos
         *      - Leer relocaciones
         *      - Convertirlo en un Module
         *      - Añadirlo a modules
         *
         *  Errores posibles
         *      - Archivo no encontrado
         *      - Formato inválido
         *      - Símbolos duplicados dentro del módulo
         *
         * @param path path donde se encuentra el archivo
         */
        void add_object_file(const std::string &path);

        /**
         * Igual que add_object_file pero desde memoria.
         *      Responsabilidades
         *          - Interpretar el buffer como un módulo objeto
         *          - Validar header
         *          - Extraer símbolos y relocaciones
         *          - Añadir a modules
         *
         * @param data buffer del archivo
         */
        void add_object_memory(const std::vector<uint8_t> &data);

        void merge_space_address(Space &dest, const Space &src);

        /**
         * Recibe el resultado del ensamblador.
         * Responsabilidades:
         *      Crear un Module con:
         *         - bytecode crudo
         *         - contexto (secciones, labels, espacios)
         *         - símbolos locales
         *         - relocaciones generadas por el ensamblador
         *     Marcarlo como is_object = false (es un ensamblado directo)
         * @param bytecode
         * @param ctx
         */
        void add_assembly_unit(const std::vector<uint8_t> &bytecode,
                               const Assembly::Bytecode::Context *ctx);

        /**
          * Carga un archivo .vela.
          * Responsabilidades
          *     - Leer header VELA
          *     - Leer tabla de módulos
          *     - NO cargar todos los módulos todavía
          *     - Guardar la librería en libraries
          *     - Los módulos se cargarán solo si se necesitan para resolver símbolos
          *
          * Comportamiento tipo ar/ld
          *     - Si un símbolo requerido está en la librería -> se extrae ese módulo
          *     - Si no -> se ignora
         * @param path archivo vela a cargar
         */
        void add_static_library(const std::string &path);

        void merge_section(Section &dest, const Section &src);

        /**
         * Responsabilidades
         *     - Construir la tabla global de símbolos
         *     - Detectar símbolos duplicados
         *     - Detectar símbolos indefinidos
         *     - Extraer módulos necesarios de librerías .vela
         *     - Repetir hasta que no falten símbolos
         *
         * Algoritmo (como ld/LLD)
         *     - Insertar símbolos de módulos ya cargados
         *     - Mientras existan símbolos indefinidos:
         *         * Buscar en librerías .vela
         *         * Si un módulo contiene el símbolo -> cargarlo
         *         * Añadir sus símbolos
         *
         *     - Si al final quedan símbolos indefinidos:
         *         * Error (a menos que allow_undefined_symbols = true)
         */
        void resolve_symbols();

        /**
         * Aplica las relocaciones de cada módulo.
         *
         * Responsabilidades
         *     Para cada relocación:
         *         - Buscar el símbolo en global_symbols
         *         - Calcular la dirección final
         *         - Escribir el valor en el bytecode
         *
         *     Tipos soportados:
         *         - ABS64
         *         - REL32
         *         - REL64
         *
         * Errores
         *     Símbolo no encontrado
         *     Offset fuera de rango
         */
        void apply_relocations();

        /**
         * Si options.optimize_bytecode == true.
         * Responsabilidades
         *     - Eliminar NOPs
         *     - Fusionar instrucciones triviales
         *     - Eliminar bloques muertos
         *     - Compactar saltos
         *     - Reordenar instrucciones si es seguro
         *
         * Resultado
         *     - Bytecode más pequeño
         *     - Mejor rendimiento en la VM
         */
        void optimize_modules();

        /**
         * Orquesta todo el proceso.
         * Responsabilidades
         *     - resolve_symbols()
         *     - apply_relocations()
         *     - optimize_modules()
         *     - merge_address_spaces()
         *     - merge_sections()
         *     - build_header()
         *     - build_section_table()
         *     - concatenar bytecode final
         *
         * @return Devuelve un std::vector<uint8_t> con el ejecutable completo.
         */
        std::vector<uint8_t> build_executable();

        /**
         * Escribe el ejecutable final en disco.
         * @param path donde guardar el ejecutable
         */
        void write_to_file(const std::string &path);

        /**
         * Permite generar un archivo velb-map:
         * Y dentro:
         *   - listar módulos incluidos
         *   - listar símbolos globales
         *   - listar relocaciones aplicadas
         *   - listar secciones finales
         *   - listar espacios de direcciones
         *   - listar optimizaciones aplicadas
         *   - Escribir tamaño final del ejecutable
         * @param path
         */
        void write_map_file(const std::string &path);

        const LinkerReport &get_report() const { return report; }

        void log_warning(const std::string &msg);

        void log_error(const std::string &msg);

    private:
        LinkerOptions options;
        LinkerReport report;

        // para construir el binario final, buffer de salida
        ByteWriter *result;

        // Interno: lista de módulos cargados
        struct Module {
            std::string name;
            std::vector<uint8_t> bytecode;
            Context ctx;
            bool is_object = false;

            std::vector<Relocation> relocations;
            std::unordered_map<std::string, uint64_t> local_symbols;
        };

        std::vector<Module> modules;
        std::vector<StaticLibrary> libraries;

        /**
         * Simbolos globales
         */
        std::unordered_map<std::string, uint64_t> global_symbols;

        /**
         * Simbolos no globales
         */
        std::unordered_map<std::string, SymbolInfo> symbol_info;

        /**
         * bytecode final que plasmar, esto esta generado por una unidad de ensamblado
         */
        std::vector<uint8_t> final_executable;

        /**
         * Header que escribir en el archivo final
         */
        HeaderVELB final_header{};


        /**
         * Tabla de secciones finales
         */
        std::vector<section_info_linker> final_sections;

        /**
         * Pool de cadenas, estas son las que iran en la seccion "strings"
         */
        std::vector<std::string> string_pool;

        /**
         * Segun el nombre instroducido, devuelve un offset u otro, este offset
         * es donde esta la cadena si se suma la direccion base.
         */
        std::unordered_map<std::string, uint64_t> string_offsets;

        /**
         * contenido final de la sección "strings"
         * "code\0data\0stack\0main\0foo\0bar\0"
         */
        std::vector<uint8_t> string_blob;

        /**
         * Vector de espacios de direcciones, cuando se junta varios modulos,
         * cada modulo puede contener los mismos espacios de direcciones o
         * tener mas o menos, en cualquiera de los casos, al generar un binario
         * final debe existir todos los espacios de direcciones descritos por
         * los modulos, en caso de que los espacios de direcciones ya existan
         * con un mismo nombre, se aplicara una relocalizacion a todo_ el codigo
         * de un modulo bajo ese espacio de direcciones, para que los
         * codigos de distintos modulos coexistan en el mismo espacio de direcciones
         */
        std::unordered_map<std::string, Space> spaces_address;

        /**
         * Fusiona los espacios de direcciones de todos los módulos.
         * Responsabilidades
         *     - Unir rangos de memoria
         *     - Detectar solapamientos
         *     - Reasignar offsets si es necesario
         *     - Preparar el layout final del ejecutable
         */
        void merge_address_spaces();

        /**
         * Fusiona secciones de todos los módulos.
         *
         * Responsabilidades
         *     - Concatenar .code
         *     - Concatenar .data
         *     - Alinear secciones según reglas
         *     - Actualizar offsets de símbolos
         */
        void merge_sections();

        /**
          * Construye el header VELB final.
          * Responsabilidades
          *     Escribir:
          *         - magic "VELB"
          *         - versión
          *         - checksum
          *         - timestamp
          *         - arquitectura
          *         - número de secciones
          *         - tabla de espacios de direcciones
          *     Calcular checksum del ejecutable
         */
        void build_header();

        /**
         * Permite la construccion de la seccion especial "strings",
         * esta seccion contiene strings especiales que solo el linker usara,
         * normalmente son strings contantes con el nombre de los espacios de direcciones,
         * nombre de los labels y otros. Cada string debe tener un terminador nulo.
         *
         * Es necesario conocer el offset en el que comienda dicha seccion para poder
         * calcular el offset a esta tabla
         *
         * @param offset_init offset donde inicial la seccion de cadena.
         */
        void build_section_strings(uint64_t offset_init);

        uint64_t compute_sections_base_offset() const;

        void compute_symbol_file_offsets();

        std::string get_string_from_offset(uint64_t offset) const;

        /**
         * Construye la tabla de secciones del ejecutable.
         * Responsabilidades
         *     Escribir:
         *         - nombre de sección (16 bytes)
         *         - rango de memoria
         *         - offset en el archivo
         *         - tamaño
         */
        void build_section_table();
    };
};

//#endif

#endif //VELB_LINKER_BYTECODE_H
