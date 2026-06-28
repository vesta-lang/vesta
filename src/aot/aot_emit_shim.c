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
#include "LibCOFFparse.h"
#include "LibPEparse.h" // lectura de exports de DLL (aot_pe_export_names)

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
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static void wr64le(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        p[i] = (uint8_t)(v >> (i * 8));
}

/* Aplica UNA relocation ya resuelta: @p target_value es la direccion (ADDR) o
 * el tamano (SIZE) del objetivo + addend; @p site_va es la VA del sitio (para
 * el rel32 PC-relativo); @p site escribe en el buffer de la seccion del sitio
 * en su offset.  Devuelve 0 en error de tamano/kind. */
static int apply_reloc(uint8_t *site, uint64_t site_va, uint64_t target_value,
                       int kind) {
    switch (kind) {
    case AOT_RELOC_REL32: {
        int64_t rel = (int64_t)target_value - (int64_t)(site_va + 4);
        wr32le(site, (uint32_t)(int32_t)rel);
        return 1;
    }
    case AOT_RELOC_IMM32: wr32le(site, (uint32_t)target_value); return 1;
    case AOT_RELOC_ABS64:
    case AOT_RELOC_IMM64: wr64le(site, target_value); return 1;
    default: return 0;
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
    if (flags & AOT_SEC_BSS) c |= ___IMAGE_SCN_CNT_UNINITIALIZED_DATA;
    if (flags & AOT_SEC_EXEC) c |= ___IMAGE_SCN_MEM_EXECUTE;
    if (flags & AOT_SEC_READ) c |= ___IMAGE_SCN_MEM_READ;
    if (flags & AOT_SEC_WRITE) c |= ___IMAGE_SCN_MEM_WRITE;
    return c;
}

/* Compara dos nombres de DLL ignorando mayusculas/minusculas (ASCII).  Windows
 * resuelve los imports case-insensitive, asi que "KERNEL32.dll" y
 * "kernel32.dll" son la MISMA libreria y deben agruparse en un solo
 * descriptor de import. */
static int aot_dll_name_eq(const char *a, const char *b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    for (; *a && *b; ++a, ++b) {
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
    }
    return *a == 0 && *b == 0;
}

/* TLS (thread_local) PE: si alguna seccion de usuario es SHF_TLS, sintetiza la
 * infraestructura TLS NATIVA de Windows -- una seccion `.tls$d` con _tls_index
 * (lo rellena el cargador con el indice del slot), un array de callbacks vacio,
 * y el IMAGE_TLS_DIRECTORY -- y pone DataDirectory[9] (TLS) apuntando ahi.  El
 * acceso (mov gs:[0x58] -> [_tls_index] -> bloque -> +offset) lo emite el
 * codegen; el reloc al simbolo `__vex_tls_index` se resuelve a la VA que
 * devuelve esta funcion.  Compartido por aot_emit_pe (.exe) y aot_emit_pe_dll
 * (.dll): el cargador procesa el directorio TLS en ambos (Vista+).
 * @return VA de _tls_index, o 0 si el modulo no tiene TLS. */
static uint64_t aot_pe_synth_tls(PE64FILE_struct *pe, const AotSection *secs,
                                 int num_secs, int is_dll, int cb_section,
                                 uint32_t cb_off) {
    int tls_sec = -1;
    for (int i = 0; i < num_secs; ++i)
        if (secs[i].flags & AOT_SEC_TLS) {
            tls_sec = i;
            break;
        }
    if (tls_sec < 0) return 0;
    const uint64_t image_base = pe->ntHeaders.OptionalHeader.ImageBase;
    /* Layout de .tls$d (64 B): [0]=_tls_index(4) [4]=pad
     * [8]=array de callbacks [&__vex_tls_init, NULL] (16 B)
     * [24]=IMAGE_TLS_DIRECTORY64(40). */
    _BYTE tlsd[64];
    memset(tlsd, 0, sizeof(tlsd));
    int tlsd_idx = addSection(pe, ".tls$d",
                              ___IMAGE_SCN_CNT_INITIALIZED_DATA |
                                  ___IMAGE_SCN_MEM_READ | ___IMAGE_SCN_MEM_WRITE,
                              tlsd, (_DWORD)sizeof(tlsd));
    const uint32_t tls_rva = pe->sectionHeaders[tls_sec].VirtualAddress;
    const uint32_t tlsd_rva = pe->sectionHeaders[tlsd_idx].VirtualAddress;
    _BYTE *sd = pe->sectionData[tlsd_idx];
    /* Array de callbacks en +8: [&__vex_tls_init, NULL].  El callback aplica la
     * plantilla por-hilo (el cargador no siempre la copia para el TLS de una
     * .dll en un consumidor minimal). */
    int have_cb = (cb_section >= 0 && cb_section < pe->numberOfSections);
    if (have_cb) {
        uint64_t cb_va = image_base +
                         pe->sectionHeaders[cb_section].VirtualAddress + cb_off;
        memcpy(sd + 8, &cb_va, 8); /* [+8] = &__vex_tls_init ; [+16] = NULL */
    }
    _BYTE *db = sd + 24; /* IMAGE_TLS_DIRECTORY */
    uint64_t start = image_base + tls_rva;
    uint64_t end = image_base + tls_rva + secs[tls_sec].size;
    uint64_t idx_va = image_base + tlsd_rva + 0; /* &_tls_index */
    uint64_t cbarr_va = image_base + tlsd_rva + 8; /* &array de callbacks */
    memcpy(db + 0, &start, 8);
    memcpy(db + 8, &end, 8);
    memcpy(db + 16, &idx_va, 8);
    memcpy(db + 24, &cbarr_va, 8);
    uint32_t zfill = (uint32_t)secs[tls_sec].bss_size;
    memcpy(db + 32, &zfill, 4); /* SizeOfZeroFill */
    uint32_t tls_char = 0x00400000u; /* IMAGE_SCN_ALIGN_8BYTES */
    memcpy(db + 36, &tls_char, 4);   /* Characteristics (alineamiento) */
    pe->ntHeaders.OptionalHeader.DataDirectory[___IMAGE_DIRECTORY_ENTRY_TLS]
        .VirtualAddress = tlsd_rva + 24;
    pe->ntHeaders.OptionalHeader.DataDirectory[___IMAGE_DIRECTORY_ENTRY_TLS]
        .Size = 40;
    /* Campos VA ABSOLUTOS: el callback (+8) y los 4 del directorio (Start/End/
     * Index/Callbacks en +24/+32/+40/+48).  En un EXE fijamos la base (sin
     * ASLR); en una DLL -- que el cargador reubica -- emitimos una .reloc
     * (IMAGE_REL_BASED_DIR64) para que el loader los fije al relocar. */
    if (is_dll) {
        const uint32_t page = tlsd_rva & ~0xFFFu;
        _BYTE rel[24];
        uint32_t nrelocs = 4 + (have_cb ? 1 : 0);
        uint32_t blksz = 8u + nrelocs * 2u;
        if (blksz & 3u) blksz += 2u; /* bloques .reloc alineados a 4 (pad 0) */
        memset(rel, 0, sizeof(rel));
        memcpy(rel + 0, &page, 4);
        memcpy(rel + 4, &blksz, 4);
        uint32_t voff[5];
        int nv = 0;
        if (have_cb) voff[nv++] = tlsd_rva + 8; /* callback */
        voff[nv++] = tlsd_rva + 24;             /* Start */
        voff[nv++] = tlsd_rva + 32;             /* End */
        voff[nv++] = tlsd_rva + 40;             /* Index */
        voff[nv++] = tlsd_rva + 48;             /* Callbacks */
        for (int k = 0; k < nv; ++k) {
            uint16_t e = (uint16_t)((10u << 12) | ((voff[k] - page) & 0xFFFu));
            memcpy(rel + 8 + k * 2, &e, 2);
        }
        int rel_idx =
            addSection(pe, ".reloc",
                       ___IMAGE_SCN_CNT_INITIALIZED_DATA |
                           ___IMAGE_SCN_MEM_READ | 0x02000000u /* DISCARDABLE */,
                       rel, blksz);
        const uint32_t rel_rva = pe->sectionHeaders[rel_idx].VirtualAddress;
        pe->ntHeaders.OptionalHeader
            .DataDirectory[___IMAGE_DIRECTORY_ENTRY_BASERELOC]
            .VirtualAddress = rel_rva;
        pe->ntHeaders.OptionalHeader
            .DataDirectory[___IMAGE_DIRECTORY_ENTRY_BASERELOC]
            .Size = blksz;
    } else {
        pe->ntHeaders.OptionalHeader.DllCharacteristics &=
            (uint16_t)~___IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE;
    }
    return idx_va;
}

/* TLS PE: resuelve un reloc al simbolo `__vex_tls_index` (RIP-relativo) o un
 * SECREL32 (offset del var dentro de su seccion .tls).  @return 1 si lo manejo
 * (el caller debe `continue`), 0 si no es un reloc TLS. */
static int aot_pe_apply_tls_reloc(PE64FILE_struct *pe, const AotSection *secs,
                                  int num_secs, const AotReloc *rl,
                                  uint64_t tls_index_va) {
    const uint64_t image_base = pe->ntHeaders.OptionalHeader.ImageBase;
    if (rl->extern_name && strcmp(rl->extern_name, "__vex_tls_index") == 0) {
        if (tls_index_va == 0 || rl->site_section < 0 ||
            rl->site_section >= num_secs)
            return 1; /* manejado (silenciosamente no-op si malformado) */
        (void)secs;
        uint64_t sva = image_base +
                       pe->sectionHeaders[rl->site_section].VirtualAddress +
                       rl->site_off;
        int32_t disp =
            (int32_t)((int64_t)tls_index_va - (int64_t)(sva + 4) + rl->addend);
        memcpy(pe->sectionData[rl->site_section] + rl->site_off, &disp, 4);
        return 1;
    }
    if (rl->kind == AOT_RELOC_SECREL32) {
        if (rl->site_section < 0 || rl->site_section >= num_secs) return 1;
        int32_t off = (int32_t)((int64_t)rl->target_off + rl->addend);
        memcpy(pe->sectionData[rl->site_section] + rl->site_off, &off, 4);
        return 1;
    }
    return 0;
}

int aot_emit_pe(const char *path, const AotLayoutCfg *cfg,
                const AotSection *secs, int num_secs, int entry_sec,
                uint64_t entry_off, const AotImport *imps, int num_imps,
                const AotReloc *relocs, int num_relocs, char *err,
                size_t err_cap) {
    if (num_secs <= 0) {
        set_err(err, err_cap, "aot_emit_pe: sin secciones");
        return 0;
    }
    if (entry_sec < 0 || entry_sec >= num_secs) {
        set_err(err, err_cap, "aot_emit_pe: entry_sec fuera de rango");
        return 0;
    }

    PE64FILE_struct pe;
    initializePE64File(&pe);

    /* Aplicar configuracion de layout (campos en 0 => se deja el default). */
    if (cfg) {
        if (cfg->image_base)
            pe.ntHeaders.OptionalHeader.ImageBase = cfg->image_base;
        if (cfg->section_align)
            pe.ntHeaders.OptionalHeader.SectionAlignment = cfg->section_align;
        if (cfg->file_align)
            pe.ntHeaders.OptionalHeader.FileAlignment = cfg->file_align;
        if (cfg->stack_reserve)
            pe.ntHeaders.OptionalHeader.SizeOfStackReserve = cfg->stack_reserve;
        if (cfg->stack_commit)
            pe.ntHeaders.OptionalHeader.SizeOfStackCommit = cfg->stack_commit;
        if (cfg->heap_reserve)
            pe.ntHeaders.OptionalHeader.SizeOfHeapReserve = cfg->heap_reserve;
        if (cfg->heap_commit)
            pe.ntHeaders.OptionalHeader.SizeOfHeapCommit = cfg->heap_commit;
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

    /* TLS (thread_local): sintetizar el IMAGE_TLS_DIRECTORY + _tls_index si hay
     * seccion SHF_TLS (devuelve la VA de _tls_index para resolver los relocs). */
    uint64_t tls_index_va = aot_pe_synth_tls(&pe, secs, num_secs, 0, cfg ? cfg->tls_callback_section : -1, cfg ? cfg->tls_callback_off : 0);

    /* Relocations cross-seccion: resolver AHORA que addSection ya asigno las
     * VirtualAddress de todas las secciones del usuario (igual que el parcheo
     * de imports, pero generico).  ImageBase + sectionHeaders[i].VirtualAddress
     * es la VA de la seccion i; pe.sectionData[i] es su buffer mutable. */
    if (relocs && num_relocs > 0) {
        const uint64_t image_base = pe.ntHeaders.OptionalHeader.ImageBase;
        /* Si hay refs ABSOLUTAS (ABS64/IMM64, p.ej. --no-pie), la imagen DEBE
         * cargar en su ImageBase: sin tabla .reloc, el loader no puede
         * reubicar. Limpiamos DYNAMIC_BASE (ASLR) -> base fija (semantica
         * -no-pie).  Las refs RIP-relativas (DATA_REL32) no lo necesitan
         * (position-independent). */
        for (int r = 0; r < num_relocs; ++r) {
            if (relocs[r].kind == AOT_RELOC_ABS64 ||
                relocs[r].kind == AOT_RELOC_IMM64) {
                pe.ntHeaders.OptionalHeader.DllCharacteristics &=
                    (uint16_t)~___IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE;
                break;
            }
        }
        for (int r = 0; r < num_relocs; ++r) {
            const AotReloc *rl = &relocs[r];
            /* TLS PE: `__vex_tls_index` (RIP-rel al slot) + SECREL32 (offset del
             * var en .tls).  Manejados por el helper compartido. */
            if (aot_pe_apply_tls_reloc(&pe, secs, num_secs, rl, tls_index_va))
                continue;
            if (rl->site_section < 0 || rl->site_section >= num_secs ||
                rl->target_section < 0 || rl->target_section >= num_secs) {
                set_err(err, err_cap,
                        "aot_emit_pe: reloc con seccion fuera de rango");
                freePE64File(&pe);
                return 0;
            }
            uint64_t target_value;
            if (rl->target_is_size) {
                target_value = aot_sec_size(&secs[rl->target_section]);
            } else {
                target_value =
                    image_base +
                    pe.sectionHeaders[rl->target_section].VirtualAddress;
                if (rl->target_is_end)
                    target_value += aot_sec_size(&secs[rl->target_section]);
                else
                    target_value += rl->target_off;
            }
            target_value = (uint64_t)((int64_t)target_value + rl->addend);
            uint64_t site_va =
                image_base +
                pe.sectionHeaders[rl->site_section].VirtualAddress +
                rl->site_off;
            uint32_t width =
                (rl->kind == AOT_RELOC_ABS64 || rl->kind == AOT_RELOC_IMM64)
                    ? 8u
                    : 4u;
            if (rl->site_off + width > secs[rl->site_section].size) {
                set_err(err, err_cap,
                        "aot_emit_pe: reloc fuera de la seccion del sitio");
                freePE64File(&pe);
                return 0;
            }
            if (!apply_reloc(pe.sectionData[rl->site_section] + rl->site_off,
                             site_va, target_value, rl->kind)) {
                set_err(err, err_cap, "aot_emit_pe: reloc kind invalido");
                freePE64File(&pe);
                return 0;
            }
        }
    }

    /* Imports -> .idata + IAT + parcheo de los call. */
    if (imps && num_imps > 0) {
        const ___IMAGE_SECTION_HEADER *last =
            &pe.sectionHeaders[pe.numberOfSections - 1];
        _DWORD idataRVA = align(last->VirtualAddress + last->Misc.VirtualSize,
                                pe.ntHeaders.OptionalHeader.SectionAlignment);

        /* Agrupar por DLL preservando orden + dedup de funciones. */
        int max_libs = num_imps;
        const char **lib_names =
            (const char **)calloc(max_libs, sizeof(char *));
        const char ***lib_funcs =
            (const char ***)calloc(max_libs, sizeof(char **));
        int *lib_nfuncs = (int *)calloc(max_libs, sizeof(int));
        int num_libs = 0;
        for (int k = 0; k < num_imps; ++k) {
            int di = -1;
            for (int j = 0; j < num_libs; ++j)
                /* DLL names case-insensitive (Windows): el stub usa
                 * "KERNEL32.dll" y un FFI extern puede usar "kernel32.dll" ->
                 * deben fundirse en UN solo descriptor de import. */
                if (aot_dll_name_eq(lib_names[j], imps[k].dll)) {
                    di = j;
                    break;
                }
            if (di < 0) {
                di = num_libs++;
                lib_names[di] = imps[k].dll;
                lib_funcs[di] = (const char **)calloc(num_imps, sizeof(char *));
                lib_nfuncs[di] = 0;
            }
            int dup = 0;
            for (int j = 0; j < lib_nfuncs[di]; ++j)
                if (strcmp(lib_funcs[di][j], imps[k].func) == 0) {
                    dup = 1;
                    break;
                }
            if (!dup) lib_funcs[di][lib_nfuncs[di]++] = imps[k].func;
        }

        ImportLibrary *libs =
            (ImportLibrary *)calloc(num_libs, sizeof(ImportLibrary));
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
            idata_idx =
                addSection(&pe, ".idata",
                           ___IMAGE_SCN_CNT_INITIALIZED_DATA |
                               ___IMAGE_SCN_MEM_READ | ___IMAGE_SCN_MEM_WRITE,
                           idata_buf, idata_size);

            pe.ntHeaders.OptionalHeader
                .DataDirectory[___IMAGE_DIRECTORY_ENTRY_IMPORT]
                .VirtualAddress = idataRVA;
            pe.ntHeaders.OptionalHeader
                .DataDirectory[___IMAGE_DIRECTORY_ENTRY_IMPORT]
                .Size =
                (_DWORD)(sizeof(___IMAGE_IMPORT_DESCRIPTOR) * (num_libs + 1));
            if (num_off > 0) {
                pe.ntHeaders.OptionalHeader
                    .DataDirectory[___IMAGE_DIRECTORY_ENTRY_IAT]
                    .VirtualAddress = idataRVA + (_DWORD)off_entries[0].offset;
                pe.ntHeaders.OptionalHeader
                    .DataDirectory[___IMAGE_DIRECTORY_ENTRY_IAT]
                    .Size = (_DWORD)(sizeof(_QWORD) * num_off);
            }

            /* Parchear cada call importado.  El offset_iat es relativo a
             * idataRVA, asi que la base que se pasa es la VA de .idata. */
            uint64_t idata_base =
                pe.ntHeaders.OptionalHeader.ImageBase + idataRVA;
            FunctionOffset *fix =
                (FunctionOffset *)calloc(num_imps, sizeof(FunctionOffset));
            int nfix = 0;
            int ok = 1;
            for (int k = 0; k < num_imps && ok; ++k) {
                if (imps[k].call_section < 0 ||
                    imps[k].call_section >= num_secs) {
                    set_err(err, err_cap, "aot_emit_pe: call_section invalido");
                    ok = 0;
                    break;
                }
                int iat_off = -1;
                for (int e = 0; e < num_off; ++e)
                    if (strcmp(imps[k].func, off_entries[e].functionName) ==
                            0 &&
                        /* DLL case-insensitive: el grouping fundio KERNEL32.dll
                         * y kernel32.dll en un descriptor; el lookup debe
                         * coincidir igual aunque difiera el case. */
                        aot_dll_name_eq(imps[k].dll, off_entries[e].dllName)) {
                        iat_off = off_entries[e].offset;
                        break;
                    }
                if (iat_off < 0) {
                    set_err(err, err_cap, "aot_emit_pe: import no resuelto");
                    ok = 0;
                    break;
                }
                /* Parchear inmediatamente (cada call puede estar en una seccion
                 * distinta -> base de codigo propia). */
                int cs = imps[k].call_section;
                uint64_t code_vaddr = pe.sectionHeaders[cs].VirtualAddress +
                                      pe.ntHeaders.OptionalHeader.ImageBase;
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
                for (int j = 0; j < num_libs; ++j)
                    free(lib_funcs[j]);
                free(lib_names);
                free(lib_funcs);
                free(lib_nfuncs);
                free(libs);
                freePE64File(&pe);
                return 0;
            }
            (void)idata_idx;
        } else {
            set_err(err, err_cap, "aot_emit_pe: fallo construyendo .idata");
            for (int j = 0; j < num_libs; ++j)
                free(lib_funcs[j]);
            free(lib_names);
            free(lib_funcs);
            free(lib_nfuncs);
            free(libs);
            freePE64File(&pe);
            return 0;
        }

        for (int j = 0; j < num_libs; ++j)
            free(lib_funcs[j]);
        free(lib_names);
        free(lib_funcs);
        free(lib_nfuncs);
        free(libs);
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
                 const AotSection *secs, int num_secs, int entry_sec,
                 uint64_t entry_off, const AotReloc *relocs, int num_relocs,
                 char *err, size_t err_cap) {
    if (num_secs <= 0) {
        set_err(err, err_cap, "aot_emit_elf: sin secciones");
        return 0;
    }
    if (entry_sec < 0 || entry_sec >= num_secs) {
        set_err(err, err_cap, "aot_emit_elf: entry_sec fuera de rango");
        return 0;
    }

    uint64_t base = (cfg && cfg->image_base) ? cfg->image_base : 0x400000ull;
    uint64_t stack_vaddr =
        (cfg && cfg->elf_stack_vaddr) ? cfg->elf_stack_vaddr : 0x70000000ull;
    uint64_t stack_size =
        (cfg && cfg->elf_stack_size) ? cfg->elf_stack_size : 0x10000ull;

    size_t total_data = 0;
    for (int i = 0; i < num_secs; ++i)
        total_data += secs[i].size;
    /* Si hay secciones con VA fija (place_section del link-script), el fichero
     * se rellena hasta vaddr-base; ampliar la capacidad para cubrir el span. */
    uint64_t span = total_data;
    for (int i = 0; i < num_secs; ++i)
        if (secs[i].vaddr && secs[i].vaddr >= base) {
            uint64_t end = (secs[i].vaddr - base) + secs[i].size;
            if (end > span) span = end;
        }
    size_t capacity =
        (span + 64 * AOT_ELF_PAGE + AOT_ELF_PAGE) & ~(AOT_ELF_PAGE - 1);

    /* 3 phdrs: PT_LOAD R+X (codigo/rodata), PT_LOAD R+W (.data), PT_GNU_STACK
     * (stack RW).  El R+W puede quedar vacio (filesz 0) si no hay writable. */
    ElfBuilder *b = elf_builder_create_exec64(capacity, /*num_phdrs=*/3);
    if (!b) {
        set_err(err, err_cap, "aot_emit_elf: create_exec64 fallo");
        return 0;
    }
    Elf64_Phdr *phdr = (Elf64_Phdr *)b->phdr;

    uint64_t entry_vaddr = 0;

    /* Para resolver relocations tras el layout: VA + offset-en-fichero de cada
     * seccion (indexado por el indice de usuario).  Solo si hay relocs. */
    uint64_t *sec_va = NULL;
    uint64_t *sec_foff = NULL;
    uint8_t *sec_seen = NULL;
    if (relocs && num_relocs > 0) {
        sec_va = (uint64_t *)calloc((size_t)num_secs, sizeof(uint64_t));
        sec_foff = (uint64_t *)calloc((size_t)num_secs, sizeof(uint64_t));
        sec_seen = (uint8_t *)calloc((size_t)num_secs, sizeof(uint8_t));
    }

    /* Agregar las secciones con datos en DOS pasadas para separar permisos:
     * (1) el segmento ejecutable: TODO lo que tenga EXEC (codigo, incluido
     * `.boot` rwx) MAS el rodata no-writable, y (2) un segmento R+W aparte
     * para los datos writable NO-ejecutables (`.data` puro).  Sin esta
     * separacion, un STORE a un global writable (e.g. __vex_cpu_features)
     * caeria en una pagina R-X y segfaultearia; e inversamente, mandar una
     * seccion rwx al segmento R+W (sin X) haria segfaultear su EJECUCION.
     * El criterio de particion es EXEC (no WRITE): el grupo 1 puede contener
     * secciones writable (rwx) -> en ese caso el segmento toma tambien W.
     * El emisor pone vaddr = base + file_off, asi que basta con alinear a
     * pagina entre las dos pasadas para que cada region empiece en un limite
     * de pagina (requisito de mmap del loader del kernel). */
    uint64_t rx_end_off = 0;     /* fin (offset-en-fichero) del segmento exec */
    uint64_t rw_start_off = 0;   /* inicio del segmento R+W (0 = sin datos rw) */
    uint64_t rw_end_off = 0;     /* fin del segmento R+W */
    int exec_seg_writable = 0;   /* alguna seccion exec es tambien writable? */
    for (int pass = 0; pass < 2; ++pass) {
        /* Pasada 0 = grupo EXEC (+ rodata); pasada 1 = data writable no-exec. */
        const int want_rwseg = (pass == 1);
        if (want_rwseg) {
            /* Cerrar el segmento exec y abrir el R+W en un limite de pagina. */
            rx_end_off = b->size;
            if (b->size % AOT_ELF_PAGE != 0) {
                size_t pad = AOT_ELF_PAGE - (b->size % AOT_ELF_PAGE);
                memset(b->mem + b->size, 0, pad);
                b->size += pad;
            }
            rw_start_off = b->size;
        }
        for (int i = 0; i < num_secs; ++i) {
            const AotSection *s = &secs[i];
            if ((s->flags & AOT_SEC_BSS) || s->size == 0) {
                /* BSS en ELF freestanding v1 no soportado (ver doc). */
                continue;
            }
            const int is_exec = (s->flags & AOT_SEC_EXEC) ? 1 : 0;
            const int is_write = (s->flags & AOT_SEC_WRITE) ? 1 : 0;
            /* El grupo 1 (segmento exec) recibe TODO lo no-writable (rodata)
             * Y todo lo ejecutable (aunque sea writable: rwx).  El grupo 2
             * (R+W) recibe solo lo writable NO-ejecutable. */
            const int goes_rwseg = (is_write && !is_exec);
            if (goes_rwseg != want_rwseg) continue;
            if (!want_rwseg && is_write) exec_seg_writable = 1;
            if (b->size % AOT_ELF_PAGE != 0) {
                size_t pad = AOT_ELF_PAGE - (b->size % AOT_ELF_PAGE);
                memset(b->mem + b->size, 0, pad);
                b->size += pad;
            }
            uint64_t sh_flags = SHF_ALLOC;
            if (s->flags & AOT_SEC_EXEC) sh_flags |= SHF_EXECINSTR;
            if (s->flags & AOT_SEC_WRITE) sh_flags |= SHF_WRITE;
            uint64_t align_use = s->align ? s->align : AOT_ELF_PAGE;
            /* VA fija (place_section): rellenar el fichero hasta vaddr-base para
             * que vaddr = base + offset de exactamente lo pedido.  El gap se
             * rellena con ceros (un PT_LOAD lo cubre).  Va en orden de VA: una
             * VA fija por DEBAJO de la posicion actual es inalcanzable. */
            if (s->vaddr != 0) {
                if (s->vaddr < base ||
                    (s->vaddr - base) < (uint64_t)b->size) {
                    free(sec_va);
                    free(sec_foff);
                    free(sec_seen);
                    elf_builder_free(b);
                    set_err(err, err_cap,
                            "aot_emit_elf: VA fija de seccion inalcanzable "
                            "(menor que base o que la posicion actual; coloca "
                            "las secciones en orden de direccion ascendente)");
                    return 0;
                }
                uint64_t want_off = s->vaddr - base;
                if (want_off > (uint64_t)b->size) {
                    memset(b->mem + b->size, 0, want_off - b->size);
                    b->size = want_off;
                }
                align_use = 1; // la VA ya esta fijada; no realinear
            }
            size_t off_out = 0;
            uint64_t vaddr_out = 0;
            size_t idx = elf_builder_add_section_ex(
                b, s->name, SHT_PROGBITS, sh_flags, s->data, s->size,
                base + b->size, align_use, &off_out, &vaddr_out, 0, 0, 0);
            if (idx == 0) {
                free(sec_va);
                free(sec_foff);
                free(sec_seen);
                elf_builder_free(b);
                set_err(err, err_cap, "aot_emit_elf: fallo añadiendo seccion");
                return 0;
            }
            if (sec_va) {
                sec_va[i] = vaddr_out;
                sec_foff[i] = off_out;
                sec_seen[i] = 1;
            }
            if (i == entry_sec) entry_vaddr = vaddr_out;
        }
    }
    rw_end_off = b->size;
    if (rx_end_off == 0) rx_end_off = b->size; /* no hubo seccion writable */

    /* BSS (SHT_NOBITS): secciones con AOT_SEC_BSS -> VA en el segmento R+W, SIN
     * contenido en fichero.  Van DESPUES de .data en el espacio de direcciones
     * (base + rw_end_off + ...); el loader las zerifica via p_memsz > p_filesz.
     * Solo son TARGET de relocs (nunca SITE), asi que sec_foff queda en 0.
     * Necesario para dev-OS: cualquier global sin inicializar (Vex o un .o de C)
     * cae en .bss. */
    uint64_t bss_total = 0;
    {
        uint64_t bss_off = rw_end_off;
        for (int i = 0; i < num_secs; ++i) {
            const AotSection *s = &secs[i];
            if (!(s->flags & AOT_SEC_BSS)) continue;
            uint64_t sz = aot_sec_size(s); /* = bss_size para BSS */
            if (sz == 0) continue;
            uint64_t align_use = s->align ? s->align : 8;
            bss_off = (bss_off + align_use - 1) & ~(align_use - 1);
            if (sec_va) {
                sec_va[i] = base + bss_off;
                sec_foff[i] = 0;
                sec_seen[i] = 1;
            }
            bss_off += sz;
        }
        bss_total = bss_off - rw_end_off;
    }
    if (entry_vaddr == 0) {
        free(sec_va);
        free(sec_foff);
        free(sec_seen);
        elf_builder_free(b);
        set_err(err, err_cap,
                "aot_emit_elf: la seccion de entrada no tiene datos");
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
                free(sec_va);
                free(sec_foff);
                free(sec_seen);
                elf_builder_free(b);
                set_err(err, err_cap,
                        "aot_emit_elf: reloc con seccion invalida");
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
            uint32_t width =
                (rl->kind == AOT_RELOC_ABS64 || rl->kind == AOT_RELOC_IMM64)
                    ? 8u
                    : 4u;
            if (rl->site_off + width > secs[rl->site_section].size) {
                free(sec_va);
                free(sec_foff);
                free(sec_seen);
                elf_builder_free(b);
                set_err(err, err_cap,
                        "aot_emit_elf: reloc fuera de la seccion del sitio");
                return 0;
            }
            if (!apply_reloc(b->mem + sec_foff[rl->site_section] + rl->site_off,
                             site_va, target_value, rl->kind)) {
                free(sec_va);
                free(sec_foff);
                free(sec_seen);
                elf_builder_free(b);
                set_err(err, err_cap, "aot_emit_elf: reloc kind invalido");
                return 0;
            }
        }
    }
    free(sec_va);
    free(sec_foff);
    free(sec_seen);
    sec_va = NULL;
    sec_foff = NULL;
    sec_seen = NULL;

    /* .strtab + .symtab con _start. */
    const char strtab_data[] = "\0_start";
    size_t st_off = 0;
    uint64_t st_va = 0;
    size_t idx_strtab = elf_builder_add_section_ex(
        b, ".strtab", SHT_STRTAB, 0, strtab_data, sizeof(strtab_data), 0, 1,
        &st_off, &st_va, 0, 0, 0);

    Elf64_Sym symtab[2];
    memset(symtab, 0, sizeof(symtab));
    symtab[0].st_info = ELF64_ST_INFO(STB_LOCAL, STT_NOTYPE);
    symtab[0].st_shndx = SHN_UNDEF;
    symtab[1].st_name = 1;
    symtab[1].st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
    symtab[1].st_shndx =
        (uint16_t)(entry_sec + 1); /* +1: la seccion 0 es SHT_NULL */
    symtab[1].st_value = entry_vaddr + entry_off;
    symtab[1].st_size = secs[entry_sec].size;
    size_t sy_off = 0;
    uint64_t sy_va = 0;
    elf_builder_add_section_ex(b, ".symtab", SHT_SYMTAB, 0, symtab,
                               sizeof(symtab), 0, 8, &sy_off, &sy_va,
                               (uint32_t)idx_strtab, 1, sizeof(Elf64_Sym));

    /* Program headers.  Los filesz usan rx_end_off / rw_end_off (capturados
     * ANTES de .strtab/.symtab, que no son ALLOC y no se mapean).  El
     * .symtab/.strtab quedan al final del fichero, fuera de todo PT_LOAD. */
    const int has_rw = (rw_end_off > rw_start_off) || (bss_total > 0);

    /* PT_LOAD ejecutable: codigo + rodata.  R+X, y ademas W si contiene alguna
     * seccion rwx (e.g. `.boot` de un kernel/bootloader que se auto-modifica). */
    phdr[0].p_type = PT_LOAD;
    phdr[0].p_flags = PF_R | PF_X | (exec_seg_writable ? PF_W : 0);
    phdr[0].p_offset = 0;
    phdr[0].p_vaddr = base;
    phdr[0].p_paddr = base;
    phdr[0].p_filesz = rx_end_off;
    phdr[0].p_memsz = (rx_end_off + AOT_ELF_PAGE - 1) & ~(AOT_ELF_PAGE - 1);
    phdr[0].p_align = AOT_ELF_PAGE;

    /* PT_LOAD R+W: secciones writable (.data).  Si no hay, este phdr describe
     * el stack RW (filesz 0) para no desperdiciar la entrada. */
    phdr[1].p_type = PT_LOAD;
    phdr[1].p_flags = PF_R | PF_W;
    if (has_rw) {
        phdr[1].p_offset = rw_start_off;
        phdr[1].p_vaddr = base + rw_start_off;
        phdr[1].p_paddr = base + rw_start_off;
        phdr[1].p_filesz = rw_end_off - rw_start_off; /* solo .data en fichero */
        /* p_memsz cubre .data + .bss (el loader zerifica [filesz, memsz)). */
        phdr[1].p_memsz = (rw_end_off - rw_start_off) + bss_total;
        phdr[1].p_align = AOT_ELF_PAGE;
    } else {
        phdr[1].p_offset = 0;
        phdr[1].p_vaddr = stack_vaddr;
        phdr[1].p_paddr = stack_vaddr;
        phdr[1].p_filesz = 0;
        phdr[1].p_memsz = stack_size;
        phdr[1].p_align = AOT_ELF_PAGE;
    }

    /* PT_GNU_STACK: pila RW no-ejecutable.  Si el R+W ya describio .data,
     * aqui reservamos ademas el stack como un PT_LOAD RW separado. */
    phdr[2].p_type = PT_LOAD;
    phdr[2].p_flags = PF_R | PF_W;
    phdr[2].p_offset = 0;
    phdr[2].p_vaddr = stack_vaddr;
    phdr[2].p_paddr = stack_vaddr;
    phdr[2].p_filesz = 0;
    phdr[2].p_memsz = has_rw ? stack_size : 0;
    phdr[2].p_align = AOT_ELF_PAGE;

    elf_builder_finalize_exec64(b, entry_vaddr + entry_off);

    FILE *f = fopen(path, "wb");
    if (!f) {
        elf_builder_free(b);
        set_err(err, err_cap, "aot_emit_elf: fopen fallo");
        return 0;
    }
    size_t written = fwrite(b->mem, 1, b->size, f);
    fclose(f);
    int ok = (written == b->size);
    if (!ok) set_err(err, err_cap, "aot_emit_elf: escritura incompleta");
    elf_builder_free(b);
    return ok;
}

/* Buffer de salida con append + alineacion (compartido por dynexec + .o). */
typedef struct {
    uint8_t *p;
    size_t len, cap;
} OBuf;
static int ob_reserve(OBuf *o, size_t extra) {
    if (o->len + extra <= o->cap) return 1;
    size_t nc = o->cap ? o->cap * 2 : 4096;
    while (nc < o->len + extra)
        nc *= 2;
    uint8_t *np = (uint8_t *)realloc(o->p, nc);
    if (!np) return 0;
    o->p = np;
    o->cap = nc;
    return 1;
}
static int ob_put(OBuf *o, const void *data, size_t n) {
    if (!ob_reserve(o, n)) return 0;
    if (n) {
        if (data)
            memcpy(o->p + o->len, data, n);
        else
            memset(o->p + o->len, 0, n);
    }
    o->len += n;
    return 1;
}
static int ob_align(OBuf *o, size_t a) {
    while (o->len % a) {
        uint8_t z = 0;
        if (!ob_put(o, &z, 1)) return 0;
    }
    return 1;
}

/* Escribe un u32 LE en p. */
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Escribe un u16 LE en p. */
static void wr16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/* Alinea x hacia arriba a un multiplo de a (potencia de 2). */
static uint32_t aot_dyn_align_u32(uint32_t x, uint32_t a) {
    return (x + (a - 1)) & ~(a - 1);
}

/* =========================================================================
 *  PE32 EJECUTABLE (i386, .exe Windows 32-bit) -- hand-rolled.
 *
 *  Minimo: secciones de usuario (RVA por pagina) + .idata sintetizada con UN
 *  import (kernel32!ExitProcess, el _start sale por ahi).  Parchea el `call
 *  main` (REL32) + intra-imagen + el `FF 15` del stub (abs32 a la IAT).  v1:
 *  EXEC self-contained (sin libc); llamadas a externos = follow-up (thunks
 * 32b).
 * ========================================================================= */
/* Construye la seccion .idata de un PE32 con N imports agrupados por DLL.
 * @p idata_rva: RVA donde vivira .idata (para los RVAs internos).
 * Devuelve buffer malloc'd (caller libera) + *size_out.  Rellena
 * @p iat_off_out[k] = offset DENTRO de .idata de la entrada IAT del import k
 * (para parchear su thunk: abs = ImageBase + idata_rva + iat_off_out[k]).
 * *impdir_size_out, *iat_rva0_out, *iat_total_out para los DataDirectory. */
static uint8_t *build_idata32(const AotImport *imps, int n, uint32_t idata_rva,
                              uint32_t *size_out, uint32_t *impdir_size_out,
                              uint32_t *iat_rva0_out, uint32_t *iat_total_out,
                              uint32_t *iat_off_out) {
    /* DLLs unicas (orden de aparicion) + funcs por DLL. */
    const char **dlls = (const char **)calloc((size_t)n, sizeof(char *));
    int *fcount = (int *)calloc((size_t)n, sizeof(int));
    /* func[d] = lista de nombres (punteros) por DLL d. */
    const char ***funcs = (const char ***)calloc((size_t)n, sizeof(char **));
    int ndll = 0;
    for (int k = 0; k < n; ++k) {
        int d = -1;
        for (int j = 0; j < ndll; ++j)
            if (strcmp(dlls[j], imps[k].dll) == 0) {
                d = j;
                break;
            }
        if (d < 0) {
            d = ndll++;
            dlls[d] = imps[k].dll;
            funcs[d] = (const char **)calloc((size_t)n, sizeof(char *));
        }
        int dup = 0;
        for (int j = 0; j < fcount[d]; ++j)
            if (strcmp(funcs[d][j], imps[k].func) == 0) {
                dup = 1;
                break;
            }
        if (!dup) funcs[d][fcount[d]++] = imps[k].func;
    }
    /* Layout. */
    uint32_t impdir = 0, impdir_sz = (uint32_t)(ndll + 1) * 20;
    uint32_t *ilt_off = (uint32_t *)calloc((size_t)ndll, sizeof(uint32_t));
    uint32_t *iat_off = (uint32_t *)calloc((size_t)ndll, sizeof(uint32_t));
    uint32_t cur = impdir + impdir_sz;
    for (int d = 0; d < ndll; ++d) {
        ilt_off[d] = cur;
        cur += (uint32_t)(fcount[d] + 1) * 4;
    }
    for (int d = 0; d < ndll; ++d) {
        iat_off[d] = cur;
        cur += (uint32_t)(fcount[d] + 1) * 4;
    }
    /* hint/name por (dll,func) unica + dll names; recordar offsets. */
    uint32_t **hint_off = (uint32_t **)calloc((size_t)ndll, sizeof(uint32_t *));
    for (int d = 0; d < ndll; ++d)
        hint_off[d] = (uint32_t *)calloc((size_t)fcount[d], sizeof(uint32_t));
    for (int d = 0; d < ndll; ++d)
        for (int i = 0; i < fcount[d]; ++i) {
            hint_off[d][i] = cur;
            uint32_t hl = 2 + (uint32_t)strlen(funcs[d][i]) + 1;
            if (hl & 1) ++hl;
            cur += hl;
        }
    uint32_t *dll_off = (uint32_t *)calloc((size_t)ndll, sizeof(uint32_t));
    for (int d = 0; d < ndll; ++d) {
        dll_off[d] = cur;
        cur += (uint32_t)strlen(dlls[d]) + 1;
    }
    uint32_t size = cur;

    uint8_t *buf = (uint8_t *)calloc(1, size);
    for (int d = 0; d < ndll; ++d) {
        uint8_t *e = buf + impdir + (uint32_t)d * 20;
        wr32(e + 0, idata_rva + ilt_off[d]);  /* OriginalFirstThunk */
        wr32(e + 12, idata_rva + dll_off[d]); /* Name */
        wr32(e + 16, idata_rva + iat_off[d]); /* FirstThunk (IAT) */
        for (int i = 0; i < fcount[d]; ++i) {
            wr32(buf + ilt_off[d] + (uint32_t)i * 4,
                 idata_rva + hint_off[d][i]);
            wr32(buf + iat_off[d] + (uint32_t)i * 4,
                 idata_rva + hint_off[d][i]);
            memcpy(buf + hint_off[d][i] + 2, funcs[d][i], strlen(funcs[d][i]));
        }
        memcpy(buf + dll_off[d], dlls[d], strlen(dlls[d]));
    }
    /* iat_off_out[k] para cada import original. */
    for (int k = 0; k < n; ++k) {
        int d = -1;
        for (int j = 0; j < ndll; ++j)
            if (strcmp(dlls[j], imps[k].dll) == 0) {
                d = j;
                break;
            }
        int fi = 0;
        for (int i = 0; i < fcount[d]; ++i)
            if (strcmp(funcs[d][i], imps[k].func) == 0) {
                fi = i;
                break;
            }
        iat_off_out[k] = iat_off[d] + (uint32_t)fi * 4;
    }
    *size_out = size;
    *impdir_size_out = impdir_sz;
    *iat_rva0_out = idata_rva + iat_off[0];
    *iat_total_out = (uint32_t)(cur > 0 ? 0 : 0); /* no usado finamente */
    /* IAT total range: desde iat_off[0] hasta fin del ultimo IAT. */
    *iat_total_out =
        (iat_off[ndll - 1] + (uint32_t)(fcount[ndll - 1] + 1) * 4) - iat_off[0];

    for (int d = 0; d < ndll; ++d) {
        free(funcs[d]);
        free(hint_off[d]);
    }
    free(dlls);
    free(fcount);
    free(funcs);
    free(ilt_off);
    free(iat_off);
    free(hint_off);
    free(dll_off);
    return buf;
}

int aot_emit_pe32(const char *path, const AotLayoutCfg *cfg,
                  const AotSection *secs, int num_secs, int entry_sec,
                  uint64_t entry_off, const AotImport *imps, int num_imps,
                  const AotReloc *relocs, int num_relocs, char *err,
                  size_t err_cap) {
    if (num_secs <= 0) {
        set_err(err, err_cap, "pe32: sin secciones");
        return 0;
    }
    if (num_imps <= 0) {
        set_err(err, err_cap, "pe32: sin imports (falta el _start)");
        return 0;
    }
    if (entry_sec < 0 || entry_sec >= num_secs) {
        set_err(err, err_cap, "pe32: entry_sec fuera de rango");
        return 0;
    }

    const uint32_t IMGBASE =
        (cfg && cfg->image_base) ? (uint32_t)cfg->image_base : 0x00400000u;
    const uint32_t SECALIGN = 0x1000u, FILEALIGN = 0x200u;
    const uint32_t HDR_RVA = 0; /* headers en RVA 0 */

    /* Numero de secciones del PE = secciones de usuario con datos + .idata. */
    int n_user = 0;
    for (int i = 0; i < num_secs; ++i)
        if (!(secs[i].flags & AOT_SEC_BSS) && secs[i].size > 0) ++n_user;
    const int n_pe_sec = n_user + 1; /* + .idata */

    /* Tamano de cabeceras: DOS(0x40) + "PE\0\0"(4) + COFF(20) + OptHdr32(224)
     * + section headers (40 c/u), alineado a FILEALIGN. */
    const uint32_t opt_size = 224; /* 96 estandar+win + 128 data dirs */
    uint32_t hdr_raw = 0x40 + 4 + 20 + opt_size + (uint32_t)n_pe_sec * 40;
    uint32_t size_headers = aot_dyn_align_u32(hdr_raw, FILEALIGN);

    /* Layout de secciones: RVA por pagina (SECALIGN), file offset por
     * FILEALIGN. sec_rva/sec_foff por seccion de usuario; luego .idata. */
    uint32_t *sec_rva = (uint32_t *)calloc((size_t)num_secs, sizeof(uint32_t));
    uint32_t *sec_foff = (uint32_t *)calloc((size_t)num_secs, sizeof(uint32_t));
    uint32_t *sec_vsz = (uint32_t *)calloc((size_t)num_secs, sizeof(uint32_t));
    uint8_t *sec_seen = (uint8_t *)calloc((size_t)num_secs, sizeof(uint8_t));
    uint32_t rva = aot_dyn_align_u32(size_headers, SECALIGN); /* primera secc */
    uint32_t foff = size_headers;
    for (int i = 0; i < num_secs; ++i) {
        if ((secs[i].flags & AOT_SEC_BSS) || secs[i].size == 0) continue;
        sec_rva[i] = rva;
        sec_foff[i] = foff;
        sec_vsz[i] = (uint32_t)secs[i].size;
        sec_seen[i] = 1;
        rva = aot_dyn_align_u32(rva + (uint32_t)secs[i].size, SECALIGN);
        foff = aot_dyn_align_u32(foff + (uint32_t)secs[i].size, FILEALIGN);
    }
    if (!sec_seen[entry_sec]) {
        free(sec_rva);
        free(sec_foff);
        free(sec_vsz);
        free(sec_seen);
        set_err(err, err_cap, "pe32: la seccion de entrada no tiene datos");
        return 0;
    }

    /* .idata multi-DLL (kernel32!ExitProcess del stub + libc de los thunks). */
    uint32_t idata_rva = rva, idata_foff = foff;
    uint32_t idata_size = 0, impdir_size = 0, iat_rva0 = 0, iat_total = 0;
    uint32_t *imp_iat_off =
        (uint32_t *)calloc((size_t)num_imps, sizeof(uint32_t));
    uint8_t *idata_buf =
        build_idata32(imps, num_imps, idata_rva, &idata_size, &impdir_size,
                      &iat_rva0, &iat_total, imp_iat_off);
    uint32_t idata_vsz = idata_size;
    uint32_t idata_fsz = aot_dyn_align_u32(idata_size, FILEALIGN);

    uint32_t total = idata_foff + idata_fsz;
    uint32_t size_image = aot_dyn_align_u32(idata_rva + idata_vsz, SECALIGN);

    uint8_t *img = (uint8_t *)calloc(1, total);
    if (!img) {
        free(sec_rva);
        free(sec_foff);
        free(sec_vsz);
        free(sec_seen);
        set_err(err, err_cap, "pe32: OOM");
        return 0;
    }

    uint32_t entry_rva = sec_rva[entry_sec] + (uint32_t)entry_off;

    /* DOS header. */
    img[0] = 'M';
    img[1] = 'Z';
    wr32(img + 0x3C, 0x40); /* e_lfanew */

    /* PE signature + COFF header (en 0x40). */
    uint8_t *pe = img + 0x40;
    pe[0] = 'P';
    pe[1] = 'E';
    pe[2] = 0;
    pe[3] = 0;
    uint8_t *coff = pe + 4;
    coff[0] = 0x4C;
    coff[1] = 0x01; /* Machine = 0x14C (i386) */
    coff[2] = (uint8_t)n_pe_sec;
    coff[3] = (uint8_t)(n_pe_sec >> 8); /* NumberOfSections */
    /* TimeDateStamp/PtrSym/NumSym = 0 (coff+4..15) */
    coff[16] = (uint8_t)opt_size;
    coff[17] = (uint8_t)(opt_size >> 8); /* SizeOfOptionalHeader */
    /* Characteristics: EXECUTABLE_IMAGE(0x2) | 32BIT_MACHINE(0x100) |
     * RELOCS_STRIPPED(0x1) */
    uint16_t chars = 0x0002 | 0x0100 | 0x0001;
    coff[18] = (uint8_t)chars;
    coff[19] = (uint8_t)(chars >> 8);

    /* Optional header PE32 (magic 0x10B). */
    uint8_t *oh = coff + 20;
    oh[0] = 0x0B;
    oh[1] = 0x01; /* Magic = 0x10B (PE32) */
    oh[2] = 1;
    oh[3] = 0;                         /* Linker version */
    wr32(oh + 16, entry_rva);          /* AddressOfEntryPoint */
    wr32(oh + 20, sec_rva[entry_sec]); /* BaseOfCode */
    wr32(oh + 24, idata_rva);          /* BaseOfData (PE32-only) */
    wr32(oh + 28, IMGBASE);            /* ImageBase (32-bit) */
    wr32(oh + 32, SECALIGN);           /* SectionAlignment */
    wr32(oh + 36, FILEALIGN);          /* FileAlignment */
    oh[40] = 4;
    oh[41] = 0;
    oh[44] = 0;
    oh[45] = 0; /* OS ver 4.0 */
    oh[48] = 4;
    oh[49] = 0;                  /* Subsystem ver 4.0 */
    wr32(oh + 56, size_image);   /* SizeOfImage */
    wr32(oh + 60, size_headers); /* SizeOfHeaders */
    oh[68] = 3;
    oh[69] = 0; /* Subsystem = CONSOLE(3) */
    /* DllCharacteristics = 0 (sin DYNAMIC_BASE -> base fija; sin .reloc). */
    wr32(oh + 72, 0x100000);
    wr32(oh + 76, 0x1000); /* StackReserve/Commit */
    wr32(oh + 80, 0x100000);
    wr32(oh + 84, 0x1000); /* HeapReserve/Commit */
    wr32(oh + 92, 16);     /* NumberOfRvaAndSizes */
    /* Data directories (offset 96), 16 * 8.  [1]=Import, [12]=IAT. */
    uint8_t *dd = oh + 96;
    wr32(dd + 1 * 8 + 0, idata_rva);   /* Import dir RVA */
    wr32(dd + 1 * 8 + 4, impdir_size); /* Import dir size */
    wr32(dd + 12 * 8 + 0, iat_rva0);   /* IAT RVA (1er IAT) */
    wr32(dd + 12 * 8 + 4, iat_total);  /* IAT size total */

    /* Section headers (tras el optional header). */
    uint8_t *sh = oh + opt_size;
    int shi = 0;
    for (int i = 0; i < num_secs; ++i) {
        if (!sec_seen[i]) continue;
        uint8_t *e = sh + shi * 40;
        const char *nm = secs[i].name ? secs[i].name : ".text";
        for (int j = 0; j < 8 && nm[j]; ++j)
            e[j] = (uint8_t)nm[j];
        wr32(e + 8, sec_vsz[i]);  /* VirtualSize */
        wr32(e + 12, sec_rva[i]); /* VirtualAddress */
        wr32(e + 16,
             aot_dyn_align_u32(sec_vsz[i], FILEALIGN)); /* SizeOfRawData */
        wr32(e + 20, sec_foff[i]);                      /* PointerToRawData */
        uint32_t c = (secs[i].flags & AOT_SEC_EXEC) ? 0x60000020u : 0x40000040u;
        if (secs[i].flags & AOT_SEC_WRITE) c |= 0x80000000u;
        wr32(e + 36, c); /* Characteristics */
        ++shi;
    }
    /* .idata section header. */
    {
        uint8_t *e = sh + shi * 40;
        memcpy(e, ".idata", 6);
        wr32(e + 8, idata_vsz);
        wr32(e + 12, idata_rva);
        wr32(e + 16, idata_fsz);
        wr32(e + 20, idata_foff);
        wr32(e + 36, 0xC0000040u); /* INITIALIZED_DATA|READ|WRITE */
    }

    /* Datos de las secciones de usuario. */
    for (int i = 0; i < num_secs; ++i)
        if (sec_seen[i]) memcpy(img + sec_foff[i], secs[i].data, secs[i].size);

    /* .idata: el buffer multi-DLL construido por build_idata32. */
    memcpy(img + idata_foff, idata_buf, idata_size);

    /* Relocs intra-imagen del driver (REL32 a funciones; ABS no en v1). */
    int ok = 1;
    for (int r = 0; r < num_relocs && ok; ++r) {
        const AotReloc *rl = &relocs[r];
        if (rl->kind != AOT_RELOC_REL32) {
            set_err(err, err_cap,
                    "pe32: solo REL32 intra-imagen en v1 (ABS32 follow-up)");
            ok = 0;
            break;
        }
        if (rl->site_section < 0 || rl->site_section >= num_secs ||
            !sec_seen[rl->site_section] ||
            (!rl->target_is_size &&
             (rl->target_section < 0 || rl->target_section >= num_secs ||
              !sec_seen[rl->target_section]))) {
            set_err(err, err_cap, "pe32: reloc con seccion invalida");
            ok = 0;
            break;
        }
        uint64_t tv = IMGBASE + sec_rva[rl->target_section] + rl->target_off;
        tv = (uint64_t)((int64_t)tv + rl->addend);
        uint64_t site_va = IMGBASE + sec_rva[rl->site_section] + rl->site_off;
        if (!apply_reloc(img + sec_foff[rl->site_section] + rl->site_off,
                         site_va, tv, rl->kind)) {
            set_err(err, err_cap, "pe32: reloc kind invalido");
            ok = 0;
            break;
        }
    }

    /* Parchear cada import: el FF 15/FF 25 disp32 = abs de SU entrada IAT.
     * (FF 15 = call del stub ExitProcess; FF 25 = jmp del thunk de un libc).
     * imp_iat_off[k] = offset dentro de .idata de la entrada IAT del import k.
     */
    for (int k = 0; k < num_imps && ok; ++k) {
        int cs = imps[k].call_section;
        if (cs < 0 || cs >= num_secs || !sec_seen[cs]) {
            set_err(err, err_cap, "pe32: call_section del import invalido");
            ok = 0;
            break;
        }
        uint32_t disp = IMGBASE + idata_rva + imp_iat_off[k];
        wr32(img + sec_foff[cs] + imps[k].call_off + 2, disp);
    }

    if (ok) {
        FILE *f = fopen(path, "wb");
        if (!f) {
            set_err(err, err_cap, "pe32: fopen fallo");
            ok = 0;
        } else {
            size_t w = fwrite(img, 1, total, f);
            fclose(f);
            if (w != total) {
                set_err(err, err_cap, "pe32: escritura incompleta");
                ok = 0;
            }
        }
    }
    free(img);
    free(sec_rva);
    free(sec_foff);
    free(sec_vsz);
    free(sec_seen);
    free(idata_buf);
    free(imp_iat_off);
    return ok;
}

/* =========================================================================
 *  ELF32 EJECUTABLE ESTaTICO (ET_EXEC, EM_386) -- modo protegido / kernels.
 *  Hand-rolled (sin structs Elf32: el orden de campos del phdr ELF32 difiere
 *  del ELF64).  1 PT_LOAD R+X cubriendo ehdr+phdr+codigo; entry = _start;
 *  salida via int 0x80 (el stub).  Freestanding: sin dynamic ni libc.  Solo
 *  relocs PC-relativas (CALL_REL32, invariantes de la base) + inmediatos; el
 *  x86-32 no tiene RIP-relativo -> refs a datos absolutas (ABS32) son
 * follow-up.
 * ========================================================================= */
int aot_emit_elf32(const char *path, const AotLayoutCfg *cfg,
                   const AotSection *secs, int num_secs, int entry_sec,
                   uint64_t entry_off, const AotReloc *relocs, int num_relocs,
                   char *err, size_t err_cap) {
    if (num_secs <= 0) {
        set_err(err, err_cap, "elf32: sin secciones");
        return 0;
    }
    if (entry_sec < 0 || entry_sec >= num_secs) {
        set_err(err, err_cap, "elf32: entry_sec fuera de rango");
        return 0;
    }

    const uint32_t base =
        (cfg && cfg->image_base) ? (uint32_t)cfg->image_base : 0x08048000u;
    const uint32_t EHDR = 52, PHDR = 32;

    /* Layout: ehdr + phdr + secciones (vaddr = base + file offset). */
    uint32_t *sec_va = (uint32_t *)calloc((size_t)num_secs, sizeof(uint32_t));
    uint32_t *sec_fo = (uint32_t *)calloc((size_t)num_secs, sizeof(uint32_t));
    uint8_t *sec_seen = (uint8_t *)calloc((size_t)num_secs, sizeof(uint8_t));
    uint32_t off = EHDR + PHDR;
    for (int i = 0; i < num_secs; ++i) {
        if ((secs[i].flags & AOT_SEC_BSS) || secs[i].size == 0) continue;
        uint32_t a = secs[i].align ? (uint32_t)secs[i].align : 16u;
        while (off % a)
            ++off;
        sec_fo[i] = off;
        sec_va[i] = base + off;
        sec_seen[i] = 1;
        off += (uint32_t)secs[i].size;
    }
    uint32_t total = off;
    if (!sec_seen[entry_sec]) {
        free(sec_va);
        free(sec_fo);
        free(sec_seen);
        set_err(err, err_cap, "elf32: la seccion de entrada no tiene datos");
        return 0;
    }
    uint32_t entry_va = sec_va[entry_sec] + (uint32_t)entry_off;

    uint8_t *img = (uint8_t *)calloc(1, total);
    if (!img) {
        free(sec_va);
        free(sec_fo);
        free(sec_seen);
        set_err(err, err_cap, "elf32: OOM");
        return 0;
    }

    /* Ehdr (52 bytes). */
    img[0] = 0x7f;
    img[1] = 'E';
    img[2] = 'L';
    img[3] = 'F';
    img[4] = 1 /*ELFCLASS32*/;
    img[5] = 1 /*LSB*/;
    img[6] = 1 /*EV_CURRENT*/;
    img[16] = 2;
    img[17] = 0; /* e_type = ET_EXEC */
    img[18] = 3;
    img[19] = 0;              /* e_machine = EM_386 */
    wr32(img + 20, 1);        /* e_version */
    wr32(img + 24, entry_va); /* e_entry */
    wr32(img + 28, EHDR);     /* e_phoff = 52 */
    wr32(img + 32, 0);        /* e_shoff */
    wr32(img + 36, 0);        /* e_flags */
    img[40] = (uint8_t)EHDR;
    img[41] = 0; /* e_ehsize */
    img[42] = (uint8_t)PHDR;
    img[43] = 0; /* e_phentsize */
    img[44] = 1;
    img[45] = 0; /* e_phnum */
    img[46] = 0;
    img[47] = 0; /* e_shentsize */
    img[48] = 0;
    img[49] = 0; /* e_shnum */
    img[50] = 0;
    img[51] = 0; /* e_shstrndx */

    /* Phdr ELF32 (PT_LOAD R+X):
     * type,offset,vaddr,paddr,filesz,memsz,flags,align. */
    uint8_t *ph = img + EHDR;
    wr32(ph + 0, 1);       /* p_type = PT_LOAD */
    wr32(ph + 4, 0);       /* p_offset */
    wr32(ph + 8, base);    /* p_vaddr */
    wr32(ph + 12, base);   /* p_paddr */
    wr32(ph + 16, total);  /* p_filesz */
    wr32(ph + 20, total);  /* p_memsz */
    wr32(ph + 24, 5);      /* p_flags = R+X */
    wr32(ph + 28, 0x1000); /* p_align */

    /* Datos de las secciones. */
    for (int i = 0; i < num_secs; ++i)
        if (sec_seen[i]) memcpy(img + sec_fo[i], secs[i].data, secs[i].size);

    /* Relocs (solo PC-relativas + inmediatos en x86-32 v1). */
    int ok = 1;
    for (int r = 0; r < num_relocs && ok; ++r) {
        const AotReloc *rl = &relocs[r];
        if (rl->kind == AOT_RELOC_ABS64 || rl->kind == AOT_RELOC_IMM64) {
            set_err(err, err_cap, "elf32: ABS64/IMM64 no aplica en 32-bit");
            ok = 0;
            break;
        }
        if (rl->site_section < 0 || rl->site_section >= num_secs ||
            !sec_seen[rl->site_section] ||
            (!rl->target_is_size &&
             (rl->target_section < 0 || rl->target_section >= num_secs ||
              !sec_seen[rl->target_section]))) {
            set_err(err, err_cap, "elf32: reloc con seccion invalida");
            ok = 0;
            break;
        }
        uint64_t tv;
        if (rl->target_is_size)
            tv = aot_sec_size(&secs[rl->target_section]);
        else {
            tv = sec_va[rl->target_section];
            tv += rl->target_is_end ? aot_sec_size(&secs[rl->target_section])
                                    : rl->target_off;
        }
        tv = (uint64_t)((int64_t)tv + rl->addend);
        uint64_t site_va = sec_va[rl->site_section] + rl->site_off;
        if (!apply_reloc(img + sec_fo[rl->site_section] + rl->site_off, site_va,
                         tv, rl->kind)) {
            set_err(err, err_cap, "elf32: reloc kind invalido");
            ok = 0;
            break;
        }
    }

    if (ok) {
        FILE *f = fopen(path, "wb");
        if (!f) {
            set_err(err, err_cap, "elf32: fopen fallo");
            ok = 0;
        } else {
            size_t w = fwrite(img, 1, total, f);
            fclose(f);
            if (w != total) {
                set_err(err, err_cap, "elf32: escritura incompleta");
                ok = 0;
            }
        }
    }
    free(img);
    free(sec_va);
    free(sec_fo);
    free(sec_seen);
    return ok;
}

/* =========================================================================
 *  ELF64 EJECUTABLE DINaMICO (PIE ET_DYN) con imports libc via eager-GOT
 *  -- hand-rolled (sin tocar el submodulo).  AOT.2.exec slice 2.
 *
 *  Modelo (mas simple que PLT lazy): por cada simbolo externo una entrada
 *  GOT (8B, .got writable) + reloc R_X86_64_GLOB_DAT contra un dynsym UND;
 *  el thunk `FF 25 disp32` (jmp [rip+GOT]) que ya emitio el driver salta por
 *  la GOT.  El linker dinamico (DT_NEEDED libc.so.6 + DF_BIND_NOW) resuelve
 *  las GOT al cargar.  PT_INTERP -> ld.so.  vaddr == file offset (PIE base 0).
 * ========================================================================= */

#ifndef DT_NULL
#define DT_NULL 0
#endif
#ifndef DT_FLAGS_1
#define DT_FLAGS_1 0x6ffffffbULL
#endif
#ifndef DF_BIND_NOW
#define DF_BIND_NOW 0x8
#endif
#ifndef DF_1_NOW
#define DF_1_NOW 0x1
#endif

/* Hash SysV de ELF (para .hash / DT_HASH). */
static unsigned long aot_elf_hash(const char *name) {
    unsigned long h = 0, g;
    while (*name) {
        h = (h << 4) + (unsigned char)*name++;
        g = h & 0xf0000000UL;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

#define AOT_DYN_ALIGN(x, a) (((x) + ((a) - 1)) & ~((uint64_t)(a) - 1))

int aot_emit_elf_dynexec(const char *path, const AotLayoutCfg *cfg,
                         const AotSection *secs, int num_secs, int entry_sec,
                         uint64_t entry_off, const AotReloc *relocs,
                         int num_relocs, const AotImport *imps, int num_imps,
                         char *err, size_t err_cap) {
    (void)cfg;
    if (num_secs <= 0) {
        set_err(err, err_cap, "elf_dynexec: sin secciones");
        return 0;
    }
    if (entry_sec < 0 || entry_sec >= num_secs) {
        set_err(err, err_cap, "elf_dynexec: entry_sec fuera de rango");
        return 0;
    }
    if (num_imps < 0) num_imps = 0;
    /* num_imps == 0 es valido: un EXEC dinamico sin imports de libc pero con TLS
     * (necesita el cargador para montar el bloque thread-local) o con relocs
     * RELATIVE.  GOT vacia + dynsym de 1 entrada (null). */

    const uint64_t PAGE = AOT_ELF_PAGE;
    /* Seccion TLS (plantilla thread_local): si existe, anyade un PT_TLS. */
    int tls_sec = -1;
    for (int i = 0; i < num_secs; ++i)
        if (secs[i].flags & AOT_SEC_TLS) {
            tls_sec = i;
            break;
        }
    const int NPH = 6 + (tls_sec >= 0 ? 1 : 0); /* +PT_TLS si hay TLS */
    const int nsym = 1 + num_imps; /* [0]=null + 1 UND por import */
    const char interp[] = "/lib64/ld-linux-x86-64.so.2";
    const size_t interp_len = sizeof(interp); /* incluye el nul */

    /* Sonames DISTINTOS de los imports -> un DT_NEEDED por libreria (el campo
     * dll de cada AotImport es el soname; un import sin dll va a libc.so.6). */
    const int scap = num_imps > 0 ? num_imps : 1;
    const char **sonames = (const char **)calloc((size_t)scap, sizeof(char *));
    uint32_t *soname_off = (uint32_t *)calloc((size_t)scap, sizeof(uint32_t));
    int num_sonames = 0;
    for (int i = 0; i < num_imps; ++i) {
        const char *so =
            (imps[i].dll && imps[i].dll[0]) ? imps[i].dll : "libc.so.6";
        int found = 0;
        for (int j = 0; j < num_sonames; ++j)
            if (strcmp(sonames[j], so) == 0) {
                found = 1;
                break;
            }
        if (!found) sonames[num_sonames++] = so;
    }
    if (num_sonames == 0) sonames[num_sonames++] = "libc.so.6";

    /* --- .dynstr: "\0" + nombres de import + sonames --- */
    OBuf dynstr;
    memset(&dynstr, 0, sizeof(dynstr));
    {
        uint8_t z = 0;
        ob_put(&dynstr, &z, 1);
    }
    uint32_t *name_off = (uint32_t *)calloc((size_t)num_imps, sizeof(uint32_t));
    for (int i = 0; i < num_imps; ++i) {
        name_off[i] = (uint32_t)dynstr.len;
        ob_put(&dynstr, imps[i].func, strlen(imps[i].func) + 1);
    }
    for (int s = 0; s < num_sonames; ++s) {
        soname_off[s] = (uint32_t)dynstr.len;
        ob_put(&dynstr, sonames[s], strlen(sonames[s]) + 1);
    }

    /* --- .hash SysV: nbucket, nchain(=nsym), buckets[], chains[] --- */
    uint32_t nbucket = (uint32_t)(nsym < 1 ? 1 : nsym);
    uint32_t nchain = (uint32_t)nsym;
    uint32_t *bucket = (uint32_t *)calloc(nbucket, sizeof(uint32_t));
    uint32_t *chain = (uint32_t *)calloc(nchain, sizeof(uint32_t));
    for (int i = 0; i < num_imps; ++i) {
        uint32_t si = (uint32_t)(1 + i);
        uint32_t b = (uint32_t)(aot_elf_hash(imps[i].func) % nbucket);
        chain[si] = bucket[b];
        bucket[b] = si;
    }
    size_t hash_size = (size_t)(2 + nbucket + nchain) * 4u;

    /* ================= PASO 1: layout (vaddr == file offset) =================
     */
    uint64_t off = 0;
    uint64_t ehdr_off = off;
    off += sizeof(Elf64_Ehdr);
    uint64_t phdr_off = off;
    off += (uint64_t)NPH * sizeof(Elf64_Phdr);
    uint64_t interp_off = off;
    off += interp_len;
    off = AOT_DYN_ALIGN(off, 8);
    uint64_t dynsym_off = off;
    off += (uint64_t)nsym * sizeof(Elf64_Sym);
    uint64_t dynstr_off = off;
    off += dynstr.len;
    off = AOT_DYN_ALIGN(off, 4);
    uint64_t hash_off = off;
    off += hash_size;
    off = AOT_DYN_ALIGN(off, 8);
    uint64_t rela_off = off;
    /* Las relocs ABS64 del driver (p.ej. las entradas de la .got que el linker
     * construye para los GOTPCREL) no caben tal cual en un PIE (base aleatoria):
     * se materializan como R_X86_64_RELATIVE en .rela.dyn (el cargador escribe
     * base + addend).  Se cuentan aqui para dimensionar .rela.dyn. */
    int num_abs64 = 0;
    for (int r = 0; r < num_relocs; ++r)
        if (relocs[r].kind == AOT_RELOC_ABS64) ++num_abs64;
    const int n_dynrela = num_imps + num_abs64;
    off += (uint64_t)n_dynrela * sizeof(Elf64_Rela);
    off = AOT_DYN_ALIGN(off, 16);

    /* Secciones de codigo/rodata (NO-WRITE) en el segmento R+X. */
    uint64_t *sec_va = (uint64_t *)calloc((size_t)num_secs, sizeof(uint64_t));
    uint8_t *sec_seen = (uint8_t *)calloc((size_t)num_secs, sizeof(uint8_t));
    for (int i = 0; i < num_secs; ++i) {
        if ((secs[i].flags & AOT_SEC_WRITE) || (secs[i].flags & AOT_SEC_BSS))
            continue;
        if (secs[i].size == 0) continue;
        uint64_t a = secs[i].align ? secs[i].align : 16;
        off = AOT_DYN_ALIGN(off, a);
        sec_va[i] = off;
        sec_seen[i] = 1;
        off += secs[i].size;
    }
    uint64_t region1_end = off;
    off = AOT_DYN_ALIGN(off, PAGE);
    uint64_t region2_start = off;

    uint64_t got_off = off;
    off += (uint64_t)num_imps * 8u;

    /* Secciones WRITE (.data) en el segmento R+W. */
    for (int i = 0; i < num_secs; ++i) {
        if (!(secs[i].flags & AOT_SEC_WRITE)) continue;
        if (secs[i].flags & AOT_SEC_BSS) continue; /* bss v1: omitido */
        if (secs[i].size == 0) continue;
        uint64_t a = secs[i].align ? secs[i].align : 8;
        off = AOT_DYN_ALIGN(off, a);
        sec_va[i] = off;
        sec_seen[i] = 1;
        off += secs[i].size;
    }
    off = AOT_DYN_ALIGN(off, 8);
    uint64_t dynamic_off = off;
    /* 11 entradas fijas (HASH/STRTAB/SYMTAB/STRSZ/SYMENT/RELA/RELASZ/RELAENT/
     * FLAGS/FLAGS_1/NULL) + un DT_NEEDED por soname. */
    const int ndyn = 11 + num_sonames;
    off += (uint64_t)ndyn * sizeof(Elf64_Dyn);
    uint64_t region2_end = off; /* fin del contenido en FICHERO */
    uint64_t total = off;       /* tamano del fichero (sin BSS) */

    /* BSS (secciones NOBITS): reciben VAs en memoria TRAS el contenido del
     * fichero pero NO ocupan bytes en el (memsz > filesz).  El cargador las
     * pone a cero. */
    uint64_t bss_mem = region2_end;
    for (int i = 0; i < num_secs; ++i) {
        if (!(secs[i].flags & AOT_SEC_BSS)) continue;
        uint64_t bsz = secs[i].bss_size ? secs[i].bss_size : secs[i].size;
        if (bsz == 0) continue;
        uint64_t a = secs[i].align ? secs[i].align : 8;
        bss_mem = AOT_DYN_ALIGN(bss_mem, a);
        sec_va[i] = bss_mem;
        sec_seen[i] = 1; /* visible para relocs; NO se copia al fichero */
        bss_mem += bsz;
    }
    uint64_t region2_mem_end = bss_mem; /* fin en MEMORIA (incluye BSS) */

    uint64_t entry_vaddr = sec_va[entry_sec] + entry_off;

    /* ================= PASO 2: emitir la imagen ================= */
    uint8_t *img = (uint8_t *)calloc(1, (size_t)total);
    if (!img) {
        set_err(err, err_cap, "elf_dynexec: OOM");
        free(name_off);
        free(sonames);
        free(soname_off);
        free(bucket);
        free(chain);
        free(dynstr.p);
        free(sec_va);
        free(sec_seen);
        return 0;
    }

    /* Ehdr. */
    Elf64_Ehdr *eh = (Elf64_Ehdr *)(img + ehdr_off);
    eh->e_ident[0] = 0x7f;
    eh->e_ident[1] = 'E';
    eh->e_ident[2] = 'L';
    eh->e_ident[3] = 'F';
    eh->e_ident[4] = ELFCLASS64;
    eh->e_ident[5] = 1 /*LSB*/;
    eh->e_ident[6] = 1 /*EV_CURRENT*/;
    eh->e_type = ET_DYN;
    eh->e_machine = EM_X86_64;
    eh->e_version = 1;
    eh->e_entry = entry_vaddr;
    eh->e_phoff = phdr_off;
    eh->e_shoff = 0;
    eh->e_flags = 0;
    eh->e_ehsize = (uint16_t)sizeof(Elf64_Ehdr);
    eh->e_phentsize = (uint16_t)sizeof(Elf64_Phdr);
    eh->e_phnum = (uint16_t)NPH;
    eh->e_shentsize = 0;
    eh->e_shnum = 0;
    eh->e_shstrndx = 0;

    /* Phdrs. */
    Elf64_Phdr *ph = (Elf64_Phdr *)(img + phdr_off);
    memset(ph, 0, (size_t)NPH * sizeof(Elf64_Phdr));
    ph[0].p_type = PT_PHDR;
    ph[0].p_flags = PF_R;
    ph[0].p_offset = phdr_off;
    ph[0].p_vaddr = phdr_off;
    ph[0].p_paddr = phdr_off;
    ph[0].p_filesz = (uint64_t)NPH * sizeof(Elf64_Phdr);
    ph[0].p_memsz = ph[0].p_filesz;
    ph[0].p_align = 8;
    ph[1].p_type = PT_INTERP;
    ph[1].p_flags = PF_R;
    ph[1].p_offset = interp_off;
    ph[1].p_vaddr = interp_off;
    ph[1].p_paddr = interp_off;
    ph[1].p_filesz = interp_len;
    ph[1].p_memsz = interp_len;
    ph[1].p_align = 1;
    ph[2].p_type = PT_LOAD;
    ph[2].p_flags = PF_R | PF_X;
    ph[2].p_offset = 0;
    ph[2].p_vaddr = 0;
    ph[2].p_paddr = 0;
    ph[2].p_filesz = region1_end;
    ph[2].p_memsz = region1_end;
    ph[2].p_align = PAGE;
    ph[3].p_type = PT_LOAD;
    ph[3].p_flags = PF_R | PF_W;
    ph[3].p_offset = region2_start;
    ph[3].p_vaddr = region2_start;
    ph[3].p_paddr = region2_start;
    ph[3].p_filesz = region2_end - region2_start;
    ph[3].p_memsz = region2_mem_end - region2_start; /* incluye BSS */
    ph[3].p_align = PAGE;
    ph[4].p_type = PT_DYNAMIC;
    ph[4].p_flags = PF_R | PF_W;
    ph[4].p_offset = dynamic_off;
    ph[4].p_vaddr = dynamic_off;
    ph[4].p_paddr = dynamic_off;
    ph[4].p_filesz = (uint64_t)ndyn * sizeof(Elf64_Dyn);
    ph[4].p_memsz = ph[4].p_filesz;
    ph[4].p_align = 8;
    ph[5].p_type = PT_GNU_STACK;
    ph[5].p_flags = PF_R | PF_W;
    ph[5].p_align = 0x10;

    /* PT_TLS: plantilla thread_local (local-exec).  El cargador la copia por
     * hilo; las variables se acceden TP-relativas (offsets ya calculados por el
     * linker como TPOFF).  p_filesz = parte .tdata (en fichero), p_memsz =
     * .tdata + .tbss (el cargador alinea via p_align). */
    if (tls_sec >= 0) {
        ph[6].p_type = PT_TLS;
        ph[6].p_flags = PF_R;
        ph[6].p_offset = sec_va[tls_sec]; /* file offset == vaddr (PIE base 0) */
        ph[6].p_vaddr = sec_va[tls_sec];
        ph[6].p_paddr = sec_va[tls_sec];
        ph[6].p_filesz = secs[tls_sec].size;
        ph[6].p_memsz = secs[tls_sec].size + secs[tls_sec].bss_size;
        ph[6].p_align = secs[tls_sec].align ? secs[tls_sec].align : 8;
    }

    /* .interp */
    memcpy(img + interp_off, interp, interp_len);

    /* .dynsym */
    Elf64_Sym *dsym = (Elf64_Sym *)(img + dynsym_off);
    memset(dsym, 0, (size_t)nsym * sizeof(Elf64_Sym));
    for (int i = 0; i < num_imps; ++i) {
        Elf64_Sym *s = &dsym[1 + i];
        s->st_name = name_off[i];
        s->st_info = ELF64_ST_INFO(STB_GLOBAL, STT_NOTYPE);
        s->st_other = 0;
        s->st_shndx = SHN_UNDEF;
        s->st_value = 0;
        s->st_size = 0;
    }
    /* .dynstr */
    memcpy(img + dynstr_off, dynstr.p, dynstr.len);
    /* .hash */
    {
        uint32_t *h = (uint32_t *)(img + hash_off);
        h[0] = nbucket;
        h[1] = nchain;
        for (uint32_t i = 0; i < nbucket; ++i)
            h[2 + i] = bucket[i];
        for (uint32_t i = 0; i < nchain; ++i)
            h[2 + nbucket + i] = chain[i];
    }
    /* .rela.dyn (R_X86_64_GLOB_DAT por entrada GOT) */
    Elf64_Rela *rela = (Elf64_Rela *)(img + rela_off);
    for (int i = 0; i < num_imps; ++i) {
        rela[i].r_offset = got_off + (uint64_t)i * 8u;
        rela[i].r_info = ELF64_R_INFO((uint64_t)(1 + i), R_X86_64_GLOB_DAT);
        rela[i].r_addend = 0;
    }
    /* Secciones de usuario (codigo/rodata + data).  BSS no tiene datos en el
     * fichero (su VA cae mas alla de 'total') -> se omite del memcpy. */
    for (int i = 0; i < num_secs; ++i)
        if (sec_seen[i] && !(secs[i].flags & AOT_SEC_BSS) && secs[i].data)
            memcpy(img + sec_va[i], secs[i].data, secs[i].size);

    /* .dynamic */
    {
        Elf64_Dyn *d = (Elf64_Dyn *)(img + dynamic_off);
        int k = 0;
        for (int s = 0; s < num_sonames; ++s) {
            d[k].d_tag = DT_NEEDED;
            d[k++].d_un.d_val = soname_off[s];
        }
        d[k].d_tag = DT_HASH;
        d[k++].d_un.d_ptr = hash_off;
        d[k].d_tag = DT_STRTAB;
        d[k++].d_un.d_ptr = dynstr_off;
        d[k].d_tag = DT_SYMTAB;
        d[k++].d_un.d_ptr = dynsym_off;
        d[k].d_tag = DT_STRSZ;
        d[k++].d_un.d_val = dynstr.len;
        d[k].d_tag = DT_SYMENT;
        d[k++].d_un.d_val = sizeof(Elf64_Sym);
        d[k].d_tag = DT_RELA;
        d[k++].d_un.d_ptr = rela_off;
        d[k].d_tag = DT_RELASZ;
        d[k++].d_un.d_val = (uint64_t)n_dynrela * sizeof(Elf64_Rela);
        d[k].d_tag = DT_RELAENT;
        d[k++].d_un.d_val = sizeof(Elf64_Rela);
        d[k].d_tag = DT_FLAGS;
        d[k++].d_un.d_val = DF_BIND_NOW;
        d[k].d_tag = (Elf64_Sxword)DT_FLAGS_1;
        d[k++].d_un.d_val = DF_1_NOW;
        d[k].d_tag = DT_NULL;
        d[k++].d_un.d_val = 0;
    }

    /* --- Relocs cross-seccion del driver (data refs / section syms).  PIE:
     * solo PC-relativo (REL32) + inmediatos (IMM32/IMM64=size); ABS64 NO va en
     *     PIE (necesitaria R_X86_64_RELATIVE) -> error claro. --- */
    int ok = 1;
    int n_rel_emitted = 0; /* contador de R_X86_64_RELATIVE ya escritas */
    Elf64_Rela *dynrela = (Elf64_Rela *)(img + rela_off);
    for (int r = 0; r < num_relocs && ok; ++r) {
        const AotReloc *rl = &relocs[r];
        if (rl->site_section < 0 || rl->site_section >= num_secs ||
            !sec_seen[rl->site_section] ||
            (!rl->target_is_size &&
             (rl->target_section < 0 || rl->target_section >= num_secs ||
              !sec_seen[rl->target_section]))) {
            {
                char m[160];
                snprintf(m, sizeof(m),
                         "elf_dynexec: reloc con seccion invalida "
                         "(kind=%d site_sec=%d seen=%d target_sec=%d seen=%d "
                         "is_size=%d num_secs=%d)",
                         rl->kind, rl->site_section,
                         (rl->site_section >= 0 && rl->site_section < num_secs)
                             ? sec_seen[rl->site_section]
                             : -1,
                         rl->target_section,
                         (rl->target_section >= 0 &&
                          rl->target_section < num_secs)
                             ? sec_seen[rl->target_section]
                             : -1,
                         rl->target_is_size, num_secs);
                set_err(err, err_cap, m);
            }
            ok = 0;
            break;
        }
        uint64_t tv;
        if (rl->target_is_size)
            tv = aot_sec_size(&secs[rl->target_section]);
        else {
            tv = sec_va[rl->target_section];
            tv += rl->target_is_end ? aot_sec_size(&secs[rl->target_section])
                                    : rl->target_off;
        }
        tv = (uint64_t)((int64_t)tv + rl->addend);
        uint64_t site_va = sec_va[rl->site_section] + rl->site_off;
        if (rl->kind == AOT_RELOC_TPOFF32) {
            /* TLS local-exec: tpoff = offset_en_el_bloque - aligned_total
             * (variante II: TP al final del bloque TLS, offset negativo).  Es
             * una CONSTANTE de enlace (no depende de la base de carga): se
             * escribe directa, sin reloc dinamica.  El cargador monta el bloque
             * TLS desde PT_TLS antes del entry. */
            if (tls_sec < 0) {
                set_err(err, err_cap,
                        "elf_dynexec: reloc TPOFF32 sin seccion TLS");
                ok = 0;
                break;
            }
            uint64_t talign = secs[tls_sec].align ? secs[tls_sec].align : 8;
            uint64_t tls_total =
                secs[tls_sec].size + secs[tls_sec].bss_size;
            uint64_t aligned_total = (tls_total + talign - 1) & ~(talign - 1);
            /* tv = VA del simbolo TLS; el offset en el bloque = tv -
             * sec_va[tls_sec] (el bloque empieza en la 1a seccion SHF_TLS). */
            int32_t tpoff = (int32_t)((int64_t)(tv - sec_va[tls_sec]) -
                                      (int64_t)aligned_total);
            memcpy(img + site_va, &tpoff, 4);
            continue;
        }
        if (rl->kind == AOT_RELOC_ABS64) {
            /* PIE: el valor absoluto se resuelve en carga.  R_X86_64_RELATIVE
             * con r_addend = tv (relativo a la imagen, base 0) -> el cargador
             * escribe base + tv.  El campo se deja con tv (el cargador lo
             * sobreescribe). */
            memcpy(img + site_va, &tv, 8);
            Elf64_Rela *e = &dynrela[num_imps + n_rel_emitted++];
            e->r_offset = site_va;
            e->r_info = ELF64_R_INFO(0, R_X86_64_RELATIVE);
            e->r_addend = (int64_t)tv;
            continue;
        }
        if (!apply_reloc(img + site_va, site_va, tv, rl->kind)) {
            set_err(err, err_cap, "elf_dynexec: reloc kind invalido");
            ok = 0;
            break;
        }
    }

    /* --- Parchear cada thunk (FF 25 disp32) -> su entrada GOT.  imps[i] <->
     * got[i]. disp32 = got_va - (thunk_va + 6); el FF 25 esta en
     * call_section:call_off. --- */
    for (int i = 0; i < num_imps && ok; ++i) {
        int cs = imps[i].call_section;
        if (cs < 0 || cs >= num_secs || !sec_seen[cs]) {
            set_err(err, err_cap,
                    "elf_dynexec: call_section del thunk invalido");
            ok = 0;
            break;
        }
        uint64_t thunk_va = sec_va[cs] + imps[i].call_off;
        uint64_t got_va = got_off + (uint64_t)i * 8u;
        int32_t disp = (int32_t)((int64_t)got_va - (int64_t)(thunk_va + 6));
        memcpy(img + thunk_va + 2, &disp, 4);
    }

    if (ok) {
        FILE *f = fopen(path, "wb");
        if (!f) {
            set_err(err, err_cap, "elf_dynexec: fopen fallo");
            ok = 0;
        } else {
            size_t w = fwrite(img, 1, (size_t)total, f);
            fclose(f);
            if (w != total) {
                set_err(err, err_cap, "elf_dynexec: escritura incompleta");
                ok = 0;
            }
        }
    }

    free(img);
    free(name_off);
    free(sonames);
    free(soname_off);
    free(bucket);
    free(chain);
    free(dynstr.p);
    free(sec_va);
    free(sec_seen);
    return ok;
}

/* =========================================================================
 *  ELF32 EJECUTABLE DINaMICO (PIE ET_DYN, EM_386) con imports libc via
 *  eager-GOT -- hand-rolled.  Variante 32-bit de aot_emit_elf_dynexec:
 *  enlaza contra libc.so.6 de i386.  Difs 32-bit: Ehdr 52B, Phdr 32B (orden
 *  distinto), Elf32_Sym 16B (info/other/shndx AL FINAL), Elf32_Rel 8B SIN
 *  addend (R_386_GLOB_DAT=6, .rel.dyn -> DT_REL/RELSZ/RELENT), Elf32_Dyn 8B,
 *  interp /lib/ld-linux.so.2, GOT 4B, thunk FF25 disp32 ABSOLUTO (i386 sin
 *  rip-rel; PIE base 0 -> vaddr==file off).
 * ========================================================================= */
#ifndef R_386_GLOB_DAT
#define R_386_GLOB_DAT 6
#endif
#define AOT_ELF32_ST_INFO(b, t) (((b) << 4) | ((t) & 0xf))
#define AOT_ELF32_R_INFO(s, t) (((uint32_t)(s) << 8) | ((t) & 0xff))

int aot_emit_elf32_dynexec(const char *path, const AotLayoutCfg *cfg,
                           const AotSection *secs, int num_secs, int entry_sec,
                           uint64_t entry_off, const AotReloc *relocs,
                           int num_relocs, const AotImport *imps, int num_imps,
                           char *err, size_t err_cap) {
    if (num_secs <= 0) {
        set_err(err, err_cap, "elf32_dynexec: sin secciones");
        return 0;
    }
    if (entry_sec < 0 || entry_sec >= num_secs) {
        set_err(err, err_cap, "elf32_dynexec: entry_sec fuera de rango");
        return 0;
    }
    if (num_imps < 0) num_imps = 0;
    /* num_imps == 0 es valido: un EXEC dinamico sin imports de libc pero con TLS
     * (necesita el cargador para montar el bloque thread-local).  GOT vacia +
     * dynsym de 1 entrada (null) + DT_NEEDED libc.so.6 (monta el TLS estatico). */

    const uint32_t PAGE = (uint32_t)AOT_ELF_PAGE;
    /* Seccion TLS (plantilla thread_local local-exec): si existe, anyade un
     * PT_TLS para que el cargador monte el bloque TLS por-hilo. */
    int tls_sec = -1;
    for (int i = 0; i < num_secs; ++i)
        if (secs[i].flags & AOT_SEC_TLS) { tls_sec = i; break; }
    const int NPH = 6 + (tls_sec >= 0 ? 1 : 0);
    const int nsym = 1 + num_imps;
    const char interp[] = "/lib/ld-linux.so.2";
    const uint32_t interp_len = (uint32_t)sizeof(interp);

    const uint32_t EHDR32 = 52, PHDR32 = 32, SYM32 = 16, REL32SZ = 8, DYN32 = 8;
    /* x86-32 NO tiene rip-relativo -> el thunk usa direccionamiento ABSOLUTO.
     * Un PIE (ET_DYN) se carga en base aleatoria -> el absoluto fallaria.  Por
     * eso este es un ET_EXEC con BASE FIJA (modelo clasico de 32-bit dinamico):
     * vaddr = BASE + file_offset.  Los offsets en disco siguen siendo file
     * offsets (indexan img); las VADDR (e_entry, p_vaddr, DT_*, r_offset, el
     * disp32 del thunk) llevan +BASE. */
    const uint32_t BASE =
        (cfg && cfg->image_base) ? (uint32_t)cfg->image_base : 0x08048000u;

    OBuf dynstr;
    memset(&dynstr, 0, sizeof(dynstr));
    {
        uint8_t z = 0;
        ob_put(&dynstr, &z, 1);
    }
    uint32_t *name_off = (uint32_t *)calloc((size_t)num_imps, sizeof(uint32_t));
    for (int i = 0; i < num_imps; ++i) {
        name_off[i] = (uint32_t)dynstr.len;
        ob_put(&dynstr, imps[i].func, strlen(imps[i].func) + 1);
    }
    uint32_t libc_off = (uint32_t)dynstr.len;
    ob_put(&dynstr, "libc.so.6", 10);

    uint32_t nbucket = (uint32_t)(nsym < 1 ? 1 : nsym);
    uint32_t nchain = (uint32_t)nsym;
    uint32_t *bucket = (uint32_t *)calloc(nbucket, sizeof(uint32_t));
    uint32_t *chain = (uint32_t *)calloc(nchain, sizeof(uint32_t));
    for (int i = 0; i < num_imps; ++i) {
        uint32_t si = (uint32_t)(1 + i);
        uint32_t b = (uint32_t)(aot_elf_hash(imps[i].func) % nbucket);
        chain[si] = bucket[b];
        bucket[b] = si;
    }
    uint32_t hash_size = (uint32_t)(2 + nbucket + nchain) * 4u;

    uint32_t off = 0;
    uint32_t ehdr_off = off;
    off += EHDR32;
    uint32_t phdr_off = off;
    off += (uint32_t)NPH * PHDR32;
    uint32_t interp_off = off;
    off += interp_len;
    off = aot_dyn_align_u32(off, 4);
    uint32_t dynsym_off = off;
    off += (uint32_t)nsym * SYM32;
    uint32_t dynstr_off = off;
    off += (uint32_t)dynstr.len;
    off = aot_dyn_align_u32(off, 4);
    uint32_t hash_off = off;
    off += hash_size;
    off = aot_dyn_align_u32(off, 4);
    uint32_t rel_off = off;
    off += (uint32_t)num_imps * REL32SZ;
    off = aot_dyn_align_u32(off, 16);

    uint32_t *sec_va = (uint32_t *)calloc((size_t)num_secs, sizeof(uint32_t));
    uint8_t *sec_seen = (uint8_t *)calloc((size_t)num_secs, sizeof(uint8_t));
    for (int i = 0; i < num_secs; ++i) {
        if ((secs[i].flags & AOT_SEC_WRITE) || (secs[i].flags & AOT_SEC_BSS))
            continue;
        if (secs[i].size == 0) continue;
        uint32_t a = secs[i].align ? (uint32_t)secs[i].align : 16u;
        off = aot_dyn_align_u32(off, a);
        sec_va[i] = off;
        sec_seen[i] = 1;
        off += (uint32_t)secs[i].size;
    }
    uint32_t region1_end = off;
    off = aot_dyn_align_u32(off, PAGE);
    uint32_t region2_start = off;

    uint32_t got_off = off;
    off += (uint32_t)num_imps * 4u;

    for (int i = 0; i < num_secs; ++i) {
        if (!(secs[i].flags & AOT_SEC_WRITE)) continue;
        if (secs[i].flags & AOT_SEC_BSS) continue;
        if (secs[i].size == 0) continue;
        uint32_t a = secs[i].align ? (uint32_t)secs[i].align : 8u;
        off = aot_dyn_align_u32(off, a);
        sec_va[i] = off;
        sec_seen[i] = 1;
        off += (uint32_t)secs[i].size;
    }
    off = aot_dyn_align_u32(off, 4);
    uint32_t dynamic_off = off;
    const int ndyn = 13;
    off += (uint32_t)ndyn * DYN32;
    uint32_t region2_end = off;
    uint32_t total = off;

    uint32_t entry_vaddr = sec_va[entry_sec] + (uint32_t)entry_off;

    uint8_t *img = (uint8_t *)calloc(1, (size_t)total);
    if (!img) {
        set_err(err, err_cap, "elf32_dynexec: OOM");
        free(name_off);
        free(bucket);
        free(chain);
        free(dynstr.p);
        free(sec_va);
        free(sec_seen);
        return 0;
    }

    img[ehdr_off + 0] = 0x7f;
    img[ehdr_off + 1] = 'E';
    img[ehdr_off + 2] = 'L';
    img[ehdr_off + 3] = 'F';
    img[ehdr_off + 4] = 1;
    img[ehdr_off + 5] = 1;
    img[ehdr_off + 6] = 1;
    img[ehdr_off + 16] = 2;
    img[ehdr_off + 17] = 0; /* e_type = ET_EXEC */
    img[ehdr_off + 18] = 3;
    img[ehdr_off + 19] = 0; /* e_machine = EM_386 */
    wr32(img + ehdr_off + 20, 1);
    wr32(img + ehdr_off + 24, BASE + entry_vaddr);
    wr32(img + ehdr_off + 28, phdr_off);
    wr32(img + ehdr_off + 32, 0);
    wr32(img + ehdr_off + 36, 0);
    img[ehdr_off + 40] = (uint8_t)EHDR32;
    img[ehdr_off + 41] = 0;
    img[ehdr_off + 42] = (uint8_t)PHDR32;
    img[ehdr_off + 43] = 0;
    img[ehdr_off + 44] = (uint8_t)NPH;
    img[ehdr_off + 45] = 0;
    img[ehdr_off + 46] = 0;
    img[ehdr_off + 47] = 0;
    img[ehdr_off + 48] = 0;
    img[ehdr_off + 49] = 0;
    img[ehdr_off + 50] = 0;
    img[ehdr_off + 51] = 0;

    {
        uint8_t *ph;
        ph = img + phdr_off + 0 * PHDR32;
        wr32(ph + 0, 6);
        wr32(ph + 4, phdr_off);
        wr32(ph + 8, BASE + phdr_off);
        wr32(ph + 12, BASE + phdr_off);
        wr32(ph + 16, (uint32_t)NPH * PHDR32);
        wr32(ph + 20, (uint32_t)NPH * PHDR32);
        wr32(ph + 24, PF_R);
        wr32(ph + 28, 4);
        ph = img + phdr_off + 1 * PHDR32;
        wr32(ph + 0, 3);
        wr32(ph + 4, interp_off);
        wr32(ph + 8, BASE + interp_off);
        wr32(ph + 12, BASE + interp_off);
        wr32(ph + 16, interp_len);
        wr32(ph + 20, interp_len);
        wr32(ph + 24, PF_R);
        wr32(ph + 28, 1);
        ph = img + phdr_off + 2 * PHDR32;
        wr32(ph + 0, 1);
        wr32(ph + 4, 0);
        wr32(ph + 8, BASE + 0);
        wr32(ph + 12, BASE + 0);
        wr32(ph + 16, region1_end);
        wr32(ph + 20, region1_end);
        wr32(ph + 24, PF_R | PF_X);
        wr32(ph + 28, PAGE);
        ph = img + phdr_off + 3 * PHDR32;
        wr32(ph + 0, 1);
        wr32(ph + 4, region2_start);
        wr32(ph + 8, BASE + region2_start);
        wr32(ph + 12, BASE + region2_start);
        wr32(ph + 16, region2_end - region2_start);
        wr32(ph + 20, region2_end - region2_start);
        wr32(ph + 24, PF_R | PF_W);
        wr32(ph + 28, PAGE);
        ph = img + phdr_off + 4 * PHDR32;
        wr32(ph + 0, 2);
        wr32(ph + 4, dynamic_off);
        wr32(ph + 8, BASE + dynamic_off);
        wr32(ph + 12, BASE + dynamic_off);
        wr32(ph + 16, (uint32_t)ndyn * DYN32);
        wr32(ph + 20, (uint32_t)ndyn * DYN32);
        wr32(ph + 24, PF_R | PF_W);
        wr32(ph + 28, 4);
        ph = img + phdr_off + 5 * PHDR32;
        wr32(ph + 0, 0x6474e551u);
        wr32(ph + 4, 0);
        wr32(ph + 8, 0);
        wr32(ph + 12, 0);
        wr32(ph + 16, 0);
        wr32(ph + 20, 0);
        wr32(ph + 24, PF_R | PF_W);
        wr32(ph + 28, 0x10);
        if (tls_sec >= 0) {
            /* PT_TLS: plantilla .tdata (filesz) + .tbss (memsz extra).  El
             * cargador la copia por-hilo bajo el thread pointer (gs). */
            ph = img + phdr_off + 6 * PHDR32;
            wr32(ph + 0, 7); /* PT_TLS */
            wr32(ph + 4, sec_va[tls_sec]);
            wr32(ph + 8, BASE + sec_va[tls_sec]);
            wr32(ph + 12, BASE + sec_va[tls_sec]);
            wr32(ph + 16, (uint32_t)secs[tls_sec].size);
            wr32(ph + 20,
                 (uint32_t)(secs[tls_sec].size + secs[tls_sec].bss_size));
            wr32(ph + 24, PF_R);
            wr32(ph + 28, secs[tls_sec].align ? (uint32_t)secs[tls_sec].align
                                              : 4u);
        }
    }

    memcpy(img + interp_off, interp, interp_len);

    for (int i = 0; i < num_imps; ++i) {
        uint8_t *s = img + dynsym_off + (uint32_t)(1 + i) * SYM32;
        wr32(s + 0, name_off[i]);
        wr32(s + 4, 0);
        wr32(s + 8, 0);
        s[12] = (uint8_t)AOT_ELF32_ST_INFO(STB_GLOBAL, STT_NOTYPE);
        s[13] = 0;
        s[14] = (uint8_t)SHN_UNDEF;
        s[15] = 0;
    }
    memcpy(img + dynstr_off, dynstr.p, dynstr.len);
    {
        uint8_t *h = img + hash_off;
        wr32(h + 0, nbucket);
        wr32(h + 4, nchain);
        for (uint32_t i = 0; i < nbucket; ++i)
            wr32(h + 8 + i * 4, bucket[i]);
        for (uint32_t i = 0; i < nchain; ++i)
            wr32(h + 8 + (nbucket + i) * 4, chain[i]);
    }
    for (int i = 0; i < num_imps; ++i) {
        uint8_t *re = img + rel_off + (uint32_t)i * REL32SZ;
        wr32(re + 0,
             BASE + got_off + (uint32_t)i * 4u); /* r_offset = vaddr GOT */
        wr32(re + 4, AOT_ELF32_R_INFO((uint32_t)(1 + i), R_386_GLOB_DAT));
    }
    for (int i = 0; i < num_secs; ++i)
        if (sec_seen[i]) memcpy(img + sec_va[i], secs[i].data, secs[i].size);

    {
        uint8_t *d = img + dynamic_off;
        int k = 0;
        wr32(d + k * DYN32 + 0, DT_NEEDED);
        wr32(d + k * DYN32 + 4, libc_off);
        ++k;
        wr32(d + k * DYN32 + 0, DT_HASH);
        wr32(d + k * DYN32 + 4, BASE + hash_off);
        ++k;
        wr32(d + k * DYN32 + 0, DT_STRTAB);
        wr32(d + k * DYN32 + 4, BASE + dynstr_off);
        ++k;
        wr32(d + k * DYN32 + 0, DT_SYMTAB);
        wr32(d + k * DYN32 + 4, BASE + dynsym_off);
        ++k;
        wr32(d + k * DYN32 + 0, DT_STRSZ);
        wr32(d + k * DYN32 + 4, (uint32_t)dynstr.len);
        ++k;
        wr32(d + k * DYN32 + 0, DT_SYMENT);
        wr32(d + k * DYN32 + 4, SYM32);
        ++k;
        wr32(d + k * DYN32 + 0, DT_REL);
        wr32(d + k * DYN32 + 4, BASE + rel_off);
        ++k;
        wr32(d + k * DYN32 + 0, DT_RELSZ);
        wr32(d + k * DYN32 + 4, (uint32_t)num_imps * REL32SZ);
        ++k;
        wr32(d + k * DYN32 + 0, DT_RELENT);
        wr32(d + k * DYN32 + 4, REL32SZ);
        ++k;
        wr32(d + k * DYN32 + 0, DT_FLAGS);
        wr32(d + k * DYN32 + 4, (uint32_t)DF_BIND_NOW);
        ++k;
        wr32(d + k * DYN32 + 0, (uint32_t)DT_FLAGS_1);
        wr32(d + k * DYN32 + 4, (uint32_t)DF_1_NOW);
        ++k;
        wr32(d + k * DYN32 + 0, DT_NULL);
        wr32(d + k * DYN32 + 4, 0);
        ++k;
        wr32(d + k * DYN32 + 0, DT_NULL);
        wr32(d + k * DYN32 + 4, 0);
        ++k;
    }

    int ok = 1;
    for (int r = 0; r < num_relocs && ok; ++r) {
        const AotReloc *rl = &relocs[r];
        if (rl->kind == AOT_RELOC_ABS64 || rl->kind == AOT_RELOC_IMM64) {
            set_err(err, err_cap,
                    "elf32_dynexec: ABS64/IMM64 no aplica en 32-bit");
            ok = 0;
            break;
        }
        if (rl->site_section < 0 || rl->site_section >= num_secs ||
            !sec_seen[rl->site_section] ||
            (!rl->target_is_size &&
             (rl->target_section < 0 || rl->target_section >= num_secs ||
              !sec_seen[rl->target_section]))) {
            set_err(err, err_cap, "elf32_dynexec: reloc con seccion invalida");
            ok = 0;
            break;
        }
        uint64_t tv;
        if (rl->target_is_size)
            tv = aot_sec_size(&secs[rl->target_section]);
        else {
            tv = sec_va[rl->target_section];
            tv += rl->target_is_end ? aot_sec_size(&secs[rl->target_section])
                                    : rl->target_off;
        }
        tv = (uint64_t)((int64_t)tv + rl->addend);
        uint64_t site_va = sec_va[rl->site_section] + rl->site_off;
        if (!apply_reloc(img + site_va, site_va, tv, rl->kind)) {
            set_err(err, err_cap, "elf32_dynexec: reloc kind invalido");
            ok = 0;
            break;
        }
    }

    for (int i = 0; i < num_imps && ok; ++i) {
        int cs = imps[i].call_section;
        if (cs < 0 || cs >= num_secs || !sec_seen[cs]) {
            set_err(err, err_cap,
                    "elf32_dynexec: call_section del thunk invalido");
            ok = 0;
            break;
        }
        uint32_t thunk_fo =
            sec_va[cs] + (uint32_t)imps[i].call_off;         /* file off */
        uint32_t got_va = BASE + got_off + (uint32_t)i * 4u; /* vaddr GOT */
        wr32(img + thunk_fo + 2, got_va); /* disp32 absoluto (base fija) */
    }

    if (ok) {
        FILE *f = fopen(path, "wb");
        if (!f) {
            set_err(err, err_cap, "elf32_dynexec: fopen fallo");
            ok = 0;
        } else {
            size_t w = fwrite(img, 1, (size_t)total, f);
            fclose(f);
            if (w != total) {
                set_err(err, err_cap, "elf32_dynexec: escritura incompleta");
                ok = 0;
            }
        }
    }

    free(img);
    free(name_off);
    free(bucket);
    free(chain);
    free(dynstr.p);
    free(sec_va);
    free(sec_seen);
    return ok;
}

/* =========================================================================
 *  ELF64 RELOCATABLE (.o, ET_REL) -- hand-rolled (sin _start ni phdrs)
 * ========================================================================= */

/* Constantes ELF para TLS no presentes en CreateELF.h (submodulo): se definen
 * aqui localmente para no tocar el submodulo. */
#ifndef SHF_TLS
#define SHF_TLS 0x400u /* la seccion contiene almacenamiento thread-local */
#endif
#ifndef R_X86_64_TPOFF32
#define R_X86_64_TPOFF32 23 /* TLS local-exec: offset TP-relativo (32-bit) */
#endif

int aot_emit_elf_obj(const char *path, const AotSection *secs, int num_secs,
                     const AotReloc *relocs, int num_relocs, const AotSym *syms,
                     int num_syms, char *err, size_t err_cap) {
    if (num_secs <= 0) {
        set_err(err, err_cap, "aot_emit_elf_obj: sin secciones");
        return 0;
    }

    /* v1: solo refs ADDR (datos/llamadas).  SIZE/END (simbolos de seccion)
     * no se soportan en .o todavia. */
    for (int r = 0; r < num_relocs; ++r) {
        if (relocs[r].target_is_size || relocs[r].target_is_end) {
            set_err(err, err_cap,
                    "aot_emit_elf_obj: SIZE/END no soportado en .o (v1)");
            return 0;
        }
        if (relocs[r].kind != AOT_RELOC_REL32 &&
            relocs[r].kind != AOT_RELOC_ABS64 &&
            relocs[r].kind != AOT_RELOC_TPOFF32) {
            set_err(err, err_cap,
                    "aot_emit_elf_obj: reloc kind no soportado en .o");
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
    if (!sec_nrel) {
        set_err(err, err_cap, "oom");
        return 0;
    }
    for (int r = 0; r < num_relocs; ++r) {
        int s = relocs[r].site_section;
        if (s < 0 || s >= num_secs) {
            free(sec_nrel);
            set_err(err, err_cap, "aot_emit_elf_obj: site_section invalido");
            return 0;
        }
        sec_nrel[s]++;
    }
    const int sym_sh = 1 + num_secs;
    const int str_sh = sym_sh + 1;
    int *rela_sh = (int *)calloc((size_t)num_secs, sizeof(int));
    if (!rela_sh) {
        free(sec_nrel);
        set_err(err, err_cap, "oom");
        return 0;
    }
    int next_sh = str_sh + 1;
    for (int i = 0; i < num_secs; ++i)
        if (sec_nrel[i] > 0) rela_sh[i] = next_sh++;
    const int shstr_sh = next_sh++;
    const int shnum = next_sh;

    /* Simbolos EXTERNOS indefinidos (convencion libc: malloc/free/abort, pero
     * los resuelve el LINKER -> el .o NO depende de libc).  Dedup lineal. */
    const char **extn =
        (const char **)calloc((size_t)num_relocs + 1, sizeof(char *));
    int n_extn = 0;
    if (!extn) {
        free(sec_nrel);
        free(rela_sh);
        set_err(err, err_cap, "oom");
        return 0;
    }
    for (int r = 0; r < num_relocs; ++r) {
        const char *e = relocs[r].extern_name;
        if (!e) continue;
        int found = 0;
        for (int k = 0; k < n_extn; ++k)
            if (strcmp(extn[k], e) == 0) {
                found = 1;
                break;
            }
        if (!found) extn[n_extn++] = e;
    }
    const int extn_base = 1 + num_secs + num_syms; /* indice del 1er externo */

    /* .strtab (nombres de simbolos) + symtab.  Simbolos:
     *   [0]            null
     *   [1..num_secs]  STT_SECTION (LOCAL) por seccion de usuario
     *   [globales...]  STB_GLOBAL (syms[])  -- DESPUES de los locales
     *   [externos...]  STB_GLOBAL indefinidos (SHN_UNDEF) -- al final. */
    OBuf strtab;
    memset(&strtab, 0, sizeof(strtab));
    {
        uint8_t z = 0;
        ob_put(&strtab, &z, 1);
    } /* [0] = "" */
    const int nsym = 1 + num_secs + num_syms + n_extn;
    Elf64_Sym *symtab = (Elf64_Sym *)calloc((size_t)nsym, sizeof(Elf64_Sym));
    if (!symtab) {
        free(sec_nrel);
        free(rela_sh);
        free(strtab.p);
        free(extn);
        set_err(err, err_cap, "oom");
        return 0;
    }
    for (int i = 0; i < num_secs; ++i) {
        symtab[1 + i].st_info = ELF64_ST_INFO(STB_LOCAL, STT_SECTION);
        symtab[1 + i].st_shndx = (Elf64_Half)(1 + i);
    }
    for (int g = 0; g < num_syms; ++g) {
        Elf64_Sym *s = &symtab[1 + num_secs + g];
        s->st_name = (Elf64_Word)strtab.len;
        ob_put(&strtab, syms[g].name, strlen(syms[g].name) + 1);
        s->st_info =
            ELF64_ST_INFO(STB_GLOBAL, syms[g].is_func ? STT_FUNC : STT_OBJECT);
        s->st_shndx = (Elf64_Half)(1 + syms[g].section);
        s->st_value = syms[g].offset;
    }
    /* Externos: indefinidos (SHN_UNDEF), STB_GLOBAL -> el linker los resuelve.
     */
    for (int e = 0; e < n_extn; ++e) {
        Elf64_Sym *s = &symtab[extn_base + e];
        s->st_name = (Elf64_Word)strtab.len;
        ob_put(&strtab, extn[e], strlen(extn[e]) + 1);
        s->st_info = ELF64_ST_INFO(STB_GLOBAL, STT_NOTYPE);
        s->st_shndx = SHN_UNDEF;
        s->st_value = 0;
    }
    const int first_global = 1 + num_secs; /* symtab sh_info */

    /* .rela arrays por seccion. */
    Elf64_Rela **rela =
        (Elf64_Rela **)calloc((size_t)num_secs, sizeof(Elf64_Rela *));
    int *rela_n = (int *)calloc((size_t)num_secs, sizeof(int));
    if (!rela || !rela_n) {
        free(sec_nrel);
        free(rela_sh);
        free(strtab.p);
        free(symtab);
        free(extn);
        free(rela);
        free(rela_n);
        set_err(err, err_cap, "oom");
        return 0;
    }
    for (int i = 0; i < num_secs; ++i)
        if (sec_nrel[i] > 0)
            rela[i] =
                (Elf64_Rela *)calloc((size_t)sec_nrel[i], sizeof(Elf64_Rela));
    for (int r = 0; r < num_relocs; ++r) {
        const AotReloc *rl = &relocs[r];
        Elf64_Rela *re = &rela[rl->site_section][rela_n[rl->site_section]++];
        re->r_offset = rl->site_off;
        if (rl->extern_name) {
            /* Llamada a simbolo externo: PLT32 contra el simbolo indefinido. */
            uint32_t si = 0;
            for (int k = 0; k < n_extn; ++k)
                if (strcmp(extn[k], rl->extern_name) == 0) {
                    si = (uint32_t)(extn_base + k);
                    break;
                }
            re->r_info = ELF64_R_INFO(si, R_X86_64_PLT32);
            re->r_addend = (Elf64_Sxword)(-4) + rl->addend;
            continue;
        }
        const uint32_t sym_idx =
            (uint32_t)(1 + rl->target_section); /* section symbol */
        if (rl->kind == AOT_RELOC_REL32) {
            re->r_info = ELF64_R_INFO(sym_idx, R_X86_64_PC32);
            re->r_addend = (Elf64_Sxword)rl->target_off - 4 + rl->addend;
        } else if (rl->kind == AOT_RELOC_TPOFF32) {
            /* TLS local-exec: R_X86_64_TPOFF32 contra el simbolo de seccion de
             * .tdata (SHF_TLS) + addend = offset.  El --link calcula el TPOFF
             * TP-relativo a partir de tls_off[.tdata] + addend. */
            re->r_info = ELF64_R_INFO(sym_idx, R_X86_64_TPOFF32);
            re->r_addend = (Elf64_Sxword)rl->target_off + rl->addend;
        } else { /* ABS64 */
            re->r_info = ELF64_R_INFO(sym_idx, R_X86_64_64);
            re->r_addend = (Elf64_Sxword)rl->target_off + rl->addend;
        }
    }

    /* .shstrtab + nombres de seccion. */
    OBuf shstr;
    memset(&shstr, 0, sizeof(shstr));
    {
        uint8_t z = 0;
        ob_put(&shstr, &z, 1);
    }
    uint32_t *sec_nameoff =
        (uint32_t *)calloc((size_t)num_secs, sizeof(uint32_t));
    uint32_t *rela_nameoff =
        (uint32_t *)calloc((size_t)num_secs, sizeof(uint32_t));
    for (int i = 0; i < num_secs; ++i) {
        sec_nameoff[i] = (uint32_t)shstr.len;
        ob_put(&shstr, secs[i].name, strlen(secs[i].name) + 1);
    }
    uint32_t no_symtab = (uint32_t)shstr.len;
    ob_put(&shstr, ".symtab", 8);
    uint32_t no_strtab = (uint32_t)shstr.len;
    ob_put(&shstr, ".strtab", 8);
    for (int i = 0; i < num_secs; ++i) {
        if (!rela_sh[i]) continue;
        rela_nameoff[i] = (uint32_t)shstr.len;
        ob_put(&shstr, ".rela", 5);
        ob_put(&shstr, secs[i].name, strlen(secs[i].name) + 1);
    }
    uint32_t no_shstr = (uint32_t)shstr.len;
    ob_put(&shstr, ".shstrtab", 10);

    /* Construir el fichero:
     * [ehdr][secs][symtab][strtab][rela...][shstrtab][shdrs]. */
    OBuf out;
    memset(&out, 0, sizeof(out));
    Elf64_Ehdr eh;
    memset(&eh, 0, sizeof(eh));
    eh.e_ident[0] = 0x7f;
    eh.e_ident[1] = 'E';
    eh.e_ident[2] = 'L';
    eh.e_ident[3] = 'F';
    eh.e_ident[4] = 2;
    eh.e_ident[5] = 1;
    eh.e_ident[6] = 1; /* ELF64, LSB, version 1 */
    eh.e_type = ET_REL;
    eh.e_machine = EM_X86_64;
    eh.e_version = 1;
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
        if (secs[i].flags & AOT_SEC_EXEC) s->sh_flags |= SHF_EXECINSTR;
        if (secs[i].flags & AOT_SEC_WRITE) s->sh_flags |= SHF_WRITE;
        if (secs[i].flags & AOT_SEC_TLS) s->sh_flags |= SHF_TLS; /* .tdata */
        s->sh_offset = sec_off[i];
        s->sh_size = secs[i].size;
        s->sh_addralign = (secs[i].flags & AOT_SEC_EXEC) ? 16 : 8;
    }
    {
        Elf64_Shdr *s = &shdr[sym_sh];
        s->sh_name = no_symtab;
        s->sh_type = SHT_SYMTAB;
        s->sh_offset = symtab_off;
        s->sh_size = (uint64_t)nsym * sizeof(Elf64_Sym);
        s->sh_link = (Elf64_Word)str_sh;
        s->sh_info = (Elf64_Word)first_global;
        s->sh_addralign = 8;
        s->sh_entsize = sizeof(Elf64_Sym);
    }
    {
        Elf64_Shdr *s = &shdr[str_sh];
        s->sh_name = no_strtab;
        s->sh_type = SHT_STRTAB;
        s->sh_offset = strtab_off;
        s->sh_size = strtab.len;
        s->sh_addralign = 1;
    }
    for (int i = 0; i < num_secs; ++i) {
        if (!rela_sh[i]) continue;
        Elf64_Shdr *s = &shdr[rela_sh[i]];
        s->sh_name = rela_nameoff[i];
        s->sh_type = SHT_RELA;
        s->sh_offset = rela_off[i];
        s->sh_size = (uint64_t)rela_n[i] * sizeof(Elf64_Rela);
        s->sh_link = (Elf64_Word)sym_sh;  /* simbolos */
        s->sh_info = (Elf64_Word)(1 + i); /* seccion a la que aplica */
        s->sh_addralign = 8;
        s->sh_entsize = sizeof(Elf64_Rela);
    }
    {
        Elf64_Shdr *s = &shdr[shstr_sh];
        s->sh_name = no_shstr;
        s->sh_type = SHT_STRTAB;
        s->sh_offset = shstr_off;
        s->sh_size = shstr.len;
        s->sh_addralign = 1;
    }
    ob_put(&out, shdr, (size_t)shnum * sizeof(Elf64_Shdr));

    ((Elf64_Ehdr *)out.p)->e_shoff = shoff;

    int ok = 1;
    FILE *f = fopen(path, "wb");
    if (!f) {
        set_err(err, err_cap, "aot_emit_elf_obj: fopen fallo");
        ok = 0;
    } else {
        size_t w = fwrite(out.p, 1, out.len, f);
        fclose(f);
        if (w != out.len) {
            set_err(err, err_cap, "aot_emit_elf_obj: escritura incompleta");
            ok = 0;
        }
    }

    free(sec_nrel);
    free(rela_sh);
    free(strtab.p);
    free(symtab);
    free(extn);
    for (int i = 0; i < num_secs; ++i)
        free(rela[i]);
    free(rela);
    free(rela_n);
    free(shstr.p);
    free(sec_nameoff);
    free(rela_nameoff);
    free(sec_off);
    free(rela_off);
    free(shdr);
    free(out.p);
    return ok;
}

/* =========================================================================
 *  ELF32 RELOCATABLE (.o i386) -- para enlazar con gcc -m32 / ld
 *  Difs vs ELF64: ELFCLASS32, EM_386, structs Elf32 (Ehdr 52 / Shdr 40 /
 *  Sym 16 / Rel 8), y usa SHT_REL (sin addend en el registro: el addend vive
 *  EN el campo de la seccion -- como COFF).  Relocs R_386_PC32 (2) / R_386_32
 *  (1).  Se construye a mano con OBuf (sin depender de structs Elf32).
 * ========================================================================= */
#define R_386_32 1
#define R_386_PC32 2

static void e32_push16(OBuf *o, uint16_t v) {
    uint8_t b[2];
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
    ob_put(o, b, 2);
}
static void e32_push32(OBuf *o, uint32_t v) {
    uint8_t b[4];
    wr32(b, v);
    ob_put(o, b, 4);
}

int aot_emit_elf32_obj(const char *path, const AotSection *secs, int num_secs,
                       const AotReloc *relocs, int num_relocs,
                       const AotSym *syms, int num_syms, char *err,
                       size_t err_cap) {
    if (num_secs <= 0) {
        set_err(err, err_cap, "aot_emit_elf32_obj: sin secciones");
        return 0;
    }
    for (int r = 0; r < num_relocs; ++r) {
        if (relocs[r].target_is_size || relocs[r].target_is_end) {
            set_err(err, err_cap, "aot_emit_elf32_obj: SIZE/END no soportado");
            return 0;
        }
        if (relocs[r].kind != AOT_RELOC_REL32 &&
            relocs[r].kind != AOT_RELOC_IMM32) {
            set_err(err, err_cap,
                    "aot_emit_elf32_obj: reloc kind no soportado (32-bit usa "
                    "REL32/ABS32, no ABS64)");
            return 0;
        }
        if (relocs[r].site_section < 0 || relocs[r].site_section >= num_secs) {
            set_err(err, err_cap, "aot_emit_elf32_obj: site_section invalido");
            return 0;
        }
    }
    /* Indices de section headers: 0=NULL, 1..N=user, sym, str, rel[i], shstr. */
    int *sec_nrel = (int *)calloc((size_t)num_secs, sizeof(int));
    for (int r = 0; r < num_relocs; ++r)
        sec_nrel[relocs[r].site_section]++;
    const int sym_sh = 1 + num_secs;
    const int str_sh = sym_sh + 1;
    int *rel_sh = (int *)calloc((size_t)num_secs, sizeof(int));
    int next_sh = str_sh + 1;
    for (int i = 0; i < num_secs; ++i)
        if (sec_nrel[i] > 0) rel_sh[i] = next_sh++;
    const int shstr_sh = next_sh++;
    const int shnum = next_sh;

    /* Externos dedup. */
    const char **extn =
        (const char **)calloc((size_t)num_relocs + 1, sizeof(char *));
    int n_extn = 0;
    for (int r = 0; r < num_relocs; ++r) {
        const char *e = relocs[r].extern_name;
        if (!e) continue;
        int found = 0;
        for (int k = 0; k < n_extn; ++k)
            if (strcmp(extn[k], e) == 0) {
                found = 1;
                break;
            }
        if (!found) extn[n_extn++] = e;
    }
    const int extn_base = 1 + num_secs + num_syms;

    /* .strtab + symtab (16 bytes/sym, construido en OBuf). */
    OBuf strtab;
    memset(&strtab, 0, sizeof(strtab));
    {
        uint8_t z = 0;
        ob_put(&strtab, &z, 1);
    }
    const int nsym = 1 + num_secs + num_syms + n_extn;
    OBuf symtab;
    memset(&symtab, 0, sizeof(symtab));
    ob_put(&symtab, NULL, 16); /* sym[0] = null */
    for (int i = 0; i < num_secs; ++i) {
        e32_push32(&symtab, 0);                /* st_name */
        e32_push32(&symtab, 0);                /* st_value */
        e32_push32(&symtab, 0);                /* st_size */
        uint8_t info = (uint8_t)((STB_LOCAL << 4) | STT_SECTION);
        ob_put(&symtab, &info, 1);             /* st_info */
        uint8_t other = 0;
        ob_put(&symtab, &other, 1);            /* st_other */
        e32_push16(&symtab, (uint16_t)(1 + i)); /* st_shndx */
    }
    for (int g = 0; g < num_syms; ++g) {
        e32_push32(&symtab, (uint32_t)strtab.len);
        ob_put(&strtab, syms[g].name, strlen(syms[g].name) + 1);
        e32_push32(&symtab, (uint32_t)syms[g].offset); /* st_value */
        e32_push32(&symtab, 0);                        /* st_size */
        uint8_t info = (uint8_t)((STB_GLOBAL << 4) |
                                 (syms[g].is_func ? STT_FUNC : STT_OBJECT));
        ob_put(&symtab, &info, 1);
        uint8_t other = 0;
        ob_put(&symtab, &other, 1);
        e32_push16(&symtab, (uint16_t)(1 + syms[g].section));
    }
    for (int e = 0; e < n_extn; ++e) {
        e32_push32(&symtab, (uint32_t)strtab.len);
        ob_put(&strtab, extn[e], strlen(extn[e]) + 1);
        e32_push32(&symtab, 0); /* st_value */
        e32_push32(&symtab, 0); /* st_size */
        uint8_t info = (uint8_t)((STB_GLOBAL << 4) | STT_NOTYPE);
        ob_put(&symtab, &info, 1);
        uint8_t other = 0;
        ob_put(&symtab, &other, 1);
        e32_push16(&symtab, (uint16_t)SHN_UNDEF);
    }
    const int first_global = 1 + num_secs;

    /* .shstrtab. */
    OBuf shstr;
    memset(&shstr, 0, sizeof(shstr));
    {
        uint8_t z = 0;
        ob_put(&shstr, &z, 1);
    }
    uint32_t *sec_nameoff = (uint32_t *)calloc((size_t)num_secs, 4);
    uint32_t *rel_nameoff = (uint32_t *)calloc((size_t)num_secs, 4);
    for (int i = 0; i < num_secs; ++i) {
        sec_nameoff[i] = (uint32_t)shstr.len;
        ob_put(&shstr, secs[i].name, strlen(secs[i].name) + 1);
    }
    uint32_t no_symtab = (uint32_t)shstr.len;
    ob_put(&shstr, ".symtab", 8);
    uint32_t no_strtab = (uint32_t)shstr.len;
    ob_put(&shstr, ".strtab", 8);
    for (int i = 0; i < num_secs; ++i) {
        if (!rel_sh[i]) continue;
        rel_nameoff[i] = (uint32_t)shstr.len;
        ob_put(&shstr, ".rel", 4);
        ob_put(&shstr, secs[i].name, strlen(secs[i].name) + 1);
    }
    uint32_t no_shstr = (uint32_t)shstr.len;
    ob_put(&shstr, ".shstrtab", 10);

    /* Construir el fichero. */
    OBuf out;
    memset(&out, 0, sizeof(out));
    /* Elf32_Ehdr (52). */
    uint8_t eh[52];
    memset(eh, 0, sizeof(eh));
    eh[0] = 0x7f;
    eh[1] = 'E';
    eh[2] = 'L';
    eh[3] = 'F';
    eh[4] = 1; /* ELFCLASS32 */
    eh[5] = 1; /* LSB */
    eh[6] = 1; /* version */
    wr16le(eh + 16, ET_REL);
    wr16le(eh + 18, 3 /* EM_386 */);
    wr32(eh + 20, 1); /* e_version */
    wr16le(eh + 40, 52); /* e_ehsize */
    wr16le(eh + 46, 40); /* e_shentsize */
    wr16le(eh + 48, (uint16_t)shnum);
    wr16le(eh + 50, (uint16_t)shstr_sh);
    ob_put(&out, eh, 52);

    uint32_t *sec_off = (uint32_t *)calloc((size_t)num_secs, 4);
    for (int i = 0; i < num_secs; ++i) {
        ob_align(&out, 16);
        sec_off[i] = (uint32_t)out.len;
        ob_put(&out, secs[i].data, secs[i].size);
    }
    /* Pre-escribir el addend EN el campo de cada sitio (REL i386 sin addend en
     * el registro).  REL32: A = target_off + addend - 4.  ABS32: A =
     * target_off + addend.  Extern (REL32): A = addend - 4. */
    for (int r = 0; r < num_relocs; ++r) {
        const AotReloc *rl = &relocs[r];
        int64_t A;
        if (rl->extern_name)
            A = rl->addend - 4;
        else if (rl->kind == AOT_RELOC_REL32)
            A = (int64_t)rl->target_off + rl->addend - 4;
        else /* IMM32 -> ABS32 */
            A = (int64_t)rl->target_off + rl->addend;
        wr32(out.p + sec_off[rl->site_section] + rl->site_off, (uint32_t)A);
    }
    ob_align(&out, 4);
    uint32_t symtab_off = (uint32_t)out.len;
    ob_put(&out, symtab.p, symtab.len);
    uint32_t strtab_off = (uint32_t)out.len;
    ob_put(&out, strtab.p, strtab.len);
    uint32_t *rel_off = (uint32_t *)calloc((size_t)num_secs, 4);
    int *rel_cnt = (int *)calloc((size_t)num_secs, sizeof(int));
    for (int i = 0; i < num_secs; ++i) {
        if (!rel_sh[i]) continue;
        ob_align(&out, 4);
        rel_off[i] = (uint32_t)out.len;
        /* Emitir los Elf32_Rel de esta seccion en orden. */
        for (int r = 0; r < num_relocs; ++r) {
            const AotReloc *rl = &relocs[r];
            if (rl->site_section != i) continue;
            uint32_t sym_idx;
            uint32_t type;
            if (rl->extern_name) {
                sym_idx = 0;
                for (int k = 0; k < n_extn; ++k)
                    if (strcmp(extn[k], rl->extern_name) == 0) {
                        sym_idx = (uint32_t)(extn_base + k);
                        break;
                    }
                type = R_386_PC32;
            } else {
                sym_idx = (uint32_t)(1 + rl->target_section); /* section sym */
                type = (rl->kind == AOT_RELOC_REL32) ? R_386_PC32 : R_386_32;
            }
            e32_push32(&out, (uint32_t)rl->site_off); /* r_offset */
            e32_push32(&out, (sym_idx << 8) | (type & 0xff)); /* r_info */
            rel_cnt[i]++;
        }
    }
    uint32_t shstr_off = (uint32_t)out.len;
    ob_put(&out, shstr.p, shstr.len);
    ob_align(&out, 4);
    uint32_t shoff = (uint32_t)out.len;

    /* Section headers (40 bytes c/u). */
    /* helper para empujar un Elf32_Shdr. */
#define E32_SHDR(name, type, flags, off, size, link, info, align, entsz)        \
    do {                                                                        \
        e32_push32(&out, (name));                                               \
        e32_push32(&out, (type));                                               \
        e32_push32(&out, (flags));                                              \
        e32_push32(&out, 0); /* sh_addr */                                      \
        e32_push32(&out, (off));                                                \
        e32_push32(&out, (size));                                               \
        e32_push32(&out, (link));                                               \
        e32_push32(&out, (info));                                               \
        e32_push32(&out, (align));                                              \
        e32_push32(&out, (entsz));                                              \
    } while (0)
    /* indice 0: NULL */
    E32_SHDR(0, 0, 0, 0, 0, 0, 0, 0, 0);
    for (int i = 0; i < num_secs; ++i) {
        uint32_t fl = SHF_ALLOC;
        if (secs[i].flags & AOT_SEC_EXEC) fl |= SHF_EXECINSTR;
        if (secs[i].flags & AOT_SEC_WRITE) fl |= SHF_WRITE;
        uint32_t al = (secs[i].flags & AOT_SEC_EXEC) ? 16 : 8;
        E32_SHDR(sec_nameoff[i], SHT_PROGBITS, fl, sec_off[i],
                 (uint32_t)secs[i].size, 0, 0, al, 0);
    }
    E32_SHDR(no_symtab, SHT_SYMTAB, 0, symtab_off, (uint32_t)symtab.len,
             (uint32_t)str_sh, (uint32_t)first_global, 4, 16);
    E32_SHDR(no_strtab, SHT_STRTAB, 0, strtab_off, (uint32_t)strtab.len, 0, 0, 1,
             0);
    for (int i = 0; i < num_secs; ++i) {
        if (!rel_sh[i]) continue;
        E32_SHDR(rel_nameoff[i], 9 /* SHT_REL */, 0, rel_off[i],
                 (uint32_t)(rel_cnt[i] * 8), (uint32_t)sym_sh, (uint32_t)(1 + i),
                 4, 8);
    }
    E32_SHDR(no_shstr, SHT_STRTAB, 0, shstr_off, (uint32_t)shstr.len, 0, 0, 1, 0);
#undef E32_SHDR

    wr32(out.p + 32, shoff); /* e_shoff */

    int ok = 1;
    FILE *f = fopen(path, "wb");
    if (!f) {
        set_err(err, err_cap, "aot_emit_elf32_obj: fopen fallo");
        ok = 0;
    } else {
        size_t w = fwrite(out.p, 1, out.len, f);
        fclose(f);
        if (w != out.len) {
            set_err(err, err_cap, "aot_emit_elf32_obj: escritura incompleta");
            ok = 0;
        }
    }
    free(sec_nrel);
    free(rel_sh);
    free(extn);
    free(strtab.p);
    free(symtab.p);
    free(shstr.p);
    free(sec_nameoff);
    free(rel_nameoff);
    free(sec_off);
    free(rel_off);
    free(rel_cnt);
    free(out.p);
    return ok;
}

/* =========================================================================
 *  COFF RELOCATABLE (.obj Windows) -- via LibCOFFparse
 * ========================================================================= */

/* Storage classes / tipos COFF (PE/COFF spec). */
#define AOT_COFF_SYM_EXTERNAL 2
#define AOT_COFF_SYM_STATIC 3
#define AOT_COFF_SYM_FUNC 0x20 /* DTYPE function << 4 */

/* IMAGE_REL_I386_* (COFF de 32-bit). */
#define IMAGE_REL_I386_DIR32 6
#define IMAGE_REL_I386_REL32 20

/* Impl comun COFF .obj para AMD64 (is32=0) e i386 (is32=1).  Difs: Machine,
 * tipos de reloc, y que i386 NO tiene ABS64 (usa DIR32 para datos abs32). */
static int coff_obj_impl(const char *path, const AotSection *secs, int num_secs,
                         const AotReloc *relocs, int num_relocs,
                         const AotSym *syms, int num_syms, char *err,
                         size_t err_cap, int is32) {
    if (num_secs <= 0) {
        set_err(err, err_cap, "aot_emit_coff_obj: sin secciones");
        return 0;
    }
    for (int r = 0; r < num_relocs; ++r) {
        if (relocs[r].target_is_size || relocs[r].target_is_end) {
            set_err(err, err_cap,
                    "aot_emit_coff_obj: SIZE/END no soportado en .obj (v1)");
            return 0;
        }
        const int kind_ok =
            relocs[r].kind == AOT_RELOC_REL32 ||
            relocs[r].kind == AOT_RELOC_SECREL32 || /* TLS (thread_local) */
            (is32 ? relocs[r].kind == AOT_RELOC_IMM32
                  : relocs[r].kind == AOT_RELOC_ABS64);
        if (!kind_ok) {
            set_err(err, err_cap,
                    "aot_emit_coff_obj: reloc kind no soportado en .obj");
            return 0;
        }
        if (relocs[r].site_section < 0 || relocs[r].site_section >= num_secs) {
            set_err(err, err_cap,
                    "aot_emit_coff_obj: site_section fuera de rango");
            return 0;
        }
        /* Los externos no tienen target_section (lo resuelve el linker). */
        if (!relocs[r].extern_name && (relocs[r].target_section < 0 ||
                                       relocs[r].target_section >= num_secs)) {
            set_err(err, err_cap,
                    "aot_emit_coff_obj: target_section fuera de rango");
            return 0;
        }
    }

    /* Copias mutables de los datos de cada seccion: COFF lleva el ADDEND en el
     * campo (no en un record aparte), asi que pre-escribimos target_off en el
     * sitio de cada reloc (4 bytes REL32 / 8 bytes ABS64). */
    uint8_t **data = (uint8_t **)calloc((size_t)num_secs, sizeof(uint8_t *));
    if (!data) {
        set_err(err, err_cap, "oom");
        return 0;
    }
    for (int i = 0; i < num_secs; ++i) {
        data[i] = (uint8_t *)malloc(secs[i].size ? secs[i].size : 1);
        if (secs[i].size) memcpy(data[i], secs[i].data, secs[i].size);
    }
    for (int r = 0; r < num_relocs; ++r) {
        const AotReloc *rl = &relocs[r];
        const uint32_t width = (rl->kind == AOT_RELOC_ABS64) ? 8u : 4u;
        if (rl->site_off + width > secs[rl->site_section].size) {
            for (int i = 0; i < num_secs; ++i)
                free(data[i]);
            free(data);
            set_err(err, err_cap,
                    "aot_emit_coff_obj: reloc fuera de la seccion del sitio");
            return 0;
        }
        /* Externo (call a simbolo indefinido): IMAGE_REL_AMD64_REL32 ya es
         * relativo al byte siguiente; el campo se deja en 0 (sin pre-escribir
         * addend) -> el linker calcula sym - (site+4). */
        if (rl->extern_name) continue;
        const uint64_t addend =
            (uint64_t)((int64_t)rl->target_off + rl->addend);
        uint8_t *p = data[rl->site_section] + rl->site_off;
        if (width == 8)
            wr64le(p, addend);
        else
            wr32le(p, (uint32_t)addend);
    }

    /* Externos unicos (convencion libc; los resuelve el linker). */
    const char **extn =
        (const char **)calloc((size_t)num_relocs + 1, sizeof(char *));
    int n_extn = 0;
    if (extn)
        for (int r = 0; r < num_relocs; ++r) {
            const char *e = relocs[r].extern_name;
            if (!e) continue;
            int found = 0;
            for (int k = 0; k < n_extn; ++k)
                if (strcmp(extn[k], e) == 0) {
                    found = 1;
                    break;
                }
            if (!found) extn[n_extn++] = e;
        }

    /* Secciones COFF (las relocs se añaden con add_relocation). */
    NewSection *ns = (NewSection *)calloc((size_t)num_secs, sizeof(NewSection));
    for (int i = 0; i < num_secs; ++i) {
        uint32_t chars = 0;
        if (secs[i].flags & AOT_SEC_CODE) chars |= IMAGE_SCN_CNT_CODE;
        if (secs[i].flags & AOT_SEC_DATA)
            chars |= IMAGE_SCN_CNT_INITIALIZED_DATA;
        if (secs[i].flags & AOT_SEC_EXEC) chars |= IMAGE_SCN_MEM_EXECUTE;
        if (secs[i].flags & AOT_SEC_READ) chars |= IMAGE_SCN_MEM_READ;
        if (secs[i].flags & AOT_SEC_WRITE) chars |= IMAGE_SCN_MEM_WRITE;
        ns[i] = create_section(secs[i].name, chars, data[i],
                               secs[i].size ? secs[i].size : 0, NULL, 0);
    }
    /* Relocs -> registros COFF.  Internos: contra el simbolo de SECCION del
     * target (indices 0..num_secs-1).  Externos: contra el simbolo externo
     * (indices num_secs+num_syms..) con REL32. */
    for (int r = 0; r < num_relocs; ++r) {
        const AotReloc *rl = &relocs[r];
        uint32_t sym_idx;
        uint16_t type;
        if (rl->extern_name) {
            int k = 0;
            for (; k < n_extn; ++k)
                if (strcmp(extn[k], rl->extern_name) == 0) break;
            sym_idx = (uint32_t)(num_secs + num_syms + k);
            type = is32 ? (uint16_t)IMAGE_REL_I386_REL32
                        : (uint16_t)IMAGE_REL_AMD64_REL32;
        } else {
            sym_idx = (uint32_t)rl->target_section;
            if (is32)
                type = (rl->kind == AOT_RELOC_SECREL32)
                           ? (uint16_t)IMAGE_REL_I386_SECREL
                       : (rl->kind == AOT_RELOC_REL32)
                           ? (uint16_t)IMAGE_REL_I386_REL32
                           : (uint16_t)IMAGE_REL_I386_DIR32;
            else
                type = (rl->kind == AOT_RELOC_SECREL32)
                           ? (uint16_t)IMAGE_REL_AMD64_SECREL
                       : (rl->kind == AOT_RELOC_ABS64)
                           ? (uint16_t)IMAGE_REL_AMD64_ADDR64
                           : (uint16_t)IMAGE_REL_AMD64_REL32;
        }
        add_relocation(
            &ns[rl->site_section],
            create_relocation((uint32_t)rl->site_off, sym_idx, type));
    }

    SECTION_HEADER *sh =
        (SECTION_HEADER *)calloc((size_t)num_secs, sizeof(SECTION_HEADER));
    setup_sections(sh, ns, num_secs);
    /* En un .obj la VirtualAddress de cada seccion es 0 (la fija el linker). */
    for (int i = 0; i < num_secs; ++i)
        sh[i].VirtualAddress = 0;

    /* Simbolos: [0..num_secs-1] seccion (STATIC), [globales], [externos]. */
    const int nsym = num_secs + num_syms + n_extn;
    COFF_SYMBOL *symtab =
        (COFF_SYMBOL *)calloc((size_t)nsym, sizeof(COFF_SYMBOL));
    /* String table: 4 bytes de tamano + nombres > 8 chars. */
    char *strtab = (char *)calloc(1, 4);
    uint32_t strtab_size = 4;
/* Helper inline: setear nombre (inline si <=8, si no offset a string table). */
#define AOT_COFF_SET_NAME(SYM, NM)                                             \
    do {                                                                       \
        const char *nm_ = (NM);                                                \
        size_t ln_ = strlen(nm_);                                              \
        if (ln_ <= 8) {                                                        \
            memset((SYM).Name.ShortName, 0, 8);                                \
            memcpy((SYM).Name.ShortName, nm_, ln_);                            \
        } else {                                                               \
            (SYM).Name.LongName.Zero = 0;                                      \
            (SYM).Name.LongName.Offset = strtab_size;                          \
            strtab = (char *)realloc(strtab, strtab_size + ln_ + 1);           \
            memcpy(strtab + strtab_size, nm_, ln_ + 1);                        \
            strtab_size += (uint32_t)(ln_ + 1);                                \
        }                                                                      \
    } while (0)

    for (int i = 0; i < num_secs; ++i) {
        AOT_COFF_SET_NAME(symtab[i], secs[i].name);
        symtab[i].Value = 0;
        symtab[i].SectionNumber = (int16_t)(i + 1);
        symtab[i].Type = 0;
        symtab[i].StorageClass = AOT_COFF_SYM_STATIC;
        symtab[i].NumberOfAuxSymbols = 0;
    }
    for (int g = 0; g < num_syms; ++g) {
        COFF_SYMBOL *s = &symtab[num_secs + g];
        AOT_COFF_SET_NAME(*s, syms[g].name);
        s->Value = (uint32_t)syms[g].offset;
        s->SectionNumber = (int16_t)(syms[g].section + 1);
        s->Type = syms[g].is_func ? AOT_COFF_SYM_FUNC : 0;
        s->StorageClass = AOT_COFF_SYM_EXTERNAL;
        s->NumberOfAuxSymbols = 0;
    }
    /* Externos: indefinidos (SectionNumber=0), EXTERNAL -> el linker resuelve.
     */
    for (int e = 0; e < n_extn; ++e) {
        COFF_SYMBOL *s = &symtab[num_secs + num_syms + e];
        AOT_COFF_SET_NAME(*s, extn[e]);
        s->Value = 0;
        s->SectionNumber = 0; /* IMAGE_SYM_UNDEFINED */
        s->Type = AOT_COFF_SYM_FUNC;
        s->StorageClass = AOT_COFF_SYM_EXTERNAL;
        s->NumberOfAuxSymbols = 0;
    }
#undef AOT_COFF_SET_NAME

    COFF_HEADER header;
    memset(&header, 0, sizeof(header));
    header.Machine =
        is32 ? 0x14c /* I386 */ : 0x8664 /* AMD64 */;
    header.NumberOfSections = (uint16_t)num_secs;
    header.NumberOfSymbols = (uint32_t)nsym;
    header.SizeOfOptionalHeader = 0;
    header.Characteristics = 0;
    header.PointerToSymbolTable = calculate_symbol_table_offset(sh, num_secs);

    int rc = create_coff_file(path, &header, sh, ns, num_secs, symtab,
                              (uint32_t)nsym, strtab, strtab_size);
    int ok = (rc == 0);
    if (!ok) set_err(err, err_cap, "aot_emit_coff_obj: create_coff_file fallo");

    cleanup_resources(ns, num_secs); /* libera ns[i].data + relocations */
    free(ns);
    free(sh);
    free(symtab);
    free(strtab);
    free(extn);
    for (int i = 0; i < num_secs; ++i)
        free(data[i]);
    free(data);
    return ok;
}

/* COFF .obj AMD64 (64-bit). */
int aot_emit_coff_obj(const char *path, const AotSection *secs, int num_secs,
                      const AotReloc *relocs, int num_relocs,
                      const AotSym *syms, int num_syms, char *err,
                      size_t err_cap) {
    return coff_obj_impl(path, secs, num_secs, relocs, num_relocs, syms,
                         num_syms, err, err_cap, /*is32=*/0);
}

/* COFF .obj i386 (32-bit). */
int aot_emit_coff32_obj(const char *path, const AotSection *secs, int num_secs,
                        const AotReloc *relocs, int num_relocs,
                        const AotSym *syms, int num_syms, char *err,
                        size_t err_cap) {
    return coff_obj_impl(path, secs, num_secs, relocs, num_relocs, syms,
                         num_syms, err, err_cap, /*is32=*/1);
}

/* =========================================================================
 *  ELF .so (ET_DYN) -- via CreateELF::elf_create_shared64
 * ========================================================================= */

int aot_emit_elf_so(const char *path, const AotSection *secs, int num_secs,
                    const AotReloc *relocs, int num_relocs, const AotSym *syms,
                    int num_syms, char *err, size_t err_cap) {
    if (num_secs <= 0) {
        set_err(err, err_cap, "aot_emit_elf_so: sin secciones");
        return 0;
    }

    ElfSharedSection *es =
        (ElfSharedSection *)calloc((size_t)num_secs, sizeof(ElfSharedSection));
    for (int i = 0; i < num_secs; ++i) {
        uint64_t f = SHF_ALLOC;
        if (secs[i].flags & AOT_SEC_EXEC) f |= SHF_EXECINSTR;
        if (secs[i].flags & AOT_SEC_WRITE) f |= SHF_WRITE;
        es[i].name = secs[i].name;
        es[i].sh_flags = f;
        es[i].data = secs[i].data;
        es[i].size = secs[i].size;
    }
    ElfSharedReloc *er = NULL;
    if (num_relocs > 0)
        er = (ElfSharedReloc *)calloc((size_t)num_relocs,
                                      sizeof(ElfSharedReloc));
    for (int r = 0; r < num_relocs; ++r) {
        if (relocs[r].target_is_size || relocs[r].target_is_end) {
            free(es);
            free(er);
            set_err(err, err_cap,
                    "aot_emit_elf_so: SIZE/END no soportado en .so (v1)");
            return 0;
        }
        er[r].site_sec = relocs[r].site_section;
        er[r].site_off = relocs[r].site_off;
        er[r].target_sec = relocs[r].target_section;
        er[r].target_off = relocs[r].target_off + (uint64_t)relocs[r].addend;
        er[r].is_abs64 = (relocs[r].kind == AOT_RELOC_ABS64 ||
                          relocs[r].kind == AOT_RELOC_IMM64)
                             ? 1
                             : 0;
    }
    ElfSharedExport *ex = NULL;
    if (num_syms > 0)
        ex = (ElfSharedExport *)calloc((size_t)num_syms,
                                       sizeof(ElfSharedExport));
    for (int g = 0; g < num_syms; ++g) {
        ex[g].name = syms[g].name;
        ex[g].sec = syms[g].section;
        ex[g].off = syms[g].offset;
        ex[g].is_func = syms[g].is_func;
    }

    int ok = elf_create_shared64(path, es, num_secs, er, num_relocs, ex,
                                 num_syms, err, err_cap);
    free(es);
    free(er);
    free(ex);
    return ok;
}

/* =========================================================================
 *  PE DLL (.dll) -- via CreatePe + tabla de exports (.edata) hand-rolled
 * ========================================================================= */

int aot_emit_pe_dll(const char *path, const AotLayoutCfg *cfg,
                    const AotSection *secs, int num_secs,
                    const AotReloc *relocs, int num_relocs, const AotSym *syms,
                    int num_syms, char *err, size_t err_cap) {
    if (num_secs <= 0) {
        set_err(err, err_cap, "aot_emit_pe_dll: sin secciones");
        return 0;
    }
    if (num_syms <= 0) {
        set_err(err, err_cap, "aot_emit_pe_dll: sin simbolos a exportar");
        return 0;
    }

    PE64FILE_struct pe;
    initializePE64File(&pe);
    /* Base de carga: por defecto 0x10000000 (base clasica de DLL, libre); el
     * usuario puede fijar otra.  Codigo PIC -> sin .reloc -> base fija. */
    pe.ntHeaders.OptionalHeader.ImageBase =
        (cfg && cfg->image_base) ? cfg->image_base : 0x10000000ull;
    if (cfg) {
        if (cfg->section_align)
            pe.ntHeaders.OptionalHeader.SectionAlignment = cfg->section_align;
        if (cfg->file_align)
            pe.ntHeaders.OptionalHeader.FileAlignment = cfg->file_align;
    }

    /* Secciones de usuario. */
    for (int i = 0; i < num_secs; ++i) {
        if (secs[i].flags & AOT_SEC_BSS)
            addBssSection(&pe, secs[i].name, secs[i].bss_size);
        else
            addSection(&pe, secs[i].name, pe_section_chars(secs[i].flags),
                       (_BYTE *)secs[i].data, secs[i].size);
    }

    /* TLS (thread_local) en la .dll: sintetizar el IMAGE_TLS_DIRECTORY +
     * _tls_index igual que en el .exe.  El cargador de Windows procesa el
     * directorio TLS tambien para DLLs (Vista+: tambien las cargadas con
     * LoadLibrary), montando el bloque por-hilo. */
    uint64_t tls_index_va = aot_pe_synth_tls(&pe, secs, num_secs, 1, cfg ? cfg->tls_callback_section : -1, cfg ? cfg->tls_callback_off : 0);

    /* Aplicar relocs (PIC: REL32 internas).  Mismo patron que aot_emit_pe. */
    {
        const uint64_t image_base = pe.ntHeaders.OptionalHeader.ImageBase;
        for (int r = 0; r < num_relocs; ++r) {
            const AotReloc *rl = &relocs[r];
            /* TLS PE: __vex_tls_index (RIP-rel) + SECREL32 (offset en .tls). */
            if (aot_pe_apply_tls_reloc(&pe, secs, num_secs, rl, tls_index_va))
                continue;
            if (rl->target_is_size || rl->target_is_end) {
                set_err(err, err_cap, "aot_emit_pe_dll: SIZE/END no soportado");
                freePE64File(&pe);
                return 0;
            }
            if (rl->site_section < 0 || rl->site_section >= num_secs ||
                rl->target_section < 0 || rl->target_section >= num_secs) {
                set_err(err, err_cap,
                        "aot_emit_pe_dll: reloc con seccion fuera de rango");
                freePE64File(&pe);
                return 0;
            }
            uint64_t target_value =
                image_base +
                pe.sectionHeaders[rl->target_section].VirtualAddress +
                rl->target_off;
            target_value = (uint64_t)((int64_t)target_value + rl->addend);
            uint64_t site_va =
                image_base +
                pe.sectionHeaders[rl->site_section].VirtualAddress +
                rl->site_off;
            uint32_t width =
                (rl->kind == AOT_RELOC_ABS64 || rl->kind == AOT_RELOC_IMM64)
                    ? 8u
                    : 4u;
            if (rl->site_off + width > secs[rl->site_section].size) {
                set_err(err, err_cap,
                        "aot_emit_pe_dll: reloc fuera de la seccion del sitio");
                freePE64File(&pe);
                return 0;
            }
            apply_reloc(pe.sectionData[rl->site_section] + rl->site_off,
                        site_va, target_value, rl->kind);
        }
    }

    /* Tabla de exports (.edata).  Predecir su RVA (siguiente seccion). */
    const ___IMAGE_SECTION_HEADER *last =
        &pe.sectionHeaders[pe.numberOfSections - 1];
    _DWORD edataRVA = align(last->VirtualAddress + last->Misc.VirtualSize,
                            pe.ntHeaders.OptionalHeader.SectionAlignment);

    /* Orden de exports por nombre (GetProcAddress hace busqueda binaria: la
     * name pointer table DEBE estar ordenada ascendente). */
    int *order = (int *)malloc((size_t)num_syms * sizeof(int));
    for (int i = 0; i < num_syms; ++i)
        order[i] = i;
    for (int i = 0; i < num_syms; ++i)
        for (int j = i + 1; j < num_syms; ++j)
            if (strcmp(syms[order[j]].name, syms[order[i]].name) < 0) {
                int t = order[i];
                order[i] = order[j];
                order[j] = t;
            }

    /* Layout del blob .edata: dir(40) + EAT + name_ptr + ordinals + strings. */
    const uint32_t n = (uint32_t)num_syms;
    const uint32_t off_eat = 40;
    const uint32_t off_names = off_eat + 4u * n;
    const uint32_t off_ord = off_names + 4u * n;
    const uint32_t off_str = off_ord + 2u * n;

    OBuf ed;
    memset(&ed, 0, sizeof(ed));
    /* strings (calculados aparte para conocer offsets), luego ensamblamos. */
    const char *dllname = strrchr(path, '/');
    const char *dllbk = strrchr(path, '\\');
    if (dllbk > dllname) dllname = dllbk;
    dllname = dllname ? dllname + 1 : path;

    /* Construir la tabla de strings y registrar offsets. */
    OBuf strs;
    memset(&strs, 0, sizeof(strs));
    uint32_t dllname_soff = (uint32_t)strs.len;
    ob_put(&strs, dllname, strlen(dllname) + 1);
    uint32_t *name_soff =
        (uint32_t *)malloc((size_t)num_syms * sizeof(uint32_t));
    for (int i = 0; i < num_syms; ++i) {
        name_soff[i] = (uint32_t)strs.len;
        ob_put(&strs, syms[order[i]].name, strlen(syms[order[i]].name) + 1);
    }

    /* Export directory (40 bytes). */
    uint8_t dir[40];
    memset(dir, 0, sizeof(dir));
    wr32le(dir + 12, edataRVA + off_str + dllname_soff); /* Name */
    wr32le(dir + 16, 1);                                 /* Base (ordinal) */
    wr32le(dir + 20, n);                                 /* NumberOfFunctions */
    wr32le(dir + 24, n);                                 /* NumberOfNames */
    wr32le(dir + 28, edataRVA + off_eat);   /* AddressOfFunctions */
    wr32le(dir + 32, edataRVA + off_names); /* AddressOfNames */
    wr32le(dir + 36, edataRVA + off_ord);   /* AddressOfNameOrdinals */
    ob_put(&ed, dir, 40);

    /* EAT: RVA de cada funcion (en orden ordenado -> ordinal i = i). */
    for (int i = 0; i < num_syms; ++i) {
        const AotSym *s = &syms[order[i]];
        uint32_t rva =
            pe.sectionHeaders[s->section].VirtualAddress + (uint32_t)s->offset;
        uint8_t b[4];
        wr32le(b, rva);
        ob_put(&ed, b, 4);
    }
    /* Name pointer table (RVA de cada nombre, ordenado). */
    for (int i = 0; i < num_syms; ++i) {
        uint8_t b[4];
        wr32le(b, edataRVA + off_str + name_soff[i]);
        ob_put(&ed, b, 4);
    }
    /* Ordinal table (u16): ordinal[i] = i (EAT en mismo orden). */
    for (int i = 0; i < num_syms; ++i) {
        uint8_t b[2];
        b[0] = (uint8_t)i;
        b[1] = (uint8_t)(i >> 8);
        ob_put(&ed, b, 2);
    }
    /* Strings. */
    ob_put(&ed, strs.p, strs.len);

    addSection(&pe, ".edata",
               ___IMAGE_SCN_CNT_INITIALIZED_DATA | ___IMAGE_SCN_MEM_READ,
               (_BYTE *)ed.p, (_DWORD)ed.len);
    pe.ntHeaders.OptionalHeader.DataDirectory[___IMAGE_DIRECTORY_ENTRY_EXPORT]
        .VirtualAddress = edataRVA;
    pe.ntHeaders.OptionalHeader.DataDirectory[___IMAGE_DIRECTORY_ENTRY_EXPORT]
        .Size = (_DWORD)ed.len;

    /* Marcar como DLL + base fija (sin .reloc, codigo PIC carga en ImageBase).
     */
    pe.ntHeaders.FileHeader.Characteristics |= ___IMAGE_FILE_DLL;
    pe.ntHeaders.OptionalHeader.DllCharacteristics &=
        (uint16_t)~___IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE;

    finalizePE64File(&pe);
    pe.ntHeaders.OptionalHeader.AddressOfEntryPoint = 0; /* sin DllMain */
    writePE64File(&pe, path);
    freePE64File(&pe);

    free(order);
    free(name_soff);
    free(strs.p);
    free(ed.p);
    return 1;
}

/* =========================================================================
 *  Flat binary (.bin) -- sin cabecera, bytes crudos de las secciones.
 * ========================================================================= */
int aot_emit_flat_bin(const char *path, uint64_t base, const AotSection *secs,
                      int num_secs, const AotReloc *relocs, int num_relocs,
                      char *err, size_t err_cap) {
    if (num_secs <= 0) {
        set_err(err, err_cap, "aot_emit_flat_bin: sin secciones");
        return 0;
    }

    /* Offset en la imagen de cada seccion (tras la colocacion). */
    uint64_t *sec_off = (uint64_t *)calloc((size_t)num_secs, sizeof(uint64_t));
    int *flow = (int *)malloc((size_t)num_secs * sizeof(int));
    int *fixd = (int *)malloc((size_t)num_secs * sizeof(int));
    if (!sec_off || !flow || !fixd) {
        set_err(err, err_cap, "aot_emit_flat_bin: OOM");
        free(sec_off);
        free(flow);
        free(fixd);
        return 0;
    }
    OBuf out;
    out.p = NULL;
    out.len = 0;
    out.cap = 0;

    /* Particionar: FLUYENTES (at<0, se colocan secuencialmente por @order) y
     * FIJAS (at>=0, ancladas a su offset).  El offset 0 = punto de entrada. */
    int nflow = 0, nfixd = 0;
    for (int i = 0; i < num_secs; ++i) {
        if (secs[i].at >= 0)
            fixd[nfixd++] = i;
        else
            flow[nflow++] = i;
    }
    /* Orden estable de las FLUYENTES por (order asc, indice asc) -- insertion
     * sort (N de secciones es pequeno). */
    for (int a = 1; a < nflow; ++a) {
        int key = flow[a];
        int b = a - 1;
        while (b >= 0 && secs[flow[b]].order > secs[key].order) {
            flow[b + 1] = flow[b];
            --b;
        }
        flow[b + 1] = key;
    }
    /* FIJAS por @at ascendente. */
    for (int a = 1; a < nfixd; ++a) {
        int key = fixd[a];
        int b = a - 1;
        while (b >= 0 && secs[fixd[b]].at > secs[key].at) {
            fixd[b + 1] = fixd[b];
            --b;
        }
        fixd[b + 1] = key;
    }

    /* Pre-chequeo: dos secciones FIJAS (@at) cuyos rangos se solapan son una
     * contradiccion que el emisor NO puede resolver (el usuario las anclo a
     * offsets incompatibles).  Lo detectamos ANTES del merge y damos un error
     * PRECISO -- nombres + offsets + cuantos bytes solapan -- para que re-
     * espaciar sea trivial en vez de adivinar. */
    for (int a = 0; a + 1 < nfixd; ++a) {
        const AotSection *cur = &secs[fixd[a]];
        const AotSection *nxt = &secs[fixd[a + 1]];
        uint64_t cur_at = (uint64_t)cur->at;
        uint64_t cur_end = cur_at + aot_sec_size(cur);
        uint64_t nxt_at = (uint64_t)nxt->at;
        if (cur_end > nxt_at) {
            char b[256];
            snprintf(b, sizeof(b),
                     "aot_emit_flat_bin: solape @at -- la seccion '%s' "
                     "(@0x%llX..0x%llX) se solapa con '%s' (@0x%llX) por 0x%llX "
                     "bytes; sube su @at a 0x%llX o reduce '%s'",
                     cur->name ? cur->name : "?",
                     (unsigned long long)cur_at, (unsigned long long)cur_end,
                     nxt->name ? nxt->name : "?", (unsigned long long)nxt_at,
                     (unsigned long long)(cur_end - nxt_at),
                     (unsigned long long)cur_end, cur->name ? cur->name : "?");
            set_err(err, err_cap, b);
            goto fail;
        }
    }

    /* Merge greedy: coloca FLUYENTES secuencialmente hasta que la siguiente no
     * cabe antes del proximo ancla FIJO; entonces rellena con ceros hasta el
     * ancla y coloca la seccion FIJA.  Las secciones FLUYENTES grandes (p.ej.
     * la .text del kernel) que no caben en ningun hueco se difieren tras el
     * ultimo ancla FIJO automaticamente (nunca se incrustan entre anclas, asi
     * no colisionan con el layout @at por mucho que crezcan). */
    {
        uint64_t cursor = 0;
        int qi = 0, fi = 0;
        const char *prev_name = "(inicio)"; /* quien avanzo el cursor */
        while (qi < nflow || fi < nfixd) {
            int take_fixed = 0;
            uint64_t aligned = cursor;
            if (qi < nflow) {
                size_t al =
                    secs[flow[qi]].align ? (size_t)secs[flow[qi]].align : 1u;
                while (aligned % al)
                    ++aligned;
            }
            if (fi < nfixd) {
                uint64_t fa = (uint64_t)secs[fixd[fi]].at;
                if (qi >= nflow)
                    take_fixed = 1;
                else if (aligned + aot_sec_size(&secs[flow[qi]]) > fa)
                    take_fixed = 1;
            }
            if (take_fixed) {
                uint64_t fa = (uint64_t)secs[fixd[fi]].at;
                if (cursor > fa) {
                    /* Una FLUYENTE colocada antes empujo el cursor sobre este
                     * ancla.  Con el diferido esto no deberia ocurrir, pero si
                     * pasa, reportamos quien y por cuanto. */
                    char b[256];
                    snprintf(b, sizeof(b),
                             "aot_emit_flat_bin: solape -- '%s' (termina en "
                             "0x%llX) invade la seccion @at '%s' (@0x%llX) por "
                             "0x%llX bytes",
                             prev_name, (unsigned long long)cursor,
                             secs[fixd[fi]].name ? secs[fixd[fi]].name : "?",
                             (unsigned long long)fa,
                             (unsigned long long)(cursor - fa));
                    set_err(err, err_cap, b);
                    goto fail;
                }
                sec_off[fixd[fi]] = fa;
                cursor = fa + aot_sec_size(&secs[fixd[fi]]);
                prev_name = secs[fixd[fi]].name ? secs[fixd[fi]].name : "?";
                ++fi;
            } else {
                sec_off[flow[qi]] = aligned;
                cursor = aligned + aot_sec_size(&secs[flow[qi]]);
                prev_name = secs[flow[qi]].name ? secs[flow[qi]].name : "?";
                ++qi;
            }
        }
        /* Construir la imagen (ceros) del tamano total y volcar cada seccion.
         */
        if (!ob_put(&out, NULL, (size_t)cursor)) {
            set_err(err, err_cap, "aot_emit_flat_bin: OOM imagen");
            goto fail;
        }
        for (int i = 0; i < num_secs; ++i) {
            if (secs[i].flags & AOT_SEC_BSS) continue; /* ya es cero */
            uint64_t sz = aot_sec_size(&secs[i]);
            if (sz && secs[i].data)
                memcpy(out.p + sec_off[i], secs[i].data, (size_t)sz);
        }
    }

    /* Resolver relocs contra la base fija (REL32 es invariante a la base). */
    for (int r = 0; r < num_relocs; ++r) {
        const AotReloc *rl = &relocs[r];
        if (rl->site_section < 0 || rl->site_section >= num_secs ||
            rl->target_section < 0 || rl->target_section >= num_secs) {
            set_err(err, err_cap,
                    "aot_emit_flat_bin: indice de seccion invalido en reloc");
            goto fail;
        }
        uint64_t site_image_off = sec_off[rl->site_section] + rl->site_off;
        uint64_t target_value;
        if (rl->target_is_size) {
            target_value = aot_sec_size(&secs[rl->target_section]);
        } else if (rl->target_is_end) {
            target_value = base + sec_off[rl->target_section] +
                           aot_sec_size(&secs[rl->target_section]);
        } else {
            target_value = base + sec_off[rl->target_section] + rl->target_off;
        }
        target_value += (uint64_t)rl->addend;
        uint32_t width =
            (rl->kind == AOT_RELOC_ABS64 || rl->kind == AOT_RELOC_IMM64) ? 8u
                                                                         : 4u;
        if (site_image_off + width > out.len) {
            set_err(err, err_cap, "aot_emit_flat_bin: reloc fuera de rango");
            goto fail;
        }
        if (!apply_reloc(out.p + site_image_off, base + site_image_off,
                         target_value, rl->kind)) {
            set_err(err, err_cap,
                    "aot_emit_flat_bin: kind de reloc no soportado");
            goto fail;
        }
    }

    {
        FILE *f = fopen(path, "wb");
        if (!f) {
            set_err(err, err_cap, "aot_emit_flat_bin: fopen fallo");
            goto fail;
        }
        size_t w = out.len ? fwrite(out.p, 1, out.len, f) : 0;
        fclose(f);
        if (w != out.len) {
            set_err(err, err_cap, "aot_emit_flat_bin: fwrite incompleto");
            goto fail;
        }
    }

    free(sec_off);
    free(flow);
    free(fixd);
    free(out.p);
    return 1;

fail:
    free(sec_off);
    free(flow);
    free(fixd);
    free(out.p);
    return 0;
}

/* --- Lectura de exports de DLL/PE (delega en LibPEparse) ----------------- */
int aot_pe_export_names(const char *path, char ***out_names, int *out_count) {
    if (out_names) *out_names = NULL;
    if (out_count) *out_count = 0;
    if (path == NULL || out_names == NULL || out_count == NULL) return 1;
    FILE *f = fopen(path, "rb");
    if (f == NULL) return 1;
    /* Parse MINIMO suficiente para los exports: DOS header (e_lfanew) + NT
     * headers (RVA del directorio de exports) + section headers (necesarios
     * para resolve64 RVA->offset).  Se EVITA el ParseFile64 completo, que
     * ademas parsea imports / base-relocs / rich-header -- innecesarios para
     * exports y costosos/fragiles en DLLs del sistema grandes (kernel32). */
    PE64FILE pe;
    PE64FILE_Initialize(&pe);
    pe.NAME = (char *)path;
    pe.Ppefile = f;
    ParseDOSHeader64(&pe);
    ParseNTHeaders64(&pe);
    ParseSectionHeaders64(&pe);
    int n = 0;
    char **names = ParseExportNames64(&pe, &n);
    if (pe.PEFILE_SECTION_HEADERS != NULL) free(pe.PEFILE_SECTION_HEADERS);
    fclose(f);
    *out_names = names;
    *out_count = n;
    return 0; /* 0 exports tambien es exito */
}

void aot_free_pe_export_names(char **names, int count) {
    FreeExportNames64(names, count);
}

/* --- Lectura de exports de una .so/ELF (dynsym, delega en LibELFparse) ---- */
int aot_elf_export_names(const char *path, char ***out_names, int *out_count) {
    if (out_names) *out_names = NULL;
    if (out_count) *out_count = 0;
    if (path == NULL || out_names == NULL || out_count == NULL) return 1;
    FILE *f = fopen(path, "rb");
    if (f == NULL) return 1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return 1;
    }
    void *buf = malloc((size_t)sz);
    if (buf == NULL) {
        fclose(f);
        return 1;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return 1;
    }
    fclose(f);
    ElfFile elf;
    if (!elf_mem_parse(&elf, buf, (size_t)sz)) {
        free(buf);
        return 1;
    }
    int n = 0;
    /* elf_dynsym_export_names COPIA cada nombre -> el buffer se puede liberar. */
    char **names = elf_dynsym_export_names(&elf, &n);
    free(buf);
    *out_names = names;
    *out_count = n;
    return 0;
}
