//
// Created by desmon0xff on 29/03/2026.
//

#ifndef STRUCT_CONTEXT_H
#define STRUCT_CONTEXT_H
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "linker/velb_linker_bytecode.h"

namespace Assembly::Bytecode {
    static inline uint64_t align_down(uint64_t value, uint64_t alignment) {
        return value & ~(alignment - 1);
    }

    static inline uint64_t align_up(uint64_t value, uint64_t alignment) {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    struct Relocation {
        enum class Type {
            Absolute40, // dirección absoluta de 40 bits, para sub, add y etc
            Relative40, // desplazamiento relativo de 40 bits
            Absolute64,
            Relative32
        };

        std::string symbol; // símbolo a resolver
        std::string section; // sección donde ocurre
        uint64_t offset; // offset dentro de la sección
        Type type; // tipo de relocación
    };


    typedef struct Label {
        std::string name;
        uint64_t address; // offset relativo dentro de la sección
        uint64_t size; // tamaño de la etiqueta
    } Label;

    typedef struct Section {
        std::string name; // nombre de la seccion
        range_memory memory;

        uint64_t size_real; // tamaño real de bytes generados

        /**
         * tamaño al que alinear la seccion
         */
        uint32_t size_align_section;

        /**
         * Las secciones contienen labels
         */
        std::unordered_map<std::string, Label> table_label;

        void set_range(uint64_t init, uint64_t final) {
            memory.address_init = init;
            memory.address_final = final;
        }

        void add_label(const std::string &name, uint64_t address, uint64_t size) {
            table_label[name] = Label{name, address, size};
        }

        Label *get_label(const std::string &name) {
            auto it = table_label.find(name);
            if (it == table_label.end())
                return nullptr;
            return &it->second;
        }

        /**
         * Permite computar el rango de inicio y final de una seccion, para
         * esta tarea, se debe haber mapeado antes todos los labels, luego obtenemos
         * el offset mas pequeño y el mas grande y al sumarlo a la direccion base,
         * obtenemos el rango de memoria virutal.
         * @param base_address direccion base del espacio de memoria o de la ultima seccion
         * @param bytes_aligned bytes a alinear, normalmente 4096 para el formato VELB
         */
        void compute_range(uint64_t base_address, uint64_t bytes_aligned) {
            uint64_t start = base_address;
            uint64_t end_real = base_address + size_real;

            uint64_t aligned_init = align_down(start, bytes_aligned);
            if (aligned_init < base_address)
                aligned_init = base_address;

            uint64_t aligned_end = align_up(end_real, size_align_section);

            memory.address_init = aligned_init;
            memory.address_final = aligned_end;
        }

        /**
         * Permtie actualizar el tamaño de una label en base al nombre de esta
         * @param name nombre de la label en la seccion.
         * @param size nuevo tamaño para la label
         */
        void update_label_size(const std::string &name, uint64_t size) {
            auto it = table_label.find(name);
            if (it == table_label.end()) {
                throw std::runtime_error("update_label_size: label no encontrada: " + name);
            }
            it->second.size = size;
        }
    } Section;

    typedef struct Space {
        range_memory range;

        // nombre de la seccion, maximo 16 bytes.
        std::string name_section;

        // tabla de secciones key(nombre): valor(seccion)
        std::unordered_map<std::string, Section> table_section;

        // Añadir sección
        void add_section(const Section &sec) {
            table_section[sec.name] = sec;
        }

        // Añadir sección por parámetros
        void add_section(const std::string &name, uint64_t init, uint64_t final) {
            Section sec;
            sec.name = name;
            sec.memory.address_init = init;
            sec.memory.address_final = final;
            sec.size_real = 0;
            sec.size_align_section = 1;
            table_section[name] = sec;
        }

        // Establecer nombre de sección (máx 16 bytes)
        void set_name(const std::string &name) {
            name_section = name;
        }

        // Liberar memoria interna
        void clear() {
            table_section.clear();
        }

        /**
         * Computar el rango de direcciones de cada seccion del espacio de direcciones.
         *
         * @param bytes_aligned cantidad de bytes a usar para alinear la seccion
         */
        void compute_sections_ranges(uint64_t bytes_aligned) {
            for (auto &[name, sec]: table_section) {
                sec.compute_range(range.address_init, bytes_aligned);
            }
        }
    } Space;

    typedef struct Context {
        // espacio de direcciones key(nombre): valor(espacio)
        std::unordered_map<std::string, Space> space_address;

        // tabla de tipo vector que contiene las tablas de relocalizaciones
        std::vector<Relocation> relocations; // todas las relocaciones del módulo

        // relocalizacion por "nombre simbolo": "indice en la tabla de relocalizaciones (relocations)"
        std::unordered_map<std::string, std::vector<size_t> > reloc_by_symbol;

        void add_relocation(const Relocation &rel) {
            // Guardar índice
            size_t index = relocations.size();

            // Añadir al vector principal
            relocations.push_back(rel);

            // Añadir al índice por símbolo
            reloc_by_symbol[rel.symbol].push_back(index);
        }

        /**
         * Permite obtener todas las relocaciones de un símbolo
         * @param symbol nombre del simbolo del que obtener todas las relocalizaciones
         * @return tabla de relocalizaciones para ese simbolo
         */
        std::vector<const Relocation *> get_relocations_for(const std::string &symbol) const {
            std::vector<const Relocation *> result;

            auto it = reloc_by_symbol.find(symbol);
            if (it == reloc_by_symbol.end())
                return result;

            for (size_t idx: it->second) {
                result.push_back(&relocations[idx]);
            }

            return result;
        }

        /**
         * Metodo para limpiar relocaciones
         */
        void clear_relocations() {
            relocations.clear();
            reloc_by_symbol.clear();
        }

        /**
         * Permite saber si un simbolo tiene relocalizaciones
         * @param symbol simbolo del que averiguar si tiene relocalizaciones
         * @return true si hay relocalizaciones.
         */
        bool has_relocations(const std::string &symbol) const {
            return reloc_by_symbol.count(symbol) > 0;
        }

        /**
         * Formato de salida, por defecto el fortmato es plano o crudo.
         * Este variable puede ser alterada usando la notacion "Format",
         * pero solo surge efecto la ultima anotacion definida de este tipo
         * en una misma unidad de traducion.
         */
        std::string format_output = "raw";

        /**
         * Tamnaño al que alinear cada seccion, si es el formato VELB,
         * debe ser 4096
         */
        uint32_t bytes_aligned = 0x1;

        // Crear y añadir un espacio por parámetros
        void add_space(const std::string &name,
                       uint64_t init, uint64_t final) {
            Space sp;
            sp.range.address_init = init;
            sp.range.address_final = final;
            sp.set_name(name);
            space_address[name] = sp;
        }

        // Obtener un espacio (si existe)
        Space *get_space(const std::string &name) {
            auto it = space_address.find(name);
            if (it != space_address.end())
                return &it->second;
            return nullptr;
        }

        // Liberar toda la memoria
        void clear() {
            for (auto &kv: space_address)
                kv.second.clear(); // limpia secciones internas

            space_address.clear();
        }

        // obtener una sección por nombre
        Section *get_section(const std::string &name) {
            for (auto &[spaceName, space]: space_address) {
                auto it = space.table_section.find(name);
                if (it != space.table_section.end())
                    return &it->second;
            }
            return nullptr;
        }

        /**
         * Computar todos los rangos de direcciones de cada seccion
         */
        void compute_all_ranges() {
            for (auto &[name, sp]: space_address) {
                sp.compute_sections_ranges(bytes_aligned);
            }
        }
    } Context;
}

#endif //STRUCT_CONTEXT_H
