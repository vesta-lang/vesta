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
