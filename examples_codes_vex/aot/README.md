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
