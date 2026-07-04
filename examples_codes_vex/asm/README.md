# Ejemplos de inline-asm (Phase AS)

Estos `.vx` usan **inline-asm nativo** (`asm { ... }` en sintaxis NASM Intel +
storage-class `register("reg")`).  Se compilan y ejecutan **sin flags**:

```sh
vm --vex 01_popcount.vx -o popcount
vm --run popcount.velb
```

- **Sin `clobbers(...)`**: el compilador INFIERE los registros/flags que el asm
  pisa (analizando el body) y los preserva automaticamente -- incluidos los
  callee-saved (`mov r12, ...`) y los reservados por el runtime (`rbx`/`rbp`,
  que `cpuid` pisa: se salvan/restauran alrededor del bloque).  Declararlos
  explicitamente sigue siendo posible, pero opcional.
- **Metaprogramacion**: las `comptime` consts se SUSTITUYEN por su literal en el
  cuerpo del asm antes de ensamblar (ver `09_comptime_mask`).
- **Ejecucion**: el body del asm corre como codigo nativo host en **los tres
  modos** -- por defecto, `-m jit` y `-m vm` (interprete puro).  En `-m jit`/
  default el JIT compila la funcion a nativo; en `-m vm` (sin compilador JIT) el
  bloque se ensambla a un trampolin y el interprete lo invoca via un helper de
  marshalling (los `register()` se copian a/desde el trampolin).

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
  todavia (el register allocator del JIT solo asigna el banco GP en v1; el
  marshalling del interprete solo copia GP).  Para SIMD, usar el patron de
  memoria de arriba (`06`/`07`).  Soporte de banco XMM/YMM = trabajo futuro
  (FP regalloc).  Un binding vectorial cae a `hlt` (trap ruidoso) en `-m vm`.
- **Interp puro (`-m vm`) necesita un ENSAMBLADOR en runtime**: el trampolin se
  ensambla al CARGAR el `.velb` via Keystone (que va embebido en `vm`).  Es un
  *ensamblador*, no el compilador JIT, asi que el modo es portable a plataformas
  sin JIT -- pero NO a un runtime sin ningun ensamblador.  La portabilidad TOTAL
  (ensamblar en compile-time + embeber los bytes nativos en una seccion del
  `.velb`) es trabajo futuro.  Ademas, los bytes son especificos de la CPU
  (x86-64): un `.velb` con inline-asm x86 no corre en ARM.
- **Maximo 8 `register()` por bloque en `-m vm`**: el helper de marshalling pasa
  los slots como argumentos (argc 12 = 4 fijos + 8 slots).  Un bloque con >8
  bindings cae a `hlt`.  Caso patologico; el path JIT no tiene este limite.
- **Clobber de `rsp`**: un asm que pisa el stack pointer no es envolvible
  (las push/pop del trampolin/wrapper usan rsp) -> se rechaza.

Tambien se puede portar cualquiera de estos `.vx` a C con `--port c` (GCC
ensambla el asm); ver `tests/vex/test_vex_e2e.sh` (Phase AS) para la paridad.
