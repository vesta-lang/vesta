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
#include "arena/arena.h"
#include <iostream>
#include <cassert>

int main() {
    const size_t PAGE_SIZE = 4096;

    // Test 1: Reservar memoria RW
    void *mem_rw = vm::allocate_memory(PAGE_SIZE, vm::MemPerm::READ | vm::MemPerm::WRITE);
    assert(mem_rw != nullptr);

    // Escribir datos
    char *buffer = static_cast<char *>(mem_rw);
    for (size_t i = 0; i < PAGE_SIZE; ++i) {
        buffer[i] = static_cast<char>(i & 0xFF);
    }

    // Leer datos y verificar
    for (size_t i = 0; i < PAGE_SIZE; ++i) {
        assert(buffer[i] == static_cast<char>(i & 0xFF));
    }

    vm::free_memory(mem_rw, PAGE_SIZE);
    std::cout << "[Arena Test] Memoria RW OK\n";

    // Test 2: Reservar memoria RWE
    void *mem_rwe = allocate_memory(PAGE_SIZE, vm::MemPerm::READ | vm::MemPerm::WRITE | vm::MemPerm::EXEC);
    assert(mem_rwe != nullptr);

    // Escribir datos
    buffer = static_cast<char *>(mem_rwe);
    for (size_t i = 0; i < PAGE_SIZE; ++i) {
        buffer[i] = static_cast<char>(0xAA);
    }

    // Leer datos y verificar
    for (size_t i = 0; i < PAGE_SIZE; ++i) {
        assert(buffer[i] == static_cast<char>(0xAA));
    }

    vm::free_memory(mem_rwe, PAGE_SIZE);
    std::cout << "[Arena Test] Memoria RWE OK\n";

    // Test 3: Reservar memoria sin permisos
    void *mem_none = vm::allocate_memory(PAGE_SIZE, vm::MemPerm::NONE);
    assert(mem_none != nullptr);
    vm::free_memory(mem_none, PAGE_SIZE);
    std::cout << "[Arena Test] Memoria NONE OK\n";

    std::cout << "[Arena Test] Todos los tests OK\n";
    return 0;
}
