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

#ifndef STRUCT_CONTEXT_H
#define STRUCT_CONTEXT_H
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <optional>
#include <system_error>

#include "linker/velb_linker_bytecode.h"

namespace Assembly::Bytecode {
    /**
     * Permite realizar una alineacion hacia abajo.
     * @param value valor a alinear
     * @param alignment valor al que debemos realizar la alineacion.
     * @return valor alineado hacia abajo.
     */
    static inline uint64_t align_down(uint64_t value, uint64_t alignment) {
        return value & ~(alignment - 1);
    }

    /**
     * Permite realizar una alineacion hacia arriba, necesario para hacer
     * alineaciones de pagina.
     * @param value valor a alinear
     * @param alignment valor al que debemos realizar la alineacion, en caso de paginas suelen ser 4096 bytes.
     * @return
     */
    static inline uint64_t align_up(uint64_t value, uint64_t alignment) {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    /**
     * Estructura basica para representar una relocalizacion
     */
    struct Relocation {
        /**
         * Tipo de relocalizaciones, esto puede cambiar en el futuro segun
         * las necesidades del conjunto de instrucciones.
         */
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


    /**
     * Estructura basica que define los campos de un label
     */
    typedef struct Label {
        std::string name; // nombre de la etiqueta
        uint64_t address{}; // offset relativo dentro de la sección
        uint64_t size{}; // tamaño de la etiqueta
    } Label;

    typedef struct Section {
        /**
         * nombre de la seccion
         */
        std::string name;

        /**
         * rango de operacion de la seccion, es relativo al espacio de direcciones al generarse el ejecutable.
         */
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

        /**
         * Permite cambiar el rango de operacion de una seccion.
         * @param init valor donde inicia la seccion
         * @param final direccion final de la seccion.
         */
        void set_range(uint64_t init, uint64_t final) {
            memory.address_init = init;
            memory.address_final = final;
        }

        /**
         * Permite añadir un label a una seccion. Las direcciones usadas en los labels son relativas a las secciones.
         * @param name Nombre de la etiqueta.
         * @param address direcciones relativas.
         * @param size tamaño del label.
         */
        void add_label(const std::string &name, uint64_t address, uint64_t size) {
            table_label[name] = Label{name, address, size};
        }

        /**
         * Obtener una etiqueta o label por su identificador.
         * @param name nombre de la etiqueta
         * @return en caso de encontrarse se devuelve un puntero a este, en caso contrario se devuelve un nullptr.
         */
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
        /**
         * Rango de memoria para el espacio de direcciones.
         */
        range_memory range;

        /**
         * nombre de la seccion
         */
        std::string name_section;

        /**
         * tabla de secciones key(nombre): valor(seccion)
         */
        std::unordered_map<std::string, Section> table_section;

        /**
         * Añadir sección nueva, estas secciones se suelen almacenar en un espacio de direcciones, y una seccion
         * suele tener una tabla con labels.
         * @param sec seccion a aladir
         */
        void add_section(const Section &sec) {
            table_section[sec.name] = sec;
        }

        /**
         * Añadir sección nueva, estas secciones se suelen almacenar en un espacio de direcciones, y una seccion
         * suele tener una tabla con labels.
         * @param name nombre de la nueva seccion.
         * @param init direccion inicial de la seccion, relativa al espacio de direcciones.
         * @param final direccion final de la seccion, relativa al espacio de direcciones.
         */
        void add_section(const std::string &name, uint64_t init, uint64_t final) {
            Section sec;
            sec.name = name;
            sec.memory.address_init = init;
            sec.memory.address_final = final;
            sec.size_real = 0;
            sec.size_align_section = 1;

            // añadimos la seccion a la tabla de secciones.
            add_section(sec);
        }

        /**
         * Establecer nombre de sección
         * @param name nombre de la seccion. El nombre de la seccion sera almacenada en la tabla de metadatos del
         * ejecutabe VELB.
         */
        void set_name(const std::string &name) {
            name_section = name;
        }

        /**
         * Liberar memoria interna
         */
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
        /**
         * Valor PC de inicio para el linker, este valor es necesario al generar el bytecode para indicar
         * a la VM donde iniciar la ejecucion, si el usuario no define donde iniciar usando la notacion
         * @InitPc(label) entonces se definiria por defecto iniciar en la direccion 0 de la memoria virtual.
         */
        uint64_t start_pc{};

        /**
         * espacio de direcciones key(nombre): valor(espacio)
         */
        std::unordered_map<std::string, Space> space_address;

        /**
         * tabla de tipo vector que contiene las tablas de relocalizaciones
         */
        std::vector<Relocation> relocations; // todas las relocaciones del módulo

        /**
         * relocalizacion por "nombre simbolo": "indice en la tabla de relocalizaciones (relocations)"
         */
        std::unordered_map<std::string, std::vector<size_t> > reloc_by_symbol;

        /**
         * Añade una relocalizacion a la tabla de relocalizaciones en el contexto del ensamblador. Esta relocalizacion
         * puede resolverse en el linker si es una relozalizacion dentro del propio codigo, en caso de ser una
         * llamada a una libreria o api externa, la relocalizacion se hara en run time con el linker dinamico, por
         * lo que la relocalizacion se añadira a la tabla de relocalizaciones del ejecutable.
         * @param rel relocalizacion a almacenar.
         */
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

        /**
         * Crear y añadir un espacio de memoria para el contexto.
         * @param name nombre del espacio de memoria
         * @param init direccion inicial del espacio
         * @param final direccion final
         */
        void add_space(const std::string &name,
                       uint64_t init, uint64_t final) {
            Space sp;
            sp.range.address_init = init;
            sp.range.address_final = final;
            sp.set_name(name);
            space_address[name] = sp;
        }

        /**
         * Obtener un espacio (si existe).
         * @param name nombre del espacio de direcciones a buscar
         * @return devuelve el espacio de direcciones si existe, en caso contrario devuelve nullptr.
         */
        Space *get_space(const std::string &name) {
            auto it = space_address.find(name);
            if (it != space_address.end())
                return &it->second;
            return nullptr;
        }

        /**
         * Liberar toda la memoria
         */
        void clear() {
            for (auto &kv: space_address)
                kv.second.clear(); // limpia secciones internas

            space_address.clear();
        }

        /**
         * obtener una sección por nombre
         * @param name nombre de la seccion a buscar
         * @return si la seccion existe la devuelve, en caso contrario se retorna nullptr
         */
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

    /**
     * @brief Resultado de una búsqueda de label en el contexto.
     */
    struct LabelLookupResult {
        Label *label = nullptr; ///< puntero al label (dentro de Section::table_label)
        Section *section = nullptr; ///< puntero a la sección que contiene el label
        Space *space = nullptr; ///< puntero al espacio que contiene la sección
        std::string space_name; ///< nombre del espacio (clave)
        std::string section_name; ///< nombre de la sección (clave)
        std::optional<uint64_t> absolute_address; ///< dirección absoluta si se pudo calcular
        [[nodiscard]] bool found() const { return label != nullptr; }
    };

    /**
     * @brief Busca un label por nombre en todas las spaces del Context.
     *
     * Recorre todas las spaces y sus secciones buscando el label con clave `label_name`.
     * Devuelve el primer match encontrado (orden de búsqueda: iteración de unordered_map).
     *
     * @param ctx Contexto que contiene space_address.
     * @param label_name Nombre del label a buscar.
     * @return LabelLookupResult con punteros y dirección absoluta si es posible.
     *
     * @note Complejidad: O(#sections + #labels en secciones) en el peor caso.
     *
     * @code{.cpp}
     * auto res = find_label_in_context(ctx, "start");
     * if (res.found()) {
     *     std::cout << "Encontrado en space=" << res.space_name
     *               << " section=" << res.section_name << "\n";
     *     if (res.absolute_address) std::cout << "Addr abs: " << *res.absolute_address << "\n";
     * }
     * @endcode
     */
    static LabelLookupResult find_label_in_context(Context &ctx, const std::string &label_name) {
        LabelLookupResult out;
        for (auto &space_kv: ctx.space_address) {
            Space &space = space_kv.second;
            const std::string &space_name = space_kv.first;
            for (auto &sec_kv: space.table_section) {
                Section &sec = sec_kv.second;
                const std::string &section_name = sec_kv.first;
                auto it = sec.table_label.find(label_name);
                if (it != sec.table_label.end()) {
                    out.label = &it->second;
                    out.section = &sec;
                    out.space = &space;
                    out.space_name = space_name;
                    out.section_name = section_name;
                    // intentar calcular dirección absoluta si los rangos están definidos
                    std::error_code ec;
                    if (space.range.address_init != 0 || sec.memory.address_init != 0) {
                        // Dirección absoluta = base del espacio + inicio de la sección (relativo al espacio) + offset del label (relativo a la sección)
                        uint64_t abs = space.range.address_init + sec.memory.address_init + it->second.address;
                        out.absolute_address = abs;
                    }
                    return out;
                }
            }
        }
        return out; // not found
    }

    /**
     * @brief Versión const de la búsqueda (no modifica el Context).
     */
    static LabelLookupResult find_label_in_context_const(const Context &ctx, const std::string &label_name) {
        LabelLookupResult out;
        for (const auto &space_kv: ctx.space_address) {
            const Space &space = space_kv.second;
            const std::string &space_name = space_kv.first;
            for (const auto &sec_kv: space.table_section) {
                const Section &sec = sec_kv.second;
                const std::string &section_name = sec_kv.first;
                auto it = sec.table_label.find(label_name);
                if (it != sec.table_label.end()) {
                    // cast away const only for pointers in result? prefer to return non-const pointers as nullptr if const context
                    out.label = const_cast<Label *>(&it->second);
                    out.section = const_cast<Section *>(&sec);
                    out.space = const_cast<Space *>(&space);
                    out.space_name = space_name;
                    out.section_name = section_name;
                    uint64_t abs = space.range.address_init + sec.memory.address_init + it->second.address;
                    out.absolute_address = abs;
                    return out;
                }
            }
        }
        return out;
    }

    /**
     * @brief Busca un label dentro de una space concreta por nombre de sección y label.
     *
     * @param ctx Contexto
     * @param space_name Nombre del espacio donde buscar
     * @param label_name Nombre del label a buscar
     * @return LabelLookupResult con resultado o vacío si no existe.
     */
    static LabelLookupResult find_label_in_space(Context &ctx, const std::string &space_name,
                                                 const std::string &label_name) {
        LabelLookupResult out;
        auto it_space = ctx.space_address.find(space_name);
        if (it_space == ctx.space_address.end()) return out;
        Space &space = it_space->second;
        for (auto &sec_kv: space.table_section) {
            Section &sec = sec_kv.second;
            const std::string &section_name = sec_kv.first;
            auto it = sec.table_label.find(label_name);
            if (it != sec.table_label.end()) {
                out.label = &it->second;
                out.section = &sec;
                out.space = &space;
                out.space_name = space_name;
                out.section_name = section_name;
                uint64_t abs = space.range.address_init + sec.memory.address_init + it->second.address;
                out.absolute_address = abs;
                return out;
            }
        }
        return out;
    }
}

#endif //STRUCT_CONTEXT_H
