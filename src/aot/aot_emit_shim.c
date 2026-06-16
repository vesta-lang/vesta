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
 * @file aot/aot_emit_shim.c
 * @brief Phase AOT.4 -- implementacion C del emisor PE/ELF sobre LibPEparse.
 *
 * Se compila como C (donde las cabeceras de LibPEparse compilan sin problemas)
 * y expone la ABI plana de @c aot_emit_shim.h al lado C++.  Aqui vive todo el
 * trabajo con @c CreatePe.h / @c CreateELF.h.
 */

#include "aot/aot_emit_shim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "CreatePe.h"
#include "CreateELF.h"
#include "LibELFparse.h"

/* Copia segura del mensaje de error al buffer del llamador. */
static void set_err(char *err, size_t cap, const char *msg) {
    if (!err || cap == 0) return;
    size_t n = strlen(msg);
    if (n >= cap) n = cap - 1;
    memcpy(err, msg, n);
    err[n] = '\0';
}

/* =========================================================================
 *  PE32+
 * ========================================================================= */

/* Traduce flags genericos AOT_SEC_* a Characteristics de seccion PE. */
static _DWORD pe_section_chars(uint32_t flags) {
    _DWORD c = 0;
    if (flags & AOT_SEC_CODE) c |= ___IMAGE_SCN_CNT_CODE;
    if (flags & AOT_SEC_DATA) c |= ___IMAGE_SCN_CNT_INITIALIZED_DATA;
    if (flags & AOT_SEC_BSS)  c |= ___IMAGE_SCN_CNT_UNINITIALIZED_DATA;
    if (flags & AOT_SEC_EXEC) c |= ___IMAGE_SCN_MEM_EXECUTE;
    if (flags & AOT_SEC_READ) c |= ___IMAGE_SCN_MEM_READ;
    if (flags & AOT_SEC_WRITE)c |= ___IMAGE_SCN_MEM_WRITE;
    return c;
}

int aot_emit_pe(const char *path, const AotLayoutCfg *cfg,
                const AotSection *secs, int num_secs,
                int entry_sec, uint64_t entry_off,
                const AotImport *imps, int num_imps,
                char *err, size_t err_cap) {
    if (num_secs <= 0) { set_err(err, err_cap, "aot_emit_pe: sin secciones"); return 0; }
    if (entry_sec < 0 || entry_sec >= num_secs) {
        set_err(err, err_cap, "aot_emit_pe: entry_sec fuera de rango"); return 0;
    }

    PE64FILE_struct pe;
    initializePE64File(&pe);

    /* Aplicar configuracion de layout (campos en 0 => se deja el default). */
    if (cfg) {
        if (cfg->image_base)    pe.ntHeaders.OptionalHeader.ImageBase = cfg->image_base;
        if (cfg->section_align) pe.ntHeaders.OptionalHeader.SectionAlignment = cfg->section_align;
        if (cfg->file_align)    pe.ntHeaders.OptionalHeader.FileAlignment = cfg->file_align;
        if (cfg->stack_reserve) pe.ntHeaders.OptionalHeader.SizeOfStackReserve = cfg->stack_reserve;
        if (cfg->stack_commit)  pe.ntHeaders.OptionalHeader.SizeOfStackCommit = cfg->stack_commit;
        if (cfg->heap_reserve)  pe.ntHeaders.OptionalHeader.SizeOfHeapReserve = cfg->heap_reserve;
        if (cfg->heap_commit)   pe.ntHeaders.OptionalHeader.SizeOfHeapCommit = cfg->heap_commit;
        if (cfg->pe_subsystem > 0)
            pe.ntHeaders.OptionalHeader.Subsystem = (uint16_t)cfg->pe_subsystem;
    }

    /* Agregar las secciones del usuario en orden (indice PE == indice AOT). */
    for (int i = 0; i < num_secs; ++i) {
        const AotSection *s = &secs[i];
        if (s->flags & AOT_SEC_BSS) {
            addBssSection(&pe, s->name, s->bss_size);
        } else {
            addSection(&pe, s->name, pe_section_chars(s->flags),
                       (_BYTE *)s->data, s->size);
        }
    }

    /* Imports -> .idata + IAT + parcheo de los call. */
    if (imps && num_imps > 0) {
        const ___IMAGE_SECTION_HEADER *last = &pe.sectionHeaders[pe.numberOfSections - 1];
        _DWORD idataRVA = align(last->VirtualAddress + last->Misc.VirtualSize,
                                pe.ntHeaders.OptionalHeader.SectionAlignment);

        /* Agrupar por DLL preservando orden + dedup de funciones. */
        int max_libs = num_imps;
        const char **lib_names = (const char **)calloc(max_libs, sizeof(char *));
        const char ***lib_funcs = (const char ***)calloc(max_libs, sizeof(char **));
        int *lib_nfuncs = (int *)calloc(max_libs, sizeof(int));
        int num_libs = 0;
        for (int k = 0; k < num_imps; ++k) {
            int di = -1;
            for (int j = 0; j < num_libs; ++j)
                if (strcmp(lib_names[j], imps[k].dll) == 0) { di = j; break; }
            if (di < 0) {
                di = num_libs++;
                lib_names[di] = imps[k].dll;
                lib_funcs[di] = (const char **)calloc(num_imps, sizeof(char *));
                lib_nfuncs[di] = 0;
            }
            int dup = 0;
            for (int j = 0; j < lib_nfuncs[di]; ++j)
                if (strcmp(lib_funcs[di][j], imps[k].func) == 0) { dup = 1; break; }
            if (!dup) lib_funcs[di][lib_nfuncs[di]++] = imps[k].func;
        }

        ImportLibrary *libs = (ImportLibrary *)calloc(num_libs, sizeof(ImportLibrary));
        for (int j = 0; j < num_libs; ++j) {
            libs[j].dllName = lib_names[j];
            libs[j].functions = lib_funcs[j];
            libs[j].numFunctions = lib_nfuncs[j];
        }

        _DWORD idata_size = 0;
        ImportOffsetEntry *off_entries = NULL;
        int num_off = 0;
        _BYTE *idata_buf = buildMultiIdataSectionWithOffsets(
            libs, num_libs, idataRVA, &idata_size, &off_entries, &num_off);

        int idata_idx = -1;
        if (idata_buf) {
            idata_idx = addSection(&pe, ".idata",
                                   ___IMAGE_SCN_CNT_INITIALIZED_DATA | ___IMAGE_SCN_MEM_READ |
                                       ___IMAGE_SCN_MEM_WRITE,
                                   idata_buf, idata_size);

            pe.ntHeaders.OptionalHeader.DataDirectory[___IMAGE_DIRECTORY_ENTRY_IMPORT]
                .VirtualAddress = idataRVA;
            pe.ntHeaders.OptionalHeader.DataDirectory[___IMAGE_DIRECTORY_ENTRY_IMPORT]
                .Size = (_DWORD)(sizeof(___IMAGE_IMPORT_DESCRIPTOR) * (num_libs + 1));
            if (num_off > 0) {
                pe.ntHeaders.OptionalHeader.DataDirectory[___IMAGE_DIRECTORY_ENTRY_IAT]
                    .VirtualAddress = idataRVA + (_DWORD)off_entries[0].offset;
                pe.ntHeaders.OptionalHeader.DataDirectory[___IMAGE_DIRECTORY_ENTRY_IAT]
                    .Size = (_DWORD)(sizeof(_QWORD) * num_off);
            }

            /* Parchear cada call importado.  El offset_iat es relativo a
             * idataRVA, asi que la base que se pasa es la VA de .idata. */
            uint64_t idata_base = pe.ntHeaders.OptionalHeader.ImageBase + idataRVA;
            FunctionOffset *fix = (FunctionOffset *)calloc(num_imps, sizeof(FunctionOffset));
            int nfix = 0;
            int ok = 1;
            for (int k = 0; k < num_imps && ok; ++k) {
                if (imps[k].call_section < 0 || imps[k].call_section >= num_secs) {
                    set_err(err, err_cap, "aot_emit_pe: call_section invalido");
                    ok = 0; break;
                }
                int iat_off = -1;
                for (int e = 0; e < num_off; ++e)
                    if (strcmp(imps[k].func, off_entries[e].functionName) == 0 &&
                        strcmp(imps[k].dll, off_entries[e].dllName) == 0) {
                        iat_off = off_entries[e].offset; break;
                    }
                if (iat_off < 0) {
                    set_err(err, err_cap, "aot_emit_pe: import no resuelto");
                    ok = 0; break;
                }
                /* Parchear inmediatamente (cada call puede estar en una seccion
                 * distinta -> base de codigo propia). */
                int cs = imps[k].call_section;
                uint64_t code_vaddr =
                    pe.sectionHeaders[cs].VirtualAddress + pe.ntHeaders.OptionalHeader.ImageBase;
                FunctionOffset fo;
                fo.offset_iat = iat_off;
                fo.offset_code = (uint32_t)imps[k].call_off;
                fo.name = imps[k].func;
                parchearDesplazamientosPorOffset(
                    pe.sectionData[cs], pe.sectionHeaders[cs].Misc.VirtualSize,
                    code_vaddr, idata_base, &fo, 1);
                fix[nfix++] = fo;
            }
            free(fix);
            free(idata_buf);
            free(off_entries);
            if (!ok) {
                for (int j = 0; j < num_libs; ++j) free(lib_funcs[j]);
                free(lib_names); free(lib_funcs); free(lib_nfuncs); free(libs);
                freePE64File(&pe);
                return 0;
            }
            (void)idata_idx;
        } else {
            set_err(err, err_cap, "aot_emit_pe: fallo construyendo .idata");
            for (int j = 0; j < num_libs; ++j) free(lib_funcs[j]);
            free(lib_names); free(lib_funcs); free(lib_nfuncs); free(libs);
            freePE64File(&pe);
            return 0;
        }

        for (int j = 0; j < num_libs; ++j) free(lib_funcs[j]);
        free(lib_names); free(lib_funcs); free(lib_nfuncs); free(libs);
    }

    finalizePE64File(&pe);
    pe.ntHeaders.OptionalHeader.AddressOfEntryPoint =
        (_DWORD)(pe.sectionHeaders[entry_sec].VirtualAddress + entry_off);

    writePE64File(&pe, path);
    freePE64File(&pe);
    return 1;
}

/* =========================================================================
 *  ELF64 (freestanding)
 * ========================================================================= */

#define AOT_ELF_PAGE 0x1000ull

int aot_emit_elf(const char *path, const AotLayoutCfg *cfg,
                 const AotSection *secs, int num_secs,
                 int entry_sec, uint64_t entry_off,
                 char *err, size_t err_cap) {
    if (num_secs <= 0) { set_err(err, err_cap, "aot_emit_elf: sin secciones"); return 0; }
    if (entry_sec < 0 || entry_sec >= num_secs) {
        set_err(err, err_cap, "aot_emit_elf: entry_sec fuera de rango"); return 0;
    }

    uint64_t base = (cfg && cfg->image_base) ? cfg->image_base : 0x400000ull;
    uint64_t stack_vaddr = (cfg && cfg->elf_stack_vaddr) ? cfg->elf_stack_vaddr : 0x70000000ull;
    uint64_t stack_size  = (cfg && cfg->elf_stack_size)  ? cfg->elf_stack_size  : 0x10000ull;

    size_t total_data = 0;
    for (int i = 0; i < num_secs; ++i) total_data += secs[i].size;
    size_t capacity = (total_data + 64 * AOT_ELF_PAGE + AOT_ELF_PAGE) & ~(AOT_ELF_PAGE - 1);

    ElfBuilder *b = elf_builder_create_exec64(capacity, /*num_phdrs=*/2);
    if (!b) { set_err(err, err_cap, "aot_emit_elf: create_exec64 fallo"); return 0; }
    Elf64_Phdr *phdr = (Elf64_Phdr *)b->phdr;

    uint64_t entry_vaddr = 0;

    /* Agregar las secciones con datos (todas en el unico segmento R+X). */
    for (int i = 0; i < num_secs; ++i) {
        const AotSection *s = &secs[i];
        if ((s->flags & AOT_SEC_BSS) || s->size == 0) {
            /* BSS en ELF freestanding v1 no soportado (ver doc). */
            continue;
        }
        if (b->size % AOT_ELF_PAGE != 0) {
            size_t pad = AOT_ELF_PAGE - (b->size % AOT_ELF_PAGE);
            memset(b->mem + b->size, 0, pad);
            b->size += pad;
        }
        uint64_t sh_flags = SHF_ALLOC;
        if (s->flags & AOT_SEC_EXEC)  sh_flags |= SHF_EXECINSTR;
        if (s->flags & AOT_SEC_WRITE) sh_flags |= SHF_WRITE;
        uint64_t align_use = s->align ? s->align : AOT_ELF_PAGE;
        size_t   off_out = 0;
        uint64_t vaddr_out = 0;
        size_t idx = elf_builder_add_section_ex(
            b, s->name, SHT_PROGBITS, sh_flags,
            s->data, s->size, base + b->size, align_use,
            &off_out, &vaddr_out, 0, 0, 0);
        if (idx == 0) {
            elf_builder_free(b);
            set_err(err, err_cap, "aot_emit_elf: fallo anyadiendo seccion");
            return 0;
        }
        if (i == entry_sec) entry_vaddr = vaddr_out;
    }
    if (entry_vaddr == 0) {
        elf_builder_free(b);
        set_err(err, err_cap, "aot_emit_elf: la seccion de entrada no tiene datos");
        return 0;
    }

    /* .strtab + .symtab con _start. */
    const char strtab_data[] = "\0_start";
    size_t st_off = 0; uint64_t st_va = 0;
    size_t idx_strtab = elf_builder_add_section_ex(
        b, ".strtab", SHT_STRTAB, 0, strtab_data, sizeof(strtab_data),
        0, 1, &st_off, &st_va, 0, 0, 0);

    Elf64_Sym symtab[2];
    memset(symtab, 0, sizeof(symtab));
    symtab[0].st_info = ELF64_ST_INFO(STB_LOCAL, STT_NOTYPE);
    symtab[0].st_shndx = SHN_UNDEF;
    symtab[1].st_name = 1;
    symtab[1].st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
    symtab[1].st_shndx = (uint16_t)(entry_sec + 1); /* +1: la seccion 0 es SHT_NULL */
    symtab[1].st_value = entry_vaddr + entry_off;
    symtab[1].st_size = secs[entry_sec].size;
    size_t sy_off = 0; uint64_t sy_va = 0;
    elf_builder_add_section_ex(
        b, ".symtab", SHT_SYMTAB, 0, symtab, sizeof(symtab),
        0, 8, &sy_off, &sy_va, (uint32_t)idx_strtab, 1, sizeof(Elf64_Sym));

    /* Program headers. */
    phdr[0].p_type = PT_LOAD;
    phdr[0].p_flags = PF_R | PF_X;
    phdr[0].p_offset = 0;
    phdr[0].p_vaddr = base;
    phdr[0].p_paddr = base;
    phdr[0].p_filesz = b->size;
    phdr[0].p_memsz = (b->size + AOT_ELF_PAGE - 1) & ~(AOT_ELF_PAGE - 1);
    phdr[0].p_align = AOT_ELF_PAGE;

    phdr[1].p_type = PT_LOAD;
    phdr[1].p_flags = PF_R | PF_W;
    phdr[1].p_offset = 0;
    phdr[1].p_vaddr = stack_vaddr;
    phdr[1].p_paddr = stack_vaddr;
    phdr[1].p_filesz = 0;
    phdr[1].p_memsz = stack_size;
    phdr[1].p_align = AOT_ELF_PAGE;

    elf_builder_finalize_exec64(b, entry_vaddr + entry_off);

    FILE *f = fopen(path, "wb");
    if (!f) { elf_builder_free(b); set_err(err, err_cap, "aot_emit_elf: fopen fallo"); return 0; }
    size_t written = fwrite(b->mem, 1, b->size, f);
    fclose(f);
    int ok = (written == b->size);
    if (!ok) set_err(err, err_cap, "aot_emit_elf: escritura incompleta");
    elf_builder_free(b);
    return ok;
}
