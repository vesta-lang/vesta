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

    typedef struct range_memory {
        uint64_t address_init;
        uint64_t address_final;
    } range_memory;

    struct Label {
        std::string name;
        uint64_t address;   // offset relativo dentro de la sección
    };

    typedef struct Section {
        std::string  name; // nombre de la seccion
        range_memory memory;

        /**
         * Las secciones contienen labels
         */
        std::unordered_map<std::string, Label> table_label;

        void set_range(uint64_t init, uint64_t final) {
            memory.address_init  = init;
            memory.address_final = final;
        }

        void add_label(const std::string& name, uint64_t address) {
            table_label[name] = Label{name, address};
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
            sec.name                 = name;
            sec.memory.address_init  = init;
            sec.memory.address_final = final;
            table_section[name]      = sec;
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
    } Space;

    typedef struct Context {
        // espacio de direcciones key(nombre): valor(espacio)
        std::unordered_map<std::string, Space> space_address;

        // Crear y añadir un espacio por parámetros
        void add_space(const std::string &name,
                       uint64_t           init, uint64_t final) {
            Space sp;
            sp.range.address_init  = init;
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
        Section* get_section(const std::string& name) {
            for (auto& [spaceName, space] : space_address) {
                auto it = space.table_section.find(name);
                if (it != space.table_section.end())
                    return &it->second;
            }
            return nullptr;
        }
    } Context;

    /**
     * Alias de tipo funcion
     */
    using AnnotationHandler = std::function<void(const vm::AnnotationNode *, Assembler &)>;

    void apply_space_address(const vm::AnnotationNode *node, Assembler &ctx);
    void apply_section(const vm::AnnotationNode *node, Assembler &ctx);

    static std::unordered_map<std::string, AnnotationHandler> annotation_handlers = {
        {
            "SpaceAddress", apply_space_address
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
}

#endif //ANNOTATIONS_H
