# Ejemplos de inline-asm (Phase AS)

Estos `.vex` usan **inline-asm nativo** (`asm { ... }` en sintaxis NASM Intel +
storage-class `register("reg")`).  Se compilan y ejecutan **sin flags**:

```sh
vm --vex 01_popcount.vex -o popcount
vm --run popcount.velb
```

- **Sin `clobbers(...)`**: el compilador INFIERE los registros/flags que el asm
  pisa (analizando el body) y los excluye/preserva automaticamente.  Declararlos
  explicitamente (`clobbers("rdx","cc")`) sigue siendo posible, pero opcional.
- **Ejecucion**: el body del asm DEBE correr como codigo nativo de la CPU host.
  El backend bytecode lo materializa via el JIT: la funcion con inline-asm se
  compila a un wrapper nativo (carga los `register()` a sus registros fisicos,
  ejecuta el asm, devuelve el resultado).  Funciona en el modo por defecto y con
  `-m jit`.

| Ejemplo | Demuestra | Resultado |
|:--------|:----------|:----------|
| `01_popcount.vex` | intrinseco `popcnt` (input rdi + output rax) | 8 |
| `02_byteswap.vex` | `bswap` con binding INOUT (rax in+out) | 0x8877665544332211 |
| `03_clobber_callee_saved.vex` | clobbers de callee-saved (r12-r15) INFERIDOS, con un valor vivo a traves del asm | 50 |
| `04_rdtsc.vex` | intrinseco de sistema `rdtsc` (edx:eax) | no determinista |

Tambien se puede portar el mismo `.vex` a C con `--port c` (GCC ensambla el asm).

> **Nota (portabilidad / sin-JIT):** en plataformas donde el JIT no este
> disponible, el plan es un fallback que use un **ensamblador externo** (la
> toolchain del sistema) para producir el codigo nativo del asm, que el
> interprete carga y llama.  Mientras tanto, ejecutar con `-m vm` (interp puro
> sin JIT) hace que la funcion con inline-asm pare de forma ruidosa (`hlt`) en
> vez de devolver un resultado incorrecto.
