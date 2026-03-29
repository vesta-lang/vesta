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

typedef struct PACKED HeaderVELA {
    char magic[4]; // "VELA"
    uint32_t version; // versión del formato
    uint32_t module_count; // número de módulos
    uint64_t module_table_offset;
} HeaderVELA;

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


#ifdef __cplusplus
}
#endif


//#ifdef __cplusplus
// Código exclusivo de C++ (clases, métodos, namespaces, etc.)

#include <unordered_map>
#include <vector>
#include <c++/string>

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
        std::string map_file_path;  // donde generar el map
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
        std::vector<uint8_t> bytecode;
        Context ctx;
        bool is_object = false;

        std::vector<Relocation> relocations;
        std::unordered_map<std::string, uint64_t> local_symbols;
    };


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
                               const Context &ctx);

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

    private:
        LinkerOptions options;
        LinkerReport report;

        // Interno: lista de módulos cargados
        struct Module {
            std::vector<uint8_t> bytecode;
            Context ctx;
            bool is_object = false;

            std::vector<Relocation> relocations;
            std::unordered_map<std::string, uint64_t> local_symbols;
        };

        std::vector<Module> modules;
        std::vector<StaticLibrary> libraries;

        std::unordered_map<std::string, uint64_t> global_symbols;

        std::vector<uint8_t> final_executable;

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
