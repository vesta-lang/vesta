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
 * @file aot/aot_emit_shim.h
 * @brief Phase AOT.4 -- ABI C plana para emitir ejecutables PE/ELF.
 *
 * Frontera limpia entre el compilador AOT (C++) y LibPEparse (C).  Este header
 * NO expone ningun tipo de LibPEparse: solo @c structs C planos y funciones.
 * El .c que lo implementa (@c aot_emit_shim.c) se compila como C -- donde las
 * cabeceras de LibPEparse (con uniones anonimas, macros ELF, etc.) compilan sin
 * problemas -- y el lado C++ incluye SOLO este header.  Asi:
 *
 *   - LibPEparse permanece como libreria C generica, sin cambios por VestaVM.
 *   - El C++ no sufre las incompatibilidades C/C++ de las cabeceras C.
 *   - La API es CONFIGURABLE: el usuario define sus propias secciones, tamanos,
 *     alineamientos, base de imagen, entradas de import, pila, subsistema, etc.
 */

#ifndef AOT_AOT_EMIT_SHIM_H
#define AOT_AOT_EMIT_SHIM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 *  Flags genericos de seccion (independientes del formato).
 * ------------------------------------------------------------------------- */
#define AOT_SEC_READ   0x01u  /* legible */
#define AOT_SEC_WRITE  0x02u  /* escribible */
#define AOT_SEC_EXEC   0x04u  /* ejecutable */
#define AOT_SEC_CODE   0x08u  /* contiene codigo */
#define AOT_SEC_DATA   0x10u  /* datos inicializados */
#define AOT_SEC_BSS    0x20u  /* datos sin inicializar (no ocupan fichero) */

/**
 * @brief Una seccion definida por el usuario.
 *
 * @c vaddr / @c align en 0 => el emisor elige (layout secuencial por pagina).
 * Para BSS, @c data=NULL/@c size=0 y @c bss_size>0.
 */
typedef struct {
    const char    *name;       /* nombre (".text", ".rodata", o propio del usuario) */
    uint32_t       flags;      /* AOT_SEC_* */
    const uint8_t *data;       /* bytes (NULL para BSS) */
    uint32_t       size;       /* tamano de @c data */
    uint32_t       bss_size;   /* tamano BSS si AOT_SEC_BSS */
    uint64_t       vaddr;      /* VA fija (0 = auto) */
    uint64_t       align;      /* alineamiento (0 = default del formato) */
} AotSection;

/**
 * @brief Una llamada a funcion importada a resolver (solo PE).
 *
 * El emisor agrupa por DLL, construye la @c .idata + IAT y parchea el @c disp32
 * del @c call indirecto (@c FF 15) en @c call_off de la seccion @c call_section.
 */
typedef struct {
    const char *dll;           /* "KERNEL32.dll" */
    const char *func;          /* "ExitProcess" */
    int         call_section;  /* indice de seccion donde esta el call */
    uint64_t    call_off;      /* offset del FF 15 dentro de esa seccion */
} AotImport;

/**
 * @brief Configuracion de layout / linker.  Campos en 0 => default del formato.
 *
 * Permite al usuario controlar base de imagen, alineamientos, pila/heap,
 * subsistema (PE) y direccion de pila (ELF) sin tocar el emisor.
 */
typedef struct {
    uint64_t image_base;        /* base de carga (PE/ELF). 0 => 0x400000 */
    uint32_t section_align;     /* alineamiento de seccion en memoria. 0 => default */
    uint32_t file_align;        /* alineamiento en fichero (PE). 0 => 0x200 */
    uint64_t stack_reserve;     /* PE SizeOfStackReserve. 0 => 0x100000 */
    uint64_t stack_commit;      /* PE SizeOfStackCommit.  0 => 0x1000 */
    uint64_t heap_reserve;      /* PE SizeOfHeapReserve.  0 => 0x100000 */
    uint64_t heap_commit;       /* PE SizeOfHeapCommit.   0 => 0x1000 */
    int      pe_subsystem;      /* PE Subsystem. <=0 => CUI (consola) */
    uint64_t elf_stack_vaddr;   /* ELF: VA sugerida de la pila. 0 => 0x70000000 */
    uint64_t elf_stack_size;    /* ELF: tamano del segmento de pila. 0 => 0x10000 */
} AotLayoutCfg;

/**
 * @brief Emite un ejecutable PE32+ (Windows) a disco.
 *
 * @param path        ruta del fichero de salida.
 * @param cfg         configuracion de layout (NULL => defaults).
 * @param secs        array de secciones (la entrada @c entry_section debe ser codigo).
 * @param num_secs    numero de secciones.
 * @param entry_sec   indice de la seccion del punto de entrada.
 * @param entry_off   offset dentro de esa seccion del @c _start.
 * @param imps        array de imports a resolver (NULL si num_imps=0).
 * @param num_imps    numero de imports.
 * @param err         buffer para el mensaje de error (puede ser NULL).
 * @param err_cap     capacidad de @c err.
 * @return 1 en exito, 0 en error (con @c err relleno).
 */
int aot_emit_pe(const char *path, const AotLayoutCfg *cfg,
                const AotSection *secs, int num_secs,
                int entry_sec, uint64_t entry_off,
                const AotImport *imps, int num_imps,
                char *err, size_t err_cap);

/**
 * @brief Emite un ejecutable ELF64 (Linux) freestanding a disco.
 *
 * La salida del proceso es responsabilidad del codigo (syscall @c exit_group):
 * el ELF no lleva imports.  Genera dos PT_LOAD (codigo R+X, pila R+W) + symtab
 * minima con @c _start.
 *
 * @param path        ruta del fichero de salida.
 * @param cfg         configuracion de layout (NULL => defaults).
 * @param secs        array de secciones.
 * @param num_secs    numero de secciones.
 * @param entry_sec   indice de la seccion del punto de entrada.
 * @param entry_off   offset dentro de esa seccion del @c _start.
 * @param err         buffer para el mensaje de error (puede ser NULL).
 * @param err_cap     capacidad de @c err.
 * @return 1 en exito, 0 en error.
 */
int aot_emit_elf(const char *path, const AotLayoutCfg *cfg,
                 const AotSection *secs, int num_secs,
                 int entry_sec, uint64_t entry_off,
                 char *err, size_t err_cap);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* AOT_AOT_EMIT_SHIM_H */
