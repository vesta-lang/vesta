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
#include <iostream>
#include <cassert>
#include <cstring> // memcpy
#include "arena/arena.h"
#include "arena/arena_manager.h"

int main() {
    vm::ArenaManager manager{};

    std::cout << "[ArenaManager Test] Iniciando tests...\n";

    // Crear areneros
    int arena1 = manager.create_arena(64, vm::MemPerm::READ | vm::MemPerm::WRITE);
    int arena2 = manager.create_arena(128, vm::MemPerm::READ | vm::MemPerm::EXEC);
    int arena3 = manager.create_arena(256, vm::MemPerm::READ | vm::MemPerm::WRITE | vm::MemPerm::EXEC);

    assert(arena1 != 0 && arena2 != 0 && arena3 != 0);

    // Mostrar informacion de los areneros
    auto show_arena = [&](int id) {
        const vm::Arena *a = manager.get_arena(id);
        assert(a != nullptr);
        std::cout << "Arena ID=" << id
                << " | ptr=" << a->ptr
                << " | size=" << a->size
                << " | perms="
                << (has_perm(a->perms, vm::MemPerm::READ) ? "R" : "")
                << (has_perm(a->perms, vm::MemPerm::WRITE) ? "W" : "")
                << (has_perm(a->perms, vm::MemPerm::EXEC) ? "X" : "")
                << "\n";
        return a;
    };

    const vm::Arena *a1 = show_arena(arena1);
    const vm::Arena *a2 = show_arena(arena2);
    const vm::Arena *a3 = show_arena(arena3);

    // Test de escritura y lectura (solo si tiene WRITE)
    if (has_perm(a1->perms, vm::MemPerm::WRITE)) {
        int *data = static_cast<int *>(a1->ptr);
        data[0]   = 123;
        data[1]   = 456;
        assert(data[0] == 123);
        assert(data[1] == 456);
        std::cout << "[Arena ID=" << arena1 << "] Escritura y lectura OK: "
                << data[0] << ", " << data[1] << "\n";
    }

    if (has_perm(a3->perms, vm::MemPerm::WRITE)) {
        char *      mem = static_cast<char *>(a3->ptr);
        const char *msg = "Hola Arena 3";
        std::memcpy(mem, msg, std::strlen(msg) + 1);
        std::cout << "[Arena ID=" << arena3 << "] Contenido escrito: " << mem << "\n";
    }

    // Liberar areneros
    assert(manager.free_arena(arena1));
    assert(manager.free_arena(arena2));
    assert(manager.free_arena(arena3));

    // Verificar que ya no existen
    assert(manager.get_arena(arena1) == nullptr);
    assert(manager.get_arena(arena2) == nullptr);
    assert(manager.get_arena(arena3) == nullptr);

    std::cout << "[ArenaManager Test] Todos los areneros liberados correctamente.\n";
    std::cout << "[ArenaManager Test] ¡Test completado con exito!\n";

    return 0;
}
