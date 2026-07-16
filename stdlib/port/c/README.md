# stdlib/port/c — Runtime snippets para el transpiler IR -> C

Cada archivo `.v.c` es un fragmento de codigo C que el transpiler **inyecta
en el .c generado** solo cuando el programa  usa la feature correspondiente.

## Motivacion

El port C no debe forzar dependencias de `stdio.h`/`stdlib.h`/`math.h` salvo
cuando el programa las necesita.  Para **programacion de sistemas o bootloaders**,
el usuario quiere salir limpio sin includes pesados.  Por eso:

- Los snippets se cargan **bajo demanda** segun el analisis estatico del IR.
- En `--port-freestanding`, se omiten los snippets que dependen de la libc
  hosted y el usuario provee sus propias implementaciones de `vx_throw`,
  `vx_panic`, etc.

## Layout

```
stdlib/port/c/
  README.md
  vx_macros.v.c          # SIEMPRE: VX_RESTRICT/HOT/COLD/UNUSED/NORETURN
  vx_pragma_silence.v.c  # SIEMPRE: pragmas -Wunused-* para output limpio
  vx_string.v.c          # si el programa usa strings (VxString runtime)
  vx_exception.v.c       # si el programa usa try/catch (vx_exc_frame)
  vx_io_stdio.v.c        # si el programa usa println/print (vio_*)
  vx_math_libm.v.c       # si el programa usa sqrt/sin/cos (vmath_*)
  vx_freestanding_panic.v.c  # ALTERNATIVA en --port-freestanding
```

## Convencion de cabecera por snippet

Cada `.v.c` empieza con:

```c
// @vx-snippet: <id_unico>
// @vx-requires: <feature1>, <feature2>   # otros snippets que necesita
// @vx-includes: <stdio.h>, <stdlib.h>    # headers C que debe incluir
// @vx-freestanding-skip: yes|no          # si se omite en --port-freestanding
```

El transpiler parsea estas cabeceras al cargar el snippet y resuelve
el orden de inclusion + lista de `#include`s a emitir.

## Como anadir un snippet nuevo

1. Crear `vx_<feature>.v.c` con la cabecera de arriba.
2. Anadir el `feature_id` al enum `port::CFeature` en `c_backend.h`.
3. En `module_uses_<feature>(mod)` del backend, detectar el uso.
4. En `emit_prelude`, llamar `emit_snippet("vx_<feature>")` si la
   feature se usa.

## Modo --port-freestanding

Cuando se pasa `--port-freestanding`:
- Solo se cargan snippets con `@vx-freestanding-skip: no`.
- No se emite ningun `#include <stdio.h>` etc.
- `vx_panic`, `vx_throw` quedan declarados como `extern` que el
  usuario debe proveer.  El transpiler emite un comentario `/* HOSTED:
  link vx_*_impl.c */` para guiar.

Esto permite usar el port C en bootloaders, kernels, firmware embebido
donde no hay libc disponible.
