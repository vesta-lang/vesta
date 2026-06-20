# libvesta -- API C embebible de VestaVM

`libvesta` es una libreria compartida (`vesta.dll` en Windows, `libvesta.so`
en POSIX) con una interfaz C estable (C-ABI) que permite compilar y ejecutar
codigo Vex desde cualquier lenguaje con FFI: C, Python, Rust, C#, etc.

La cabecera publica es `include/capi/vesta.h`.

## Construir la libreria

La libreria se construye junto al resto del proyecto.  El target CMake se
llama `vesta_ffi` y esta activado por defecto (opcion `VESTA_BUILD_FFI=ON`).

```bash
cmake -S . -B cmake-build-release
cmake --build cmake-build-release --target vesta_ffi -j4
```

Artefactos resultantes en `cmake-build-release/`:

- `libvesta.dll` (Windows) / `libvesta.so` (POSIX): la libreria.
- `libvesta.dll.a` (Windows): import library para enlazar con `-lvesta`.

En Windows con MinGW la libreria enlaza `libgcc`/`libstdc++` de forma
estatica, por lo que el consumidor **no** necesita los runtime DLLs de MinGW.
Sí necesita, junto al ejecutable o en el `PATH`, las DLL de OpenSSL que el
build copia al directorio de salida:

- `libssl-3-x64.dll`
- `libcrypto-3-x64.dll`

## API

```c
const char *vesta_version(void);

int vesta_compile(const char *src, const char *unit_name,
                  unsigned char **out_velb, size_t *out_len, char **out_err);

int vesta_run(const unsigned char *velb, size_t len, int argc,
              const char *const *argv, int *out_exit, char **out_err);

int vesta_eval(const char *src, const char *unit_name,
               int *out_exit, char **out_err);

void vesta_free(void *p);
```

Convenciones:

- Toda funcion que puede fallar devuelve `int`: `0` = exito, distinto de `0`
  = error.  Si `out_err` no es `NULL` y hay error, recibe un mensaje en heap
  que **debes** liberar con `vesta_free`.
- Los buffers devueltos (`out_velb`, `out_err`) se liberan con `vesta_free`.
- El valor de retorno del programa Vex (`return` de `main`) se entrega en
  `out_exit` (registro R0 del proceso principal).

## Ejemplo en C

`c/test_ffi.c` muestra `vesta_eval` y `vesta_compile` + `vesta_run`:

```c
#include "capi/vesta.h"
#include <stdio.h>

int main(void) {
    int exit_val = 0;
    char *err = NULL;
    if (vesta_eval("i32 main() { return 42; }", "demo", &exit_val, &err) == 0) {
        printf("resultado = %d\n", exit_val);  // 42
    } else {
        fprintf(stderr, "error: %s\n", err);
        vesta_free(err);
    }
    return 0;
}
```

Compilar y ejecutar (Windows / MinGW):

```bash
cd examples/ffi/c
gcc test_ffi.c -I../../../include -L../../../cmake-build-release -lvesta -o test_ffi.exe
# Ejecutar con las DLL en el PATH:
PATH="../../../cmake-build-release:$PATH" ./test_ffi.exe
```

POSIX:

```bash
gcc test_ffi.c -I../../../include -L../../../cmake-build-release -lvesta \
    -Wl,-rpath,../../../cmake-build-release -o test_ffi
./test_ffi
```

## Ejemplo en Python (ctypes)

`python/test_ffi.py` carga el DLL con `ctypes`, declara las firmas y evalua
snippets Vex:

```python
import ctypes
lib = ctypes.CDLL("cmake-build-release/libvesta.dll")
lib.vesta_eval.restype = ctypes.c_int
lib.vesta_eval.argtypes = [ctypes.c_char_p, ctypes.c_char_p,
                           ctypes.POINTER(ctypes.c_int),
                           ctypes.POINTER(ctypes.c_char_p)]
out = ctypes.c_int()
err = ctypes.c_char_p()
lib.vesta_eval(b"i32 main() { return 42; }", b"demo",
               ctypes.byref(out), ctypes.byref(err))
print(out.value)  # 42
```

Ejecutar:

```bash
cd examples/ffi/python
PATH="../../../cmake-build-release:$PATH" python test_ffi.py
```

## Ejemplo en Rust (esquema)

Declarar las funciones `extern "C"` y enlazar contra `vesta`:

```rust
use std::os::raw::{c_char, c_int};

#[link(name = "vesta")]
extern "C" {
    fn vesta_eval(src: *const c_char, unit: *const c_char,
                  out_exit: *mut c_int, out_err: *mut *mut c_char) -> c_int;
    fn vesta_free(p: *mut std::ffi::c_void);
}

fn main() {
    let src = std::ffi::CString::new("i32 main() { return 42; }").unwrap();
    let unit = std::ffi::CString::new("demo").unwrap();
    let mut exit_val: c_int = 0;
    let mut err: *mut c_char = std::ptr::null_mut();
    let rc = unsafe {
        vesta_eval(src.as_ptr(), unit.as_ptr(), &mut exit_val, &mut err)
    };
    assert_eq!(rc, 0);
    println!("resultado = {}", exit_val);  // 42
}
```

## Notas y limitaciones (MVP)

- La salida del programa Vex (`println`, etc.) va al `stdout`/`stderr` del
  proceso host.  No se captura por la API en esta version.
- La VM usa estado global (registries, caches): las llamadas a la API **no**
  son thread-safe entre si.  Serializa el acceso desde el lado del llamante
  si compartes la libreria entre hilos.
- El preprocesador VPP no se aplica a la fuente en `vesta_compile`/`vesta_eval`
  (la fuente se compila directamente con el frontend Vex).
- `vesta_compile` usa ficheros temporales para el paso de ensamblado/linkado
  interno; se borran automaticamente.
