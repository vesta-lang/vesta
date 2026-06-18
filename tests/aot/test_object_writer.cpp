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
#include <cstring>
#include <string>
#include <vector>

using namespace aot;

static int g_checks = 0;
static int g_fails = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_fails;                                                         \
            std::printf("  FAIL linea %d: %s\n", __LINE__, #cond);             \
        }                                                                      \
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
    //   48 83 EC 28           sub rsp, 0x28        ; reservar shadow space +
    //   align B9 2A 00 00 00        mov ecx, 42          ; arg0 = codigo de
    //   salida FF 15 00 00 00 00     call [rip+disp32]    ; ExitProcess (disp a
    //   parchear) CC                    int3                 ; unreachable
    std::vector<uint8_t> code = {0x48, 0x83, 0xEC, 0x28, 0xB9, 0x2A,
                                 0x00, 0x00, 0x00, 0xFF, 0x15, 0x00,
                                 0x00, 0x00, 0x00, 0xCC};
    const uint64_t call_off = 9; // offset del FF 15

    ObjectWriter w(ObjFormat::PE);
    const int t = w.add_text(code); // crea .text + fija entrada en off 0
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
    CHECK(entry_rva == 0x1000); // .text en RVA 0x1000, entry_off=0
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
    std::vector<uint8_t> code = {0xBF, 0x2A, 0x00, 0x00, 0x00, 0xB8,
                                 0x3C, 0x00, 0x00, 0x00, 0x0F, 0x05};

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
    CHECK(elf.size() >= 4 && elf[0] == 0x7F && elf[1] == 'E' && elf[2] == 'L' &&
          elf[3] == 'F');
    CHECK(elf[4] == 2); // ELFCLASS64
    CHECK(elf[5] == 1); // ELFDATA2LSB
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
        if (elf[i] == 0xBF && elf[i + 1] == 0x2A && elf[i + 5] == 0xB8 &&
            elf[i + 6] == 0x3C) {
            found = true;
            break;
        }
    }
    CHECK(found);
}

// =========================================================================
//  Reloc REL32 cross-seccion: leer un byte de .rodata via lea RIP-relativo
//  (Phase AOT.3 2a).  El exit-code = ese byte (42).  Valida add_reloc.
// =========================================================================

static void test_pe_rodata_reloc() {
    std::printf("test_pe_rodata_reloc\n");
    //   48 83 EC 28           sub rsp, 0x28
    //   48 8D 0D <disp32>     lea rcx, [rip+disp32]   ; &rodata  (reloc @7)
    //   0F B6 09              movzx ecx, byte [rcx]    ; ecx = rodata[0] = 42
    //   FF 15 <disp32>        call [rip+ExitProcess]   ; (call @14)
    //   CC
    std::vector<uint8_t> code = {0x48, 0x83, 0xEC, 0x28, 0x48, 0x8D, 0x0D,
                                 0x00, 0x00, 0x00, 0x00, 0x0F, 0xB6, 0x09,
                                 0xFF, 0x15, 0x00, 0x00, 0x00, 0x00, 0xCC};
    ObjectWriter w(ObjFormat::PE);
    const int t = w.add_text(code);
    const int rd = w.add_rodata(std::vector<uint8_t>{42});
    w.add_reloc(t, 7, RelocTarget::addr(rd, 0), RelocKind::REL32);
    w.add_import_call(ImportCall{"KERNEL32.dll", "ExitProcess", t, 14});

    std::string err;
    const std::string path = "rodata_pe.exe";
    const bool ok = w.write(path, err);
    if (!ok) std::printf("  write error: %s\n", err.c_str());
    CHECK(ok);
    std::vector<uint8_t> pe = read_file(path);
    CHECK(pe.size() > 0x200);
    CHECK(pe.size() >= 2 && pe[0] == 'M' && pe[1] == 'Z');
    // Tras la reloc, el disp32 del lea (@7) NO debe seguir en 0 (se parcheo).
    bool patched = false;
    for (size_t i = 0; i + 6 < pe.size(); ++i)
        if (pe[i] == 0x48 && pe[i + 1] == 0x8D && pe[i + 2] == 0x0D &&
            (pe[i + 3] | pe[i + 4] | pe[i + 5] | pe[i + 6]) != 0) {
            patched = true;
            break;
        }
    CHECK(patched);
}

static void test_elf_rodata_reloc() {
    std::printf("test_elf_rodata_reloc\n");
    //   48 8D 3D <disp32>     lea rdi, [rip+disp32]   ; &rodata  (reloc @3)
    //   0F B6 3F              movzx edi, byte [rdi]    ; edi = 42
    //   B8 3C 00 00 00        mov eax, 60              ; sys_exit
    //   0F 05                 syscall
    std::vector<uint8_t> code = {0x48, 0x8D, 0x3D, 0x00, 0x00, 0x00,
                                 0x00, 0x0F, 0xB6, 0x3F, 0xB8, 0x3C,
                                 0x00, 0x00, 0x00, 0x0F, 0x05};
    ObjectWriter w(ObjFormat::ELF);
    const int t = w.add_text(code);
    const int rd = w.add_rodata(std::vector<uint8_t>{42});
    w.add_reloc(t, 3, RelocTarget::addr(rd, 0), RelocKind::REL32);

    std::string err;
    const std::string path = "rodata_elf.elf";
    const bool ok = w.write(path, err);
    if (!ok) std::printf("  write error: %s\n", err.c_str());
    CHECK(ok);
    std::vector<uint8_t> elf = read_file(path);
    CHECK(elf.size() > 64);
    CHECK(elf.size() >= 4 && elf[0] == 0x7F && elf[1] == 'E');
    // el lea (48 8D 3D) debe tener disp32 != 0 tras la reloc.
    bool patched = false;
    for (size_t i = 0; i + 6 < elf.size(); ++i)
        if (elf[i] == 0x48 && elf[i + 1] == 0x8D && elf[i + 2] == 0x3D &&
            (elf[i + 3] | elf[i + 4] | elf[i + 5] | elf[i + 6]) != 0) {
            patched = true;
            break;
        }
    CHECK(patched);
}

// =========================================================================
//  Reloc REL32 cross-SECCION de codigo: .text llama a una funcion en .boot
//  (Phase AOT.3 2b, dev OS).  El exit-code = lo que .boot devuelve (42).
// =========================================================================

static void test_pe_cross_section_call() {
    std::printf("test_pe_cross_section_call\n");
    // .text (_start):
    //   48 83 EC 28           sub rsp, 0x28
    //   E8 <rel32>            call boot              ; (reloc REL32 -> .boot@0,
    //   @5) 89 C1                 mov ecx, eax FF 15 <disp32>        call
    //   [rip+ExitProcess]  ; (call @11) CC
    std::vector<uint8_t> text = {0x48, 0x83, 0xEC, 0x28, 0xE8, 0x00,
                                 0x00, 0x00, 0x00, 0x89, 0xC1, 0xFF,
                                 0x15, 0x00, 0x00, 0x00, 0x00, 0xCC};
    // .boot:  B8 2A 00 00 00  mov eax, 42 ; C3 ret
    std::vector<uint8_t> boot = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};

    ObjectWriter w(ObjFormat::PE);
    WriterSection st;
    st.name = ".text";
    st.flags = SecFlag::CODE | SecFlag::EXEC | SecFlag::READ;
    st.data = text;
    const int ts = w.add_section(std::move(st));
    WriterSection sb;
    sb.name = ".boot";
    sb.flags = SecFlag::CODE | SecFlag::EXEC | SecFlag::READ | SecFlag::WRITE;
    sb.data = boot;
    const int bs = w.add_section(std::move(sb));
    w.set_entry(ts, 0);
    w.add_reloc(ts, 5, RelocTarget::addr(bs, 0), RelocKind::REL32); // call boot
    w.add_import_call(ImportCall{"KERNEL32.dll", "ExitProcess", ts, 11});

    std::string err;
    const std::string path = "cross_pe.exe";
    const bool ok = w.write(path, err);
    if (!ok) std::printf("  write error: %s\n", err.c_str());
    CHECK(ok);
    std::vector<uint8_t> pe = read_file(path);
    CHECK(pe.size() > 0x200 && pe[0] == 'M' && pe[1] == 'Z');
    // el rel32 del call (@5) debe estar parcheado (!= 0).
    bool patched = false;
    for (size_t i = 0; i + 4 < pe.size(); ++i)
        if (pe[i] == 0xE8 &&
            (pe[i + 1] | pe[i + 2] | pe[i + 3] | pe[i + 4]) != 0) {
            patched = true;
            break;
        }
    CHECK(patched);
}

// =========================================================================
//  OBJECT relocatable ELF (.o, ET_REL): main global + .text->.rodata reloc.
//  (Phase AOT.4-ext)  Linkable con ld/gcc.
// =========================================================================

static void test_elf_obj() {
    std::printf("test_elf_obj\n");
    // .text:  i32 main(){ return rodata[0]; }
    //   48 8D 05 <disp32>     lea rax, [rip+disp32]   ; &rodata  (reloc @3)
    //   0F B6 00              movzx eax, byte [rax]
    //   C3                    ret
    std::vector<uint8_t> text = {0x48, 0x8D, 0x05, 0x00, 0x00, 0x00,
                                 0x00, 0x0F, 0xB6, 0x00, 0xC3};
    std::vector<uint8_t> rodata = {42};

    ObjectWriter w(ObjFormat::ELF);
    w.set_output_kind(OutputKind::OBJECT);
    const int ts = w.add_section(WriterSection{
        ".text", SecFlag::CODE | SecFlag::EXEC | SecFlag::READ, text, 0, 0, 0});
    const int rs = w.add_section(WriterSection{
        ".rodata", SecFlag::DATA | SecFlag::READ, rodata, 0, 0, 0});
    w.add_reloc(ts, 3, RelocTarget::addr(rs, 0), RelocKind::REL32);
    w.add_symbol("main", ts, 0, /*is_func=*/true);

    std::string err;
    const std::string path = "obj_main.o";
    const bool ok = w.write(path, err);
    if (!ok) std::printf("  write error: %s\n", err.c_str());
    CHECK(ok);
    std::vector<uint8_t> o = read_file(path);
    CHECK(o.size() > 64);
    CHECK(o.size() >= 4 && o[0] == 0x7F && o[1] == 'E' && o[2] == 'L' &&
          o[3] == 'F');
    CHECK(o[4] == 2);           // ELFCLASS64
    CHECK(rd_u16(o, 16) == 1);  // e_type ET_REL
    CHECK(rd_u16(o, 18) == 62); // e_machine EM_X86_64
    CHECK(rd_u16(o, 60) > 0);   // e_shnum > 0
    // El nombre "main" debe aparecer en la .strtab.
    bool has_main = false;
    for (size_t i = 0; i + 4 < o.size(); ++i)
        if (o[i] == 'm' && o[i + 1] == 'a' && o[i + 2] == 'i' &&
            o[i + 3] == 'n' && o[i + 4] == 0) {
            has_main = true;
            break;
        }
    CHECK(has_main);
}

// =========================================================================
//  OBJECT relocatable COFF (.obj): main external + .text->.rodata reloc.
//  (Phase AOT.4-ext)  Linkable con link.exe / gcc-mingw.
// =========================================================================

static void test_coff_obj() {
    std::printf("test_coff_obj\n");
    std::vector<uint8_t> text = {
        0x48, 0x8D, 0x05, 0x00, 0x00, 0x00, 0x00, // lea rax,[rip+0]  (reloc @3)
        0x0F, 0xB6, 0x00,                         // movzx eax,[rax]
        0xC3                                      // ret
    };
    std::vector<uint8_t> rodata = {42};

    ObjectWriter w(ObjFormat::PE);
    w.set_output_kind(OutputKind::OBJECT);
    const int ts = w.add_section(WriterSection{
        ".text", SecFlag::CODE | SecFlag::EXEC | SecFlag::READ, text, 0, 0, 0});
    const int rs = w.add_section(WriterSection{
        ".rodata", SecFlag::DATA | SecFlag::READ, rodata, 0, 0, 0});
    w.add_reloc(ts, 3, RelocTarget::addr(rs, 0), RelocKind::REL32);
    w.add_symbol("main", ts, 0, /*is_func=*/true);

    std::string err;
    const std::string path = "obj_main.obj";
    const bool ok = w.write(path, err);
    if (!ok) std::printf("  write error: %s\n", err.c_str());
    CHECK(ok);
    std::vector<uint8_t> o = read_file(path);
    CHECK(o.size() > 20);
    CHECK(rd_u16(o, 0) == 0x8664); // Machine AMD64
    CHECK(rd_u16(o, 2) == 2);      // NumberOfSections (.text + .rodata)
    // El nombre "main" debe aparecer en la symtab.
    bool has_main = false;
    for (size_t i = 0; i + 3 < o.size(); ++i)
        if (o[i] == 'm' && o[i + 1] == 'a' && o[i + 2] == 'i' &&
            o[i + 3] == 'n') {
            has_main = true;
            break;
        }
    CHECK(has_main);
}

// =========================================================================
//  SHARED PE .dll: exporta una funcion (IMAGE_FILE_DLL + export directory).
// =========================================================================

static void test_pe_dll() {
    std::printf("test_pe_dll\n");
    // .text:  i32 answer(){ return 42; }  ->  B8 2A 00 00 00 C3
    std::vector<uint8_t> text = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};
    ObjectWriter w(ObjFormat::PE);
    w.set_output_kind(OutputKind::SHARED);
    const int ts = w.add_section(WriterSection{
        ".text", SecFlag::CODE | SecFlag::EXEC | SecFlag::READ, text, 0, 0, 0});
    w.add_symbol("answer", ts, 0, /*is_func=*/true);

    std::string err;
    const std::string path = "shared_test.dll";
    const bool ok = w.write(path, err);
    if (!ok) std::printf("  write error: %s\n", err.c_str());
    CHECK(ok);
    std::vector<uint8_t> d = read_file(path);
    CHECK(d.size() > 0x200 && d[0] == 'M' && d[1] == 'Z');
    const uint32_t lfanew = rd_u32(d, 0x3C);
    CHECK(rd_u32(d, lfanew) == 0x00004550); // 'PE\0\0'
    CHECK(rd_u16(d, lfanew + 4) == 0x8664); // Machine AMD64
    // FileHeader.Characteristics @ lfanew+4+18; debe tener IMAGE_FILE_DLL
    // (0x2000).
    CHECK((rd_u16(d, lfanew + 4 + 18) & 0x2000) != 0);
    // "answer" debe aparecer (export name string).
    bool has = false;
    for (size_t i = 0; i + 6 < d.size(); ++i)
        if (!memcmp(&d[i], "answer", 6)) {
            has = true;
            break;
        }
    CHECK(has);
}

// =========================================================================
//  FLAT_BIN (.bin): binario plano sin cabecera; entry@0; relocs contra base.
//  (Phase AOT Inc 2)
// =========================================================================

static void test_flat_bin() {
    std::printf("test_flat_bin\n");
    // .text:
    //   E8 <rel32>          call data_ref       ; REL32 -> .rodata@4 (@1)
    //   48 B8 <imm64>       mov rax, &.rodata@0 ; ABS64 (@7) contra base
    //   C3                  ret
    std::vector<uint8_t> text = {
        0xE8, 0x00, 0x00, 0x00, 0x00,                // call rel32 (@1)
        0x48, 0xB8, 0,    0,    0,    0, 0, 0, 0, 0, // mov rax, imm64 (@7)
        0xC3};
    // .rodata: "ABCD" (4 bytes).
    std::vector<uint8_t> rod = {'A', 'B', 'C', 'D'};

    ObjectWriter w(ObjFormat::ELF); // formato irrelevante para flat
    WriterSection st;
    st.name = ".text";
    st.flags = SecFlag::CODE | SecFlag::EXEC | SecFlag::READ;
    st.data = text;
    const int ts = w.add_section(std::move(st));
    WriterSection sr;
    sr.name = ".rodata";
    sr.flags = SecFlag::DATA | SecFlag::READ;
    sr.data = rod;
    const int rs = w.add_section(std::move(sr));
    w.set_output_kind(OutputKind::FLAT_BIN);
    const uint64_t base = 0x7C00;
    w.set_flat_base(base);
    // REL32 desde .text@1 a .rodata@4 (invariante a la base).
    w.add_reloc(ts, 1, RelocTarget::addr(rs, 4), RelocKind::REL32);
    // ABS64 desde .text@7 a .rodata@0 (= base + offset_en_imagen de .rodata).
    w.add_reloc(ts, 7, RelocTarget::addr(rs, 0), RelocKind::ABS64);

    std::string err;
    const std::string path = "flat.bin";
    const bool ok = w.write(path, err);
    if (!ok) std::printf("  write error: %s\n", err.c_str());
    CHECK(ok);

    std::vector<uint8_t> bin = read_file(path);
    // Sin cabecera: el primer byte es el codigo (0xE8), no 'M'/0x7F.
    CHECK(bin.size() == text.size() + rod.size()); // 16 + 4 = 20 (sin padding)
    CHECK(!bin.empty() && bin[0] == 0xE8);
    CHECK(bin[0] != 'M' && bin[0] != 0x7F);
    // .rodata arranca justo tras .text (align 1): offset 16.
    const size_t rod_off = text.size();
    CHECK(bin.size() >= rod_off + 4 && bin[rod_off] == 'A' &&
          bin[rod_off + 3] == 'D');
    // REL32 @1: rel = target_image_off(rod@4=20) - (site+4=5) = 15.
    CHECK((int32_t)rd_u32(bin, 1) == (int32_t)((rod_off + 4) - 5));
    // ABS64 @7: base + rod_off (= .rodata@0 en la imagen).
    CHECK(rd_u64(bin, 7) == base + rod_off);
}

int main() {
    std::printf("=== test_object_writer (Phase AOT.4) ===\n");
    test_pe_exit42();
    test_elf_exit42();
    test_pe_rodata_reloc();
    test_elf_rodata_reloc();
    test_pe_cross_section_call();
    test_elf_obj();
    test_coff_obj();
    test_pe_dll();
    test_flat_bin();
    std::printf("\n%d checks, %d fallos\n", g_checks, g_fails);
    std::printf("Ficheros: exit42_pe.exe / exit42_elf.elf / rodata_pe.exe / "
                "rodata_elf.elf (correr para validar la reloc -> exit 42)\n");
    return g_fails == 0 ? 0 : 1;
}
