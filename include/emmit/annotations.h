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

#ifndef ANNOTATIONS_H
#define ANNOTATIONS_H
#include <cstring>
#include <functional>
#include <string>
#include <unordered_set>

#include "assembly/assembly.h"
#include "parser/parser.h"

namespace vm {
    struct AnnotationNode;
}

namespace Assembly::Bytecode {
    class Assembler;

    static inline uint64_t align_down(uint64_t value, uint64_t alignment) {
        return value & ~(alignment - 1);
    }

    static inline uint64_t align_up(uint64_t value, uint64_t alignment) {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    typedef struct range_memory {
        uint64_t address_init;
        uint64_t address_final;
    } range_memory;

    struct Label {
        std::string name;
        uint64_t address; // offset relativo dentro de la sección
    };

    typedef struct Section {
        std::string name; // nombre de la seccion
        range_memory memory;

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

        void add_label(const std::string &name, uint64_t address) {
            table_label[name] = Label{name, address};
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
            // Si no hay labels, la sección está vacía
            if (table_label.empty()) {
                memory.address_init = base_address;
                memory.address_final = base_address;
                return;
            }

            // El inicio REAL de la sección es SIEMPRE base_address
            uint64_t start = base_address;

            // El final REAL es el mayor offset relativo
            uint64_t max_off = 0;
            for (const auto &[name, lbl]: table_label) {
                if (lbl.address > max_off)
                    max_off = lbl.address;
            }

            uint64_t end = base_address + max_off;

            // Alinear inicio y fin
            uint64_t aligned_init = align_down(start, bytes_aligned);
            if (aligned_init < base_address)
                aligned_init = base_address;

            uint64_t aligned_end = align_up(end, size_align_section);

            memory.address_init = aligned_init;
            memory.address_final = aligned_end;
        }
    } Section;

    typedef struct Space {
        range_memory range;

        // nombre de la seccion, maximo 16 bytes.
        uint8_t name_section[16];

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
            table_section[name] = sec;
        }

        // Establecer nombre de sección (máx 16 bytes)
        void set_name(const std::string &name) {
            std::memset(name_section, 0, sizeof(name_section));
            std::memcpy(name_section, name.c_str(),
                        std::min(name.size(), sizeof(name_section)));
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

    /**
     * Alias de tipo funcion
     */
    using AnnotationHandler = std::function<void(const vm::AnnotationNode *, Assembler &)>;

    /**
     * Permite aplicar una anotacion de tipo "SpaceAddress" que permite definir un espacio
     * de direcciones para las secciones.
     * @param node nodo padre
     * @param ctx contexto global del ensamblador
     */
    void apply_space_address(const vm::AnnotationNode *node, Assembler &ctx);

    /**
     * Permite crear una seccion asociada a un espacio de direcciones. Las secciones
     * pueden contener cualquier tipo de informacion, ya sea codigo datos u otro.
     * @param node nodo padre
     * @param ctx contexto global del ensamblador
     */
    void apply_section(const vm::AnnotationNode *node, Assembler &ctx);

    /**
     * Permite indicar al ensamblador el formato final del archivo que se quiere
     * ensamblar. No es estrictamente necesario definir el formato, pero todo dependera
     * del objetivo, sino se define formato, el codigo generado final sera plano, sin secciones
     * ni otro tipo de informacion que no sea el dispuesto por el usuario.
     * @param node nodo padre
     * @param ctx contexto global del ensamblador
     */
    void apply_format(const vm::AnnotationNode *node, Assembler &ctx);

    static std::unordered_map<std::string, AnnotationHandler> annotation_handlers = {
        {
            "SpaceAddress", apply_space_address
        },
        {
            "Format", apply_format
        },
        {
            "Section", apply_section
        },
        {
            "IniAddress", [](const vm::AnnotationNode *a, Assembler &ctx) {
                /* ... */
            }
        },
        {
            "EndAddress", [](const vm::AnnotationNode *a, Assembler &ctx) {
                /* ... */
            }
        },
        {
            "Name", [](const vm::AnnotationNode *a, Assembler &ctx) {
                /* ... */
            }
        }
    };

    static std::unordered_set<std::string> annotation_allow_doc = {
        "Args"
        "Class",
        "Method"
        "Description",
        "Access",
        "Extends",
        "Interface",
        "Field",
        "Description",
        "Name",
        "Offset",
        "Type",
        "Size"
        "Warning"
    };

    /**
     * Permite imprimir el contexto generado por el ensamblador
     * @param ctx contexto del ensamblador
     */
    static void print_context(const Context &ctx) {
        std::cout << "=== CONTEXT ===\n";
        std::cout << "Formato de salida: " << ctx.format_output << "\n\n";

        for (const auto &[spaceName, space]: ctx.space_address) {
            std::cout << "== Space: " << spaceName << " ==\n";
            std::cout << "  Rango: [0x"
                    << std::hex << space.range.address_init
                    << ", 0x" << space.range.address_final << "]\n";

            std::cout << "  Nombre seccion (16 bytes): ";
            for (int i = 0; i < 16; i++) {
                if (space.name_section[i] == 0) break;
                std::cout << space.name_section[i];
            }
            std::cout << "\n";


            // Recorrer secciones
            for (const auto &[secName, sec]: space.table_section) {
                std::cout << "  -- Section: " << secName << " Alineacion: 0x" << sec.size_align_section << " --\n";
                std::cout << "     Rango: [0x"
                        << std::hex << sec.memory.address_init
                        << ", 0x" << sec.memory.address_final << "]\n";

                // Recorrer labels
                if (sec.table_label.empty()) {
                    std::cout << "     (sin labels)\n";
                } else {
                    std::cout << "     Labels:\n";
                    for (const auto &[labelName, label]: sec.table_label) {
                        std::cout << "       * " << labelName
                                << " @ offset 0x" << std::hex << label.address
                                << "\n";
                    }
                }
            }

            std::cout << "\n";
        }
    }

    static void print_context_with_bytes(const Context &ctx,
                                         const std::vector<uint8_t> &bytes) {
        std::cout << "=== CONTEXT + BYTES ===\n";
        std::cout << "Formato de salida: " << ctx.format_output << "\n\n";

        for (const auto &[spaceName, space]: ctx.space_address) {
            std::cout << "== Space: " << spaceName << " ==\n";
            std::cout << "  Rango: [0x"
                    << std::hex << space.range.address_init
                    << ", 0x" << space.range.address_final << "]\n";

            std::cout << "  Nombre seccion (16 bytes): ";
            for (int i = 0; i < 16; i++) {
                if (space.name_section[i] == 0) break;
                std::cout << space.name_section[i];
            }
            std::cout << "\n";

            // Recorrer secciones
            for (const auto &[secName, sec]: space.table_section) {
                std::cout << "  -- Section: " << secName
                        << " (align 0x" << std::hex << sec.size_align_section << ") --\n";

                std::cout << "     Rango virtual: [0x"
                        << std::hex << sec.memory.address_init
                        << ", 0x" << sec.memory.address_final << "]\n";

                // Mostrar labels
                if (sec.table_label.empty()) {
                    std::cout << "     (sin labels)\n";
                } else {
                    std::cout << "     Labels:\n";
                    for (const auto &[labelName, label]: sec.table_label) {
                        std::cout << "       * " << labelName
                                << " @ offset 0x" << std::hex << label.address << "\n";
                    }
                }

                // Mostrar bytes reales de la sección
                std::cout << "     Bytes:\n";

                uint64_t start = sec.memory.address_init - space.range.address_init;
                uint64_t end = sec.memory.address_final - space.range.address_init;

                if (start >= bytes.size()) {
                    std::cout << "       (fuera del rango del binario)\n";
                    continue;
                }

                end = std::min<uint64_t>(end, bytes.size());

                const size_t BYTES_PER_LINE = 16;

                for (uint64_t i = start; i < end; i++) {
                    if ((i - start) % BYTES_PER_LINE == 0) {
                        std::cout << "\n       "
                                << std::setw(8) << std::setfill('0') << std::hex << i
                                << ": ";
                    }

                    std::cout << std::setw(2) << std::setfill('0')
                            << std::hex << (int) bytes[i] << " ";
                }

                std::cout << "\n\n";
            }

            std::cout << "\n";
        }
    }
}

#endif //ANNOTATIONS_H
