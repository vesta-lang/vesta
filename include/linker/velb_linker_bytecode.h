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
     */
    class Linker {
    public:
        explicit Linker(const LinkerOptions& opts = {})
        : options(opts) {}

        // Cargar archivos objeto o ejecutables parciales
        void add_object_file(const std::string &path);

        void add_object_memory(const std::vector<uint8_t> &data);

        // Cargar un ensamblado crudo (bytecode + contexto)
        void add_assembly_unit(const std::vector<uint8_t> &bytecode,
                               const Context &ctx);

        void add_static_library(const std::string &path);

        // Resolver símbolos entre módulos
        void resolve_symbols();
        void apply_relocations();
        void optimize_modules();

        // Construir el ejecutable final VELB
        std::vector<uint8_t> build_executable();

        // Guardar a archivo
        void write_to_file(const std::string &path);

        const LinkerReport& get_report() const { return report; }

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

        // Internos
        void merge_address_spaces();
        void merge_sections();
        void build_header();
        void build_section_table();
    };
};

//#endif

#endif //VELB_LINKER_BYTECODE_H
