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

/* Escritura little-endian de 4/8 bytes en un buffer. */
static void wr32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v);       p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void wr64le(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (i * 8));
}

/* Aplica UNA relocation ya resuelta: @p target_value es la direccion (ADDR) o
 * el tamano (SIZE) del objetivo + addend; @p site_va es la VA del sitio (para
 * el rel32 PC-relativo); @p site escribe en el buffer de la seccion del sitio
 * en su offset.  Devuelve 0 en error de tamano/kind. */
static int apply_reloc(uint8_t *site, uint64_t site_va,
                       uint64_t target_value, int kind) {
    switch (kind) {
        case AOT_RELOC_REL32: {
            int64_t rel = (int64_t)target_value - (int64_t)(site_va + 4);
            wr32le(site, (uint32_t)(int32_t)rel);
            return 1;
        }
        case AOT_RELOC_IMM32:
            wr32le(site, (uint32_t)target_value);
            return 1;
        case AOT_RELOC_ABS64:
        case AOT_RELOC_IMM64:
            wr64le(site, target_value);
            return 1;
        default:
            return 0;
    }
}

/* Tamano "logico" de una seccion (datos inicializados o BSS). */
static uint64_t aot_sec_size(const AotSection *s) {
    return (s->flags & AOT_SEC_BSS) ? s->bss_size : s->size;
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
                const AotReloc *relocs, int num_relocs,
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

    /* Relocations cross-seccion: resolver AHORA que addSection ya asigno las
     * VirtualAddress de todas las secciones del usuario (igual que el parcheo
     * de imports, pero generico).  ImageBase + sectionHeaders[i].VirtualAddress
     * es la VA de la seccion i; pe.sectionData[i] es su buffer mutable. */
    if (relocs && num_relocs > 0) {
        const uint64_t image_base = pe.ntHeaders.OptionalHeader.ImageBase;
        /* Si hay refs ABSOLUTAS (ABS64/IMM64, p.ej. --no-pie), la imagen DEBE
         * cargar en su ImageBase: sin tabla .reloc, el loader no puede reubicar.
         * Limpiamos DYNAMIC_BASE (ASLR) -> base fija (semantica -no-pie).  Las
         * refs RIP-relativas (DATA_REL32) no lo necesitan (position-independent). */
        for (int r = 0; r < num_relocs; ++r) {
            if (relocs[r].kind == AOT_RELOC_ABS64 || relocs[r].kind == AOT_RELOC_IMM64) {
                pe.ntHeaders.OptionalHeader.DllCharacteristics &=
                    (uint16_t)~___IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE;
                break;
            }
        }
        for (int r = 0; r < num_relocs; ++r) {
            const AotReloc *rl = &relocs[r];
            if (rl->site_section < 0 || rl->site_section >= num_secs ||
                rl->target_section < 0 || rl->target_section >= num_secs) {
                set_err(err, err_cap, "aot_emit_pe: reloc con seccion fuera de rango");
                freePE64File(&pe); return 0;
            }
            uint64_t target_value;
            if (rl->target_is_size) {
                target_value = aot_sec_size(&secs[rl->target_section]);
            } else {
                target_value = image_base +
                    pe.sectionHeaders[rl->target_section].VirtualAddress;
                if (rl->target_is_end)
                    target_value += aot_sec_size(&secs[rl->target_section]);
                else
                    target_value += rl->target_off;
            }
            target_value = (uint64_t)((int64_t)target_value + rl->addend);
            uint64_t site_va = image_base +
                pe.sectionHeaders[rl->site_section].VirtualAddress + rl->site_off;
            uint32_t width = (rl->kind == AOT_RELOC_ABS64 ||
                              rl->kind == AOT_RELOC_IMM64) ? 8u : 4u;
            if (rl->site_off + width > secs[rl->site_section].size) {
                set_err(err, err_cap, "aot_emit_pe: reloc fuera de la seccion del sitio");
                freePE64File(&pe); return 0;
            }
            if (!apply_reloc(pe.sectionData[rl->site_section] + rl->site_off,
                             site_va, target_value, rl->kind)) {
                set_err(err, err_cap, "aot_emit_pe: reloc kind invalido");
                freePE64File(&pe); return 0;
            }
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
                 const AotReloc *relocs, int num_relocs,
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

    /* Para resolver relocations tras el layout: VA + offset-en-fichero de cada
     * seccion (indexado por el indice de usuario).  Solo si hay relocs. */
    uint64_t *sec_va   = NULL;
    uint64_t *sec_foff = NULL;
    uint8_t  *sec_seen = NULL;
    if (relocs && num_relocs > 0) {
        sec_va   = (uint64_t *)calloc((size_t)num_secs, sizeof(uint64_t));
        sec_foff = (uint64_t *)calloc((size_t)num_secs, sizeof(uint64_t));
        sec_seen = (uint8_t  *)calloc((size_t)num_secs, sizeof(uint8_t));
    }

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
            free(sec_va); free(sec_foff); free(sec_seen);
            elf_builder_free(b);
            set_err(err, err_cap, "aot_emit_elf: fallo anyadiendo seccion");
            return 0;
        }
        if (sec_va) { sec_va[i] = vaddr_out; sec_foff[i] = off_out; sec_seen[i] = 1; }
        if (i == entry_sec) entry_vaddr = vaddr_out;
    }
    if (entry_vaddr == 0) {
        free(sec_va); free(sec_foff); free(sec_seen);
        elf_builder_free(b);
        set_err(err, err_cap, "aot_emit_elf: la seccion de entrada no tiene datos");
        return 0;
    }

    /* Resolver relocations cross-seccion (mismo modelo que PE): el layout ya
     * fijo VA + offset-en-fichero de cada seccion -> parcheamos b->mem. */
    if (relocs && num_relocs > 0) {
        for (int r = 0; r < num_relocs; ++r) {
            const AotReloc *rl = &relocs[r];
            if (rl->site_section < 0 || rl->site_section >= num_secs ||
                rl->target_section < 0 || rl->target_section >= num_secs ||
                !sec_seen[rl->site_section] ||
                (!rl->target_is_size && !sec_seen[rl->target_section])) {
                free(sec_va); free(sec_foff); free(sec_seen);
                elf_builder_free(b);
                set_err(err, err_cap, "aot_emit_elf: reloc con seccion invalida");
                return 0;
            }
            uint64_t target_value;
            if (rl->target_is_size) {
                target_value = aot_sec_size(&secs[rl->target_section]);
            } else {
                target_value = sec_va[rl->target_section];
                if (rl->target_is_end)
                    target_value += aot_sec_size(&secs[rl->target_section]);
                else
                    target_value += rl->target_off;
            }
            target_value = (uint64_t)((int64_t)target_value + rl->addend);
            uint64_t site_va = sec_va[rl->site_section] + rl->site_off;
            uint32_t width = (rl->kind == AOT_RELOC_ABS64 ||
                              rl->kind == AOT_RELOC_IMM64) ? 8u : 4u;
            if (rl->site_off + width > secs[rl->site_section].size) {
                free(sec_va); free(sec_foff); free(sec_seen);
                elf_builder_free(b);
                set_err(err, err_cap, "aot_emit_elf: reloc fuera de la seccion del sitio");
                return 0;
            }
            if (!apply_reloc(b->mem + sec_foff[rl->site_section] + rl->site_off,
                             site_va, target_value, rl->kind)) {
                free(sec_va); free(sec_foff); free(sec_seen);
                elf_builder_free(b);
                set_err(err, err_cap, "aot_emit_elf: reloc kind invalido");
                return 0;
            }
        }
    }
    free(sec_va); free(sec_foff); free(sec_seen);
    sec_va = NULL; sec_foff = NULL; sec_seen = NULL;

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

/* =========================================================================
 *  ELF64 RELOCATABLE (.o, ET_REL) -- hand-rolled (sin _start ni phdrs)
 * ========================================================================= */

/* Buffer de salida con append + alineacion. */
typedef struct { uint8_t *p; size_t len, cap; } OBuf;
static int ob_reserve(OBuf *o, size_t extra) {
    if (o->len + extra <= o->cap) return 1;
    size_t nc = o->cap ? o->cap * 2 : 4096;
    while (nc < o->len + extra) nc *= 2;
    uint8_t *np = (uint8_t *)realloc(o->p, nc);
    if (!np) return 0;
    o->p = np; o->cap = nc; return 1;
}
static int ob_put(OBuf *o, const void *data, size_t n) {
    if (!ob_reserve(o, n)) return 0;
    if (n) { if (data) memcpy(o->p + o->len, data, n); else memset(o->p + o->len, 0, n); }
    o->len += n; return 1;
}
static int ob_align(OBuf *o, size_t a) {
    while (o->len % a) { uint8_t z = 0; if (!ob_put(o, &z, 1)) return 0; }
    return 1;
}

int aot_emit_elf_obj(const char *path,
                     const AotSection *secs, int num_secs,
                     const AotReloc *relocs, int num_relocs,
                     const AotSym *syms, int num_syms,
                     char *err, size_t err_cap) {
    if (num_secs <= 0) { set_err(err, err_cap, "aot_emit_elf_obj: sin secciones"); return 0; }

    /* v1: solo refs ADDR (datos/llamadas).  SIZE/END (simbolos de seccion)
     * no se soportan en .o todavia. */
    for (int r = 0; r < num_relocs; ++r) {
        if (relocs[r].target_is_size || relocs[r].target_is_end) {
            set_err(err, err_cap, "aot_emit_elf_obj: SIZE/END no soportado en .o (v1)");
            return 0;
        }
        if (relocs[r].kind != AOT_RELOC_REL32 && relocs[r].kind != AOT_RELOC_ABS64) {
            set_err(err, err_cap, "aot_emit_elf_obj: reloc kind no soportado en .o");
            return 0;
        }
    }

    /* Indices de section headers:
     *   0            = NULL
     *   1..num_secs  = secciones de usuario (PROGBITS)
     *   sym_sh       = .symtab
     *   str_sh       = .strtab
     *   rela_sh[i]   = .rela de la seccion de usuario i (si tiene relocs)
     *   shstr_sh     = .shstrtab
     */
    int *sec_nrel = (int *)calloc((size_t)num_secs, sizeof(int));
    if (!sec_nrel) { set_err(err, err_cap, "oom"); return 0; }
    for (int r = 0; r < num_relocs; ++r) {
        int s = relocs[r].site_section;
        if (s < 0 || s >= num_secs) { free(sec_nrel);
            set_err(err, err_cap, "aot_emit_elf_obj: site_section invalido"); return 0; }
        sec_nrel[s]++;
    }
    const int sym_sh = 1 + num_secs;
    const int str_sh = sym_sh + 1;
    int *rela_sh = (int *)calloc((size_t)num_secs, sizeof(int));
    if (!rela_sh) { free(sec_nrel); set_err(err, err_cap, "oom"); return 0; }
    int next_sh = str_sh + 1;
    for (int i = 0; i < num_secs; ++i)
        if (sec_nrel[i] > 0) rela_sh[i] = next_sh++;
    const int shstr_sh = next_sh++;
    const int shnum = next_sh;

    /* .strtab (nombres de simbolos) + symtab.  Simbolos:
     *   [0]            null
     *   [1..num_secs]  STT_SECTION (LOCAL) por seccion de usuario
     *   [globales...]  STB_GLOBAL (syms[])  -- DESPUES de los locales. */
    OBuf strtab; memset(&strtab, 0, sizeof(strtab));
    { uint8_t z = 0; ob_put(&strtab, &z, 1); }  /* [0] = "" */
    const int nsym = 1 + num_secs + num_syms;
    Elf64_Sym *symtab = (Elf64_Sym *)calloc((size_t)nsym, sizeof(Elf64_Sym));
    if (!symtab) { free(sec_nrel); free(rela_sh); free(strtab.p);
        set_err(err, err_cap, "oom"); return 0; }
    for (int i = 0; i < num_secs; ++i) {
        symtab[1 + i].st_info  = ELF64_ST_INFO(STB_LOCAL, STT_SECTION);
        symtab[1 + i].st_shndx = (Elf64_Half)(1 + i);
    }
    for (int g = 0; g < num_syms; ++g) {
        Elf64_Sym *s = &symtab[1 + num_secs + g];
        s->st_name  = (Elf64_Word)strtab.len;
        ob_put(&strtab, syms[g].name, strlen(syms[g].name) + 1);
        s->st_info  = ELF64_ST_INFO(STB_GLOBAL, syms[g].is_func ? STT_FUNC : STT_OBJECT);
        s->st_shndx = (Elf64_Half)(1 + syms[g].section);
        s->st_value = syms[g].offset;
    }
    const int first_global = 1 + num_secs;  /* symtab sh_info */

    /* .rela arrays por seccion. */
    Elf64_Rela **rela = (Elf64_Rela **)calloc((size_t)num_secs, sizeof(Elf64_Rela *));
    int *rela_n = (int *)calloc((size_t)num_secs, sizeof(int));
    if (!rela || !rela_n) { free(sec_nrel); free(rela_sh); free(strtab.p); free(symtab);
        free(rela); free(rela_n); set_err(err, err_cap, "oom"); return 0; }
    for (int i = 0; i < num_secs; ++i)
        if (sec_nrel[i] > 0)
            rela[i] = (Elf64_Rela *)calloc((size_t)sec_nrel[i], sizeof(Elf64_Rela));
    for (int r = 0; r < num_relocs; ++r) {
        const AotReloc *rl = &relocs[r];
        Elf64_Rela *re = &rela[rl->site_section][rela_n[rl->site_section]++];
        re->r_offset = rl->site_off;
        const uint32_t sym_idx = (uint32_t)(1 + rl->target_section);  /* section symbol */
        if (rl->kind == AOT_RELOC_REL32) {
            re->r_info   = ELF64_R_INFO(sym_idx, R_X86_64_PC32);
            re->r_addend = (Elf64_Sxword)rl->target_off - 4 + rl->addend;
        } else { /* ABS64 */
            re->r_info   = ELF64_R_INFO(sym_idx, R_X86_64_64);
            re->r_addend = (Elf64_Sxword)rl->target_off + rl->addend;
        }
    }

    /* .shstrtab + nombres de seccion. */
    OBuf shstr; memset(&shstr, 0, sizeof(shstr));
    { uint8_t z = 0; ob_put(&shstr, &z, 1); }
    uint32_t *sec_nameoff  = (uint32_t *)calloc((size_t)num_secs, sizeof(uint32_t));
    uint32_t *rela_nameoff = (uint32_t *)calloc((size_t)num_secs, sizeof(uint32_t));
    for (int i = 0; i < num_secs; ++i) {
        sec_nameoff[i] = (uint32_t)shstr.len;
        ob_put(&shstr, secs[i].name, strlen(secs[i].name) + 1);
    }
    uint32_t no_symtab = (uint32_t)shstr.len; ob_put(&shstr, ".symtab", 8);
    uint32_t no_strtab = (uint32_t)shstr.len; ob_put(&shstr, ".strtab", 8);
    for (int i = 0; i < num_secs; ++i) {
        if (!rela_sh[i]) continue;
        rela_nameoff[i] = (uint32_t)shstr.len;
        ob_put(&shstr, ".rela", 5);
        ob_put(&shstr, secs[i].name, strlen(secs[i].name) + 1);
    }
    uint32_t no_shstr = (uint32_t)shstr.len; ob_put(&shstr, ".shstrtab", 10);

    /* Construir el fichero: [ehdr][secs][symtab][strtab][rela...][shstrtab][shdrs]. */
    OBuf out; memset(&out, 0, sizeof(out));
    Elf64_Ehdr eh; memset(&eh, 0, sizeof(eh));
    eh.e_ident[0]=0x7f; eh.e_ident[1]='E'; eh.e_ident[2]='L'; eh.e_ident[3]='F';
    eh.e_ident[4]=2; eh.e_ident[5]=1; eh.e_ident[6]=1;  /* ELF64, LSB, version 1 */
    eh.e_type = ET_REL; eh.e_machine = EM_X86_64; eh.e_version = 1;
    eh.e_ehsize = (Elf64_Half)sizeof(Elf64_Ehdr);
    eh.e_shentsize = (Elf64_Half)sizeof(Elf64_Shdr);
    eh.e_shnum = (Elf64_Half)shnum;
    eh.e_shstrndx = (Elf64_Half)shstr_sh;
    ob_put(&out, &eh, sizeof(eh));

    uint64_t *sec_off = (uint64_t *)calloc((size_t)num_secs, sizeof(uint64_t));
    for (int i = 0; i < num_secs; ++i) {
        ob_align(&out, 16);
        sec_off[i] = out.len;
        ob_put(&out, secs[i].data, secs[i].size);
    }
    ob_align(&out, 8);
    uint64_t symtab_off = out.len;
    ob_put(&out, symtab, (size_t)nsym * sizeof(Elf64_Sym));
    uint64_t strtab_off = out.len;
    ob_put(&out, strtab.p, strtab.len);
    uint64_t *rela_off = (uint64_t *)calloc((size_t)num_secs, sizeof(uint64_t));
    for (int i = 0; i < num_secs; ++i) {
        if (!rela_sh[i]) continue;
        ob_align(&out, 8);
        rela_off[i] = out.len;
        ob_put(&out, rela[i], (size_t)rela_n[i] * sizeof(Elf64_Rela));
    }
    uint64_t shstr_off = out.len;
    ob_put(&out, shstr.p, shstr.len);

    ob_align(&out, 8);
    uint64_t shoff = out.len;
    Elf64_Shdr *shdr = (Elf64_Shdr *)calloc((size_t)shnum, sizeof(Elf64_Shdr));
    for (int i = 0; i < num_secs; ++i) {
        Elf64_Shdr *s = &shdr[1 + i];
        s->sh_name = sec_nameoff[i];
        s->sh_type = SHT_PROGBITS;
        s->sh_flags = SHF_ALLOC;
        if (secs[i].flags & AOT_SEC_EXEC)  s->sh_flags |= SHF_EXECINSTR;
        if (secs[i].flags & AOT_SEC_WRITE) s->sh_flags |= SHF_WRITE;
        s->sh_offset = sec_off[i];
        s->sh_size = secs[i].size;
        s->sh_addralign = (secs[i].flags & AOT_SEC_EXEC) ? 16 : 8;
    }
    { Elf64_Shdr *s = &shdr[sym_sh];
      s->sh_name = no_symtab; s->sh_type = SHT_SYMTAB;
      s->sh_offset = symtab_off; s->sh_size = (uint64_t)nsym * sizeof(Elf64_Sym);
      s->sh_link = (Elf64_Word)str_sh; s->sh_info = (Elf64_Word)first_global;
      s->sh_addralign = 8; s->sh_entsize = sizeof(Elf64_Sym); }
    { Elf64_Shdr *s = &shdr[str_sh];
      s->sh_name = no_strtab; s->sh_type = SHT_STRTAB;
      s->sh_offset = strtab_off; s->sh_size = strtab.len; s->sh_addralign = 1; }
    for (int i = 0; i < num_secs; ++i) {
        if (!rela_sh[i]) continue;
        Elf64_Shdr *s = &shdr[rela_sh[i]];
        s->sh_name = rela_nameoff[i]; s->sh_type = SHT_RELA;
        s->sh_offset = rela_off[i];
        s->sh_size = (uint64_t)rela_n[i] * sizeof(Elf64_Rela);
        s->sh_link = (Elf64_Word)sym_sh;       /* simbolos */
        s->sh_info = (Elf64_Word)(1 + i);      /* seccion a la que aplica */
        s->sh_addralign = 8; s->sh_entsize = sizeof(Elf64_Rela);
    }
    { Elf64_Shdr *s = &shdr[shstr_sh];
      s->sh_name = no_shstr; s->sh_type = SHT_STRTAB;
      s->sh_offset = shstr_off; s->sh_size = shstr.len; s->sh_addralign = 1; }
    ob_put(&out, shdr, (size_t)shnum * sizeof(Elf64_Shdr));

    ((Elf64_Ehdr *)out.p)->e_shoff = shoff;

    int ok = 1;
    FILE *f = fopen(path, "wb");
    if (!f) { set_err(err, err_cap, "aot_emit_elf_obj: fopen fallo"); ok = 0; }
    else {
        size_t w = fwrite(out.p, 1, out.len, f);
        fclose(f);
        if (w != out.len) { set_err(err, err_cap, "aot_emit_elf_obj: escritura incompleta"); ok = 0; }
    }

    free(sec_nrel); free(rela_sh); free(strtab.p); free(symtab);
    for (int i = 0; i < num_secs; ++i) free(rela[i]);
    free(rela); free(rela_n); free(shstr.p); free(sec_nameoff);
    free(rela_nameoff); free(sec_off); free(rela_off); free(shdr); free(out.p);
    return ok;
}
