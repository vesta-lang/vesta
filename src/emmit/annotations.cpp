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

#include "emmit/annotations.h"

#include "emmit/parser_to_bytecode.h"


namespace Assembly::Bytecode {

    const vm::AnnotationNode* find_child_raw(
        const vm::AnnotationNode* node,
        const std::string& key
    ) {
        for (const auto& child : node->children) {
            if (child->key == key)
                return child.get();
        }
        return nullptr;
    }


    /**
     * Función auxiliar para buscar un hijo por nombre. No es obligatorio que exista
     * @param node Nodo padre
     * @param key nombre del nodo hijo
     * @return nodo hijo que coindice
     */
    const vm::AnnotationNode *optional_find_child(
        const vm::AnnotationNode *node,
        const std::string &key
    ) {
        return find_child_raw(node, key);
    }

    /**
     * Función auxiliar para buscar un hijo por nombre, este debe existir o genera
     * una excepcion.
     * @param node Nodo padre
     * @param key nombre del nodo hijo
     * @return nodo hijo que coindice
     */
    const vm::AnnotationNode *expect_find_child(
        const vm::AnnotationNode *node,
        const std::string &key
    ) {
        if (auto* found = find_child_raw(node, key))
            return found;

        throw std::runtime_error(
            "La anotacion '" + node->key +
            "' requiere el operando '" + key + "' pero no fue encontrado"
        );
    }

    void apply_space_address(const vm::AnnotationNode *node, Assembler &assembler) {
        // Buscar los operandos por nombre
        const vm::AnnotationNode *nameNode =
                expect_find_child(node, "Name");

        const vm::AnnotationNode *iniNode =
                expect_find_child(node, "IniAddress");

        const vm::AnnotationNode *endNode =
                expect_find_child(node, "EndAddress");

        // Extraer valores
        std::string string_space = nameNode->value;
        uint64_t IniAddress = vm::parse_number(iniNode->value);
        uint64_t EndAddress = vm::parse_number(endNode->value);

        // Crear el espacio
        assembler.ctx.add_space(string_space, IniAddress, EndAddress);
    }

    void apply_section(const vm::AnnotationNode *node, Assembler &assembler) {
        // Obtener el nombre de la sección
        const vm::AnnotationNode *nameNode = expect_find_child(node, "Name");
        std::string section_name = nameNode->value;

        // Obtener el SpaceAddress asociado
        const vm::AnnotationNode *spaceNode = expect_find_child(node, "SpaceAddress");
        std::string space_name = spaceNode->value;

        // miramos si el usuario introdujo un nodo de tipo alineacion
        const vm::AnnotationNode *alignNode = optional_find_child(node, "Align");

        // Buscar el espacio en el contexto
        Space *sp = assembler.ctx.get_space(space_name);
        if (!sp) {
            throw std::runtime_error(
                "El espacio '" + space_name +
                "' no existe, pero fue referenciado por la seccion '" +
                section_name + "'"
            );
        }

        // Crear la sección
        Section sec;
        sec.name = section_name;

        // la alineacion para esta seccion sera la alineacion configurada globalmente
        // si no se definio una notacion Align dentro de la notacion de seccion.
        if (alignNode == nullptr) {
            sec.size_align_section = assembler.ctx.bytes_aligned;
        } else sec.size_align_section = vm::parse_number(alignNode->value);

        sec.memory.address_final = 0;
        sec.memory.address_init = 0;
        //  hereda el rango del espacio
        //sec.memory = sp->range;

        // Insertarla en el espacio
        sp->add_section(sec);

        // indicar al ensamblador la seccion nueva, usarla
        assembler.current_section = assembler.ctx.get_section(sec.name);
    }

    void apply_format(const vm::AnnotationNode *node, Assembler &ctx) {
        if (node->value.empty()) {
            throw std::runtime_error(
                "La anotacion '" + node->key +
                "' requiere que se defina un formato a usar, pero no se definio ninguno"
            );
        }

        // cambiamos el formato raw al definido por el usuario.
        ctx.ctx.format_output = node->value;
    }
}
