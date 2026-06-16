/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 */

/**
 * @file tests/aot/test_object_writer.cpp
 * @brief Phase AOT.4 -- valida el emisor de ejecutables (Pe/ElfWriter).
 *
 * Emite un programa "exit(42)" escrito a mano en PE y en ELF y verifica la
 * estructura del fichero producido (cabeceras, maquina, entrada).  Los
 * ficheros quedan en disco (exit42_pe.exe / exit42_elf.elf) para correr el PE
 * en el host Windows como prueba end-to-end (./exit42_pe.exe ; echo $?  -> 42).
 */

#include "aot/object_writer.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace aot;

static int g_checks = 0;
static int g_fails  = 0;

#define CHECK(cond)                                                         \
    do {                                                                    \
        ++g_checks;                                                         \
        if (!(cond)) {                                                      \
            ++g_fails;                                                      \
            std::printf("  FAIL linea %d: %s\n", __LINE__, #cond);          \
        }                                                                   \
    } while (0)

static std::vector<uint8_t> read_file(const std::string &path) {
    std::vector<uint8_t> out;
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0) {
        out.resize(static_cast<size_t>(n));
        size_t rd = std::fread(out.data(), 1, out.size(), f);
        out.resize(rd);
    }
    std::fclose(f);
    return out;
}

static uint32_t rd_u32(const std::vector<uint8_t> &b, size_t off) {
    if (off + 4 > b.size()) return 0;
    return (uint32_t)b[off] | ((uint32_t)b[off + 1] << 8) |
           ((uint32_t)b[off + 2] << 16) | ((uint32_t)b[off + 3] << 24);
}
static uint16_t rd_u16(const std::vector<uint8_t> &b, size_t off) {
    if (off + 2 > b.size()) return 0;
    return (uint16_t)(b[off] | (b[off + 1] << 8));
}
static uint64_t rd_u64(const std::vector<uint8_t> &b, size_t off) {
    return (uint64_t)rd_u32(b, off) | ((uint64_t)rd_u32(b, off + 4) << 32);
}

// =========================================================================
//  PE: exit(42) via kernel32!ExitProcess
// =========================================================================

static void test_pe_exit42() {
    std::printf("test_pe_exit42\n");

    // _start:
    //   48 83 EC 28           sub rsp, 0x28        ; reservar shadow space + align
    //   B9 2A 00 00 00        mov ecx, 42          ; arg0 = codigo de salida
    //   FF 15 00 00 00 00     call [rip+disp32]    ; ExitProcess (disp a parchear)
    //   CC                    int3                 ; unreachable
    std::vector<uint8_t> code = {
        0x48, 0x83, 0xEC, 0x28,
        0xB9, 0x2A, 0x00, 0x00, 0x00,
        0xFF, 0x15, 0x00, 0x00, 0x00, 0x00,
        0xCC
    };
    const uint64_t call_off = 9;  // offset del FF 15

    ObjectWriter w(ObjFormat::PE);
    const int t = w.add_text(code);             // crea .text + fija entrada en off 0
    w.add_import_call(ImportCall{"KERNEL32.dll", "ExitProcess", t, call_off});

    std::string err;
    const std::string path = "exit42_pe.exe";
    const bool ok = w.write(path, err);
    if (!ok) std::printf("  write error: %s\n", err.c_str());
    CHECK(ok);

    std::vector<uint8_t> pe = read_file(path);
    CHECK(pe.size() > 0x200);
    // Firma MZ.
    CHECK(pe.size() >= 2 && pe[0] == 'M' && pe[1] == 'Z');
    // e_lfanew @ 0x3C -> firma PE\0\0.
    const uint32_t lfanew = rd_u32(pe, 0x3C);
    CHECK(rd_u32(pe, lfanew) == 0x00004550 /* 'PE\0\0' */);
    // Machine AMD64 (0x8664) en FileHeader (lfanew+4).
    CHECK(rd_u16(pe, lfanew + 4) == 0x8664);
    // OptionalHeader.AddressOfEntryPoint @ FileHeader(20) + 16.
    const uint32_t opt = lfanew + 4 + 20;
    const uint32_t entry_rva = rd_u32(pe, opt + 16);
    CHECK(entry_rva == 0x1000);  // .text en RVA 0x1000, entry_off=0
}

// =========================================================================
//  ELF: exit(42) via syscall (freestanding, sin libc ni imports)
// =========================================================================

static void test_elf_exit42() {
    std::printf("test_elf_exit42\n");

    // _start:
    //   BF 2A 00 00 00        mov edi, 42          ; arg0 = codigo de salida
    //   B8 3C 00 00 00        mov eax, 60          ; sys_exit
    //   0F 05                 syscall
    std::vector<uint8_t> code = {
        0xBF, 0x2A, 0x00, 0x00, 0x00,
        0xB8, 0x3C, 0x00, 0x00, 0x00,
        0x0F, 0x05
    };

    ObjectWriter w(ObjFormat::ELF);
    w.add_text(code);

    std::string err;
    const std::string path = "exit42_elf.elf";
    const bool ok = w.write(path, err);
    if (!ok) std::printf("  write error: %s\n", err.c_str());
    CHECK(ok);

    std::vector<uint8_t> elf = read_file(path);
    CHECK(elf.size() > 64);
    // Magic \x7f E L F.
    CHECK(elf.size() >= 4 && elf[0] == 0x7F && elf[1] == 'E' && elf[2] == 'L' && elf[3] == 'F');
    CHECK(elf[4] == 2);  // ELFCLASS64
    CHECK(elf[5] == 1);  // ELFDATA2LSB
    // e_type @ 16 = ET_EXEC (2).
    CHECK(rd_u16(elf, 16) == 2);
    // e_machine @ 18 = EM_X86_64 (62).
    CHECK(rd_u16(elf, 18) == 62);
    // e_entry @ 24: en el rango de carga (>= base 0x400000).
    const uint64_t entry = rd_u64(elf, 24);
    CHECK(entry >= 0x400000);
    // Los bytes del codigo deben aparecer en el fichero (mov edi,42 = BF 2A..).
    bool found = false;
    for (size_t i = 0; i + 2 < elf.size(); ++i) {
        if (elf[i] == 0xBF && elf[i + 1] == 0x2A && elf[i + 5] == 0xB8 && elf[i + 6] == 0x3C) {
            found = true;
            break;
        }
    }
    CHECK(found);
}

int main() {
    std::printf("=== test_object_writer (Phase AOT.4) ===\n");
    test_pe_exit42();
    test_elf_exit42();
    std::printf("\n%d checks, %d fallos\n", g_checks, g_fails);
    std::printf("Ficheros: exit42_pe.exe (correr en host) / exit42_elf.elf\n");
    return g_fails == 0 ? 0 : 1;
}
