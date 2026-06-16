# Ejemplos AOT (Phase AOT.3 Paso 2b-ii)

Programas Vex que compilan a ejecutable nativo standalone (sin runtime VM)
con CALL intra-modulo + tail-call con TCO genuino.

Compilar (PE para Windows, ELF para Linux):

    vm --vex examples_codes_vex/aot/01_call.vex -m aot -o 01_call.exe        # PE (default host)
    vm --vex examples_codes_vex/aot/01_call.vex -m aot --format elf -o 01_call.elf

Ejecutar -> el `return` de `main` es el codigo de salida del proceso:

| Ejemplo                 | exit-code esperado |
|:------------------------|-------------------:|
| 01_call.vex             | 42                 |
| 02_call_chain.vex       | 40 (main->inc->triple) |
| 03_recursion.vex        | 120 (fact(5), recursion no-tail) |
| 04_tail_call_tco.vex    | 64 (loop(5000000,0) mod 256; TCO O(1) pila, no desborda) |

El codegen va por el path vreg en ABI HOST_LEAF (args en arg_regs, retorno en
RAX, sin ProcessVM*); las CALL cross-funcion y los tail-call se resuelven con
relocations rel32 parcheadas tras el layout de `.text`.

## Referencias a datos (.rodata) -- Paso 2b/2a

| Ejemplo            | exit-code | nota |
|:-------------------|----------:|:-----|
| 05_rodata.vex      | 67 ('C')  | `char* m="ABC"; m[2]` -> literal en `.rodata` |
| 06_call_rodata.vex | 69 ('E')  | CALL + dato `.rodata` cruzando la llamada |

Por defecto las refs a datos son **RIP-relativas** (position-independent, listo
para PIE/.so). Con `--no-pie` se emiten **absolutas** (`mov reg,imm64`, requieren
base de imagen fija; el emisor PE limpia DYNAMIC_BASE para fijarla), analogo a
`gcc/clang -no-pie`.

## Secciones definidas por el usuario (dev OS) -- Paso 2b

`@section(".name")` (datos o codigo) coloca la funcion/dato en una seccion
propia; permisos por convencion del nombre (`.text*`->rx, `.rodata*`->r,
`.data*`/`.bss*`->rw) o explicitos `@section(".boot","rwx")`.  Las llamadas
cross-seccion las resuelve el ObjectWriter (relocs rel32).  Una funcion con
`@section` NO se inlinea (permanece fisica en su seccion).

| Ejemplo               | exit-code | nota |
|:----------------------|----------:|:-----|
| 07_section_devos.vex  | 42        | `boot_entry` en `.boot` (RWX), main en `.text`, call cross-seccion |

## Simbolos de seccion (dev OS) -- Paso 2c

`section_start(".x") -> void*`, `section_end(".x") -> void*`,
`section_size(".x") -> u64` -- estilo `__start_NAME`/`__stop_NAME` del linker.
El writer AOT los resuelve tras el layout (relocs ADDR/END/SIZE).  En `-m vm`/`-m jit`
(sin secciones nativas) devuelven 0.

| Ejemplo               | exit-code | nota |
|:----------------------|----------:|:-----|
| 08_section_symbols.vex | size de .boot | valida `end-start == size` (28 con este codegen) |

## Objeto relocatable .o (AOT.4-ext) -- linkable con toolchains externos

`--emit obj --format elf` produce un ELF ET_REL (.o) en vez de un ejecutable:
SIN `_start`, con `main` como simbolo GLOBAL y las relocs como registros
(`.rela.text`: R_X86_64_PC32 / R_X86_64_64).  Se linka con `ld`/`gcc`/`clang`,
que aportan el crt (`_start` -> `main`) + libc + permiten linker scripts (dev OS):

    vm --vex examples_codes_vex/aot/01_call.vex -m aot --format elf --emit obj -o 01_call.o
    gcc 01_call.o -o prog        # el crt de gcc llama a main
    ./prog; echo $?              # 42

Validado: 01_call.o (call), 06_call_rodata.o (data reloc -> .rodata),
07_section_devos.o (.text + .boot cross-section) linkan con gcc y devuelven
42/69/42.  COFF (.obj para link.exe) pendiente.

## Objeto COFF .obj (Windows) -- linkable con link.exe / gcc-mingw

`--emit obj --format pe` produce un COFF .obj (Machine AMD64): SIN _start, `main`
como simbolo EXTERNAL, relocs COFF (IMAGE_REL_AMD64_REL32 / ADDR64) contra el
simbolo de seccion del target.  COFF lleva el addend EN el campo (no en un record
aparte), asi que el emisor pre-escribe target_off en el sitio.

    vm --vex examples_codes_vex/aot/01_call.vex -m aot --format pe --emit obj -o 01_call.obj
    gcc 01_call.obj -o prog.exe        # gcc-mingw aporta el crt -> main
    ./prog.exe; echo $?                # 42

Validado: 01_call.obj / 06_call_rodata.obj / 07_section_devos.obj linkan con
gcc-mingw (TDM-GCC) y devuelven 42/69/42.  (Reusa la API de LibCOFFparse.)

## Libreria compartida .so (ELF) -- dlopen/dlsym

`--emit shared --format elf` produce un ELF ET_DYN (.so) PIC que EXPORTA todas
sus funciones (sin main; sin _start).  Cargable con dlopen + dlsym:

    vm --vex examples_codes_vex/aot/09_shared_lib.vex -m aot --format elf --emit shared -o libfoo.so
    // host C: dlopen("./libfoo.so") + dlsym("add") -> add(40,2) == 42

Validado: libfoo.so (add/triple) cargado via dlopen+dlsym en Linux -> 42/42.
(.dll PE shared pendiente.)

## Cross-target: el ABI lo decide el FORMATO, no el host

El codegen usa el ABI del TARGET (SysV para `--format elf`, Win64 para
`--format pe`), no el del host -> se puede generar ELF en Windows y PE en Linux.
Validado: ELF generado en Windows corre en WSL Linux (01/03/06 = 42/120/69).

## Libreria compartida .dll (PE Windows) -- LoadLibrary/GetProcAddress

`--emit shared --format pe` produce una DLL PE32+ (IMAGE_FILE_DLL + tabla de
exports .edata).  Cargable con LoadLibrary + GetProcAddress:

    vm --vex examples_codes_vex/aot/10_shared_dll.vex -m aot --format pe --emit shared -o foo.dll
    // host C: LoadLibraryA("foo.dll") + GetProcAddress("add") -> add(40,2) == 42

Validado: foo.dll (add/triple) cargada via LoadLibrary+GetProcAddress -> 42/42.
Codigo PIC (RIP-rel); base fija (DYNAMIC_BASE limpiado, sin .reloc).
