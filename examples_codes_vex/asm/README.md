# Ejemplos de inline-asm (Phase AS)

Estos `.vex` usan **inline-asm nativo** (`asm { ... }` en sintaxis NASM Intel +
storage-class `register("reg")`).  Se compilan y ejecutan **sin flags**:

```sh
vm --vex 01_popcount.vex -o popcount
vm --run popcount.velb
```

- **Sin `clobbers(...)`**: el compilador INFIERE los registros/flags que el asm
  pisa (analizando el body) y los preserva automaticamente -- incluidos los
  callee-saved (`mov r12, ...`) y los reservados por el runtime (`rbx`/`rbp`,
  que `cpuid` pisa: se salvan/restauran alrededor del bloque).  Declararlos
  explicitamente sigue siendo posible, pero opcional.
- **Metaprogramacion**: las `comptime` consts se SUSTITUYEN por su literal en el
  cuerpo del asm antes de ensamblar (ver `09_comptime_mask`).
- **Ejecucion**: el body del asm corre como codigo nativo host; el backend lo
  materializa via el JIT (la funcion con inline-asm se compila a un wrapper
  nativo).  Funciona en el modo por defecto y con `-m jit`.

| Ejemplo | Demuestra | Resultado |
|:--------|:----------|:----------|
| `01_popcount` | intrinseco `popcnt` (input rdi + output rax) | 8 |
| `02_byteswap` | `bswap` con binding INOUT (rax in+out) | 0x8877665544332211 |
| `03_clobber_callee_saved` | clobbers callee-saved (r12/r13) INFERIDOS, valor vivo a traves | 50 |
| `04_rdtsc` | intrinseco de sistema `rdtsc` (edx:eax) | no determinista |
| `05_cpuid_maxleaf` | `cpuid` (pisa rbx=ProcessVM*, salvado/restaurado) | CPU-specific |
| `06_sse2_paddd` | SIMD SSE2: 4 sumas i32 en paralelo (`paddd`) sobre memoria | 110 |
| `07_avx2_paddd` | SIMD AVX2: 8 sumas i32 en paralelo (`vpaddd`, ymm) | 396 |
| `08_bmi_pext` | BMI2 `pext` (parallel bits extract) | 3 |
| `09_comptime_mask` | `comptime` const sustituida en el asm (metaprogramacion) | 0x340078 |

**Patron SIMD recomendado:** los datos viven en memoria host (`malloc`) y los
`register()` ligan PUNTEROS (GP); el registro vectorial (xmm/ymm) es scratch
interno del asm.  Asi se evita ligar registros vectoriales directamente.

## Limitaciones conocidas

- **Bindings de registros vectoriales** (`register("xmm0") ...`): NO soportados
  todavia (el register allocator del JIT solo asigna el banco GP en v1).  Para
  SIMD, usar el patron de memoria de arriba (`06`/`07`).  Soporte de banco
  XMM/YMM = trabajo futuro (FP regalloc).
- **Interp puro sin JIT (`-m vm`)**: hoy la funcion con inline-asm para de forma
  ruidosa (`hlt`).  El plan es un fallback con **ensamblador externo** (la
  toolchain del sistema) cuando el JIT no este disponible.
- **Clobber de `rsp`**: un asm que pisa el stack pointer no es envolvible
  (las push/pop del wrapper usan rsp) -> se rechaza.

Tambien se puede portar cualquiera de estos `.vex` a C con `--port c` (GCC
ensambla el asm); ver `tests/vex/test_vex_e2e.sh` (Phase AS) para la paridad.
