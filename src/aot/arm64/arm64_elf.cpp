/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file arm64_elf.cpp
 * @brief Implementacion del emisor de ejecutable ELF64 AArch64 minimo.
 */

#include "aot/arm64/arm64_elf.h"

#include <cstring>
#include <fstream>
#include <vector>

namespace aot {

namespace {
constexpr uint16_t ET_EXEC = 2;
constexpr uint16_t EM_AARCH64 = 183; // 0xB7
constexpr uint32_t PT_LOAD = 1;
constexpr uint32_t PF_X = 1, PF_W = 2, PF_R = 4;
constexpr uint32_t EHDR_SIZE = 64;
constexpr uint32_t PHDR_SIZE = 56;

/// Escribe un entero de @p n bytes en little-endian al final de @p v.
void put(std::vector<uint8_t> &v, uint64_t value, int n) {
    for (int i = 0; i < n; ++i)
        v.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFF));
}
} // namespace

bool write_elf64_aarch64_exec(const std::string &path, const uint8_t *code,
                              size_t size, uint64_t base_vaddr) {
    const uint32_t code_off = EHDR_SIZE + PHDR_SIZE; // codigo tras las cabeceras.
    const uint64_t entry = base_vaddr + code_off;
    const uint64_t total = code_off + size;

    std::vector<uint8_t> out;
    out.reserve(total);

    // --- ELF64 header (64 bytes) ---
    out.push_back(0x7F); // e_ident: magic
    out.push_back('E');
    out.push_back('L');
    out.push_back('F');
    out.push_back(2); // EI_CLASS = ELFCLASS64
    out.push_back(1); // EI_DATA  = ELFDATA2LSB (little-endian)
    out.push_back(1); // EI_VERSION = EV_CURRENT
    out.push_back(0); // EI_OSABI = SYSV
    for (int i = 8; i < 16; ++i)
        out.push_back(0); // padding EI_PAD
    put(out, ET_EXEC, 2);     // e_type
    put(out, EM_AARCH64, 2);  // e_machine
    put(out, 1, 4);           // e_version
    put(out, entry, 8);       // e_entry
    put(out, EHDR_SIZE, 8);   // e_phoff (justo tras el header)
    put(out, 0, 8);           // e_shoff (sin secciones)
    put(out, 0, 4);           // e_flags
    put(out, EHDR_SIZE, 2);   // e_ehsize
    put(out, PHDR_SIZE, 2);   // e_phentsize
    put(out, 1, 2);           // e_phnum
    put(out, 0, 2);           // e_shentsize
    put(out, 0, 2);           // e_shnum
    put(out, 0, 2);           // e_shstrndx

    // --- Program header (56 bytes): un PT_LOAD RWX con TODO el fichero ---
    put(out, PT_LOAD, 4);              // p_type
    put(out, PF_R | PF_W | PF_X, 4);   // p_flags
    put(out, 0, 8);                    // p_offset (mapea desde el inicio)
    put(out, base_vaddr, 8);           // p_vaddr
    put(out, base_vaddr, 8);           // p_paddr
    put(out, total, 8);                // p_filesz
    put(out, total, 8);                // p_memsz
    put(out, 0x1000, 8);               // p_align

    // --- Codigo ---
    out.insert(out.end(), code, code + size);

    std::ofstream f(path, std::ios::binary);
    if (!f)
        return false;
    f.write(reinterpret_cast<const char *>(out.data()),
            static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(f);
}

} // namespace aot
