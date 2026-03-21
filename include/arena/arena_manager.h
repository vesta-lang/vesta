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

#ifndef ARENA_MANAGER_H
#define ARENA_MANAGER_H

#include <unordered_map>

#include "arena.h"

namespace vm {
    struct Arena;

    class ArenaManager {
    public:
        ArenaManager();
        ~ArenaManager();


        /**
         * @brief Crea un nuevo arenero de memoria, 4096 bytes o 4KBm las paginas no tienen por que estar
         * de forma consecutiva
         * @param size Tamaño en bytes
         * @param perms Permisos de la memoria
         * @return ID unico del arenero
         */
        int create_arena(size_t size, MemPerm perms);

        /**
         * @brief Libera un arenero previamente creado
         * @param id ID del arenero
         * @return true si se libero correctamente
         */
        bool free_arena(int id);

        /**
         * @brief Obtiene informacion del arenero
         * @param id ID del arenero
         * @return puntero a Arena, nullptr si no existe
         */
        const Arena *get_arena(int id) const;

        /**
         * @brief Libera todos los areneros
         */
        void free_all();

        size_t total_allocated_bytes_;

        std::unordered_map<uint64_t, Arena> arenas;

    protected: // Para que hereden, y TLB pueda usarlo
        int                            next_id; ///< para generar IDs unicos

    };
}
#endif //ARENA_MANAGER_H
