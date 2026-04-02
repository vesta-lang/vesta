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
#include "linker/velb_linker_bytecode.h"
#include "parser/parser.h"
#include "emmit/struct_context.h"

namespace vm {
    struct AnnotationNode;
}

namespace Assembly::Bytecode {
    class Assembler;


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

    /**
     * Permite configurar el valor PC de inicio, se recomienda inicializar el valor para indicar donde empezar la
     * ejecuccion, sino se configura la ejecucion siempre comenzara en la direccion 0x000000.
     * Esta notacion permite recibir un label donde se calculara la direccion de inicio, o se le puede
     * especificar una direccion cruda directamente si se conoce.
     * En caso de especificarle una label, la label debe estar previamente declarada, un ejemplo:
     *
     * @code{.cpp}
     * code:
     *     @InitPc(code)
     *     adds r0, 3
     * @endcode
     *
     * En caso de encontrarse varios InitPc, se usara el ultimo definido.
     *
     * @param node nodo padre
     * @param assembler contexto global del ensamblador
     */
    void apply_init_pc(const vm::AnnotationNode *node, Assembler &assembler);

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
            "InitPc", apply_init_pc
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
