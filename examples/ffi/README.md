# libvesta -- API C embebible de VestaVM

`libvesta` es una libreria compartida (`vesta.dll` en Windows, `libvesta.so`
en POSIX) con una interfaz C estable (C-ABI) que permite compilar y ejecutar
codigo Vex desde cualquier lenguaje con FFI: C, Python, Rust, C#, etc.

La cabecera publica es `include/capi/vesta.h`.

## Construir la libreria

La libreria se construye junto al resto del proyecto.  El target CMake se
llama `vesta_ffi` y esta activado por defecto (opcion `VESTA_BUILD_FFI=ON`).

**Auto-build:** construir el ejecutable `vm` arrastra `libvesta` de forma
automatica (hay un `add_dependencies(vm vesta_ffi)`), asi que basta con:

```bash
cmake -S . -B cmake-build-release
cmake --build cmake-build-release --target vm -j4
```

y obtienes `vm.exe` **y** `libvesta.dll`/`.so` en el mismo directorio.  Si
solo quieres la libreria puedes pedir su target directamente:

```bash
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

/* --- Compilar / ejecutar --- */
int vesta_compile(const char *src, const char *unit_name,
                  unsigned char **out_velb, size_t *out_len, char **out_err);

int vesta_run(const unsigned char *velb, size_t len, int argc,
              const char *const *argv, int *out_exit, char **out_err);

int vesta_eval(const char *src, const char *unit_name,
               int *out_exit, char **out_err);

/* --- Artefactos del pipeline --- */
int vesta_compile_to_vel(const char *src, const char *unit_name,
                         char **out_vel, char **out_err);

int vesta_compile_to_ir(const char *src, const char *unit_name,
                        char **out_ir, char **out_err);

int vesta_assemble(const char *vel_text, unsigned char **out_velb,
                   size_t *out_len, char **out_err);

int vesta_disasm(const unsigned char *bytes, size_t len, const char *arch,
                 char **out_text, char **out_err);

int vesta_diagram(const char *src, const char *unit_name, const char *kind,
                  const char *format, char **out_text, char **out_err);

/* --- Conveniencia: vel + ir + velb en una compilacion --- */
int vesta_compile_full(const char *src, const char *unit_name,
                       char **out_vel, char **out_ir,
                       unsigned char **out_velb, size_t *out_velb_len,
                       char **out_err);

/* --- VestaShellScript --- */
int vesta_vsh_eval(const char *script, int *out_rc, char **out_err);

/* --- IR -> bytecode (cierra el ciclo de manipulacion del IR) --- */
int vesta_ir_to_velb(const char *ir_text, unsigned char **out_velb,
                     size_t *out_len, char **out_err);

/* --- JSON (nlohmann) --- */
int vesta_json_validate(const char *json_text, char **out_err);
int vesta_json_format(const char *json_text, int indent,
                      char **out_text, char **out_err);

/* --- SQLite --- */
int vesta_sqlite_exec(const char *db_path, const char *sql,
                      char **out_json, char **out_err);

void vesta_free(void *p);
```

| Funcion | Que hace |
| :------ | :------- |
| `vesta_version` | Cadena de version (estatica, NO liberar). |
| `vesta_compile` | Fuente Vex -> bytes `.velb` (con seccion `@ir` v3). |
| `vesta_run` | Ejecuta bytes `.velb`; `out_exit` = R0 de `main`. |
| `vesta_eval` | Compila + ejecuta en una llamada. |
| `vesta_compile_to_vel` | Fuente Vex -> texto `.vel` (bytecode textual). |
| `vesta_compile_to_ir` | Fuente Vex -> texto del IR SSA. |
| `vesta_assemble` | Texto `.vel` -> bytes `.velb`. |
| `vesta_disasm` | Buffer de bytes nativos -> listado (Capstone); `arch` = `"X86-32"`/`"X86-64"`/`"ARM"`/`"AArch64"` (NULL => `"X86-64"`). |
| `vesta_diagram` | Fuente Vex -> diagrama; `kind` = `"ast"`/`"ir-pre"`/`"ir-post"`/`"vel"`, `format` = `"mermaid"`/`"graphviz"`/`"html"`. |
| `vesta_compile_full` | Devuelve a la vez `.vel`, IR y `.velb` (cada salida opcional con NULL). |
| `vesta_vsh_eval` | Ejecuta un script VestaShellScript desde una cadena. |
| `vesta_ir_to_velb` | Texto IR SSA -> bytes `.velb`. Cierra el ciclo `vesta_compile_to_ir` -> (editar IR) -> `vesta_ir_to_velb` -> `vesta_run`. |
| `vesta_json_validate` | Valida una cadena JSON (nlohmann); `0` = valido, mensaje de parse en `out_err`. |
| `vesta_json_format` | Reformatea JSON: `indent < 0` => compacto/minificado, `indent >= 0` => pretty con esa sangria. |
| `vesta_sqlite_exec` | Abre una BD SQLite (acepta `":memory:"`), ejecuta el SQL y devuelve las filas de los `SELECT` como array JSON `[{columna: valor}, ...]` (`"[]"` si no hay filas). |

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

## Funciones P1 (IR -> velb, JSON, SQLite)

`c/test_ffi_p1.c` (y `python/test_ffi_p1.py`) cubren las cuatro funciones
P1.  Snippets:

```c
/* 1. Cerrar el ciclo: fuente -> IR -> (editar) -> velb -> run. */
char *ir = NULL, *err = NULL;
vesta_compile_to_ir("i32 main() { return 7; }", "u", &ir, &err);
unsigned char *velb = NULL; size_t len = 0;
vesta_ir_to_velb(ir, &velb, &len, &err);     /* IR text -> .velb */
int code = 0;
vesta_run(velb, len, 0, NULL, &code, &err);  /* code == 7 */
vesta_free(ir); vesta_free(velb);

/* 2. Validar JSON. */
if (vesta_json_validate("{\"a\":1}", &err) == 0) { /* valido */ }

/* 3. Minificar / embellecer. */
char *mini = NULL;   /* indent < 0 => compacto */
vesta_json_format("{ \"x\" : 1 }", -1, &mini, &err);   /* {"x":1} */
char *pretty = NULL; /* indent >= 0 => sangria */
vesta_json_format("{\"x\":1}", 2, &pretty, &err);
vesta_free(mini); vesta_free(pretty);

/* 4. SQLite -> JSON de filas. */
char *rows = NULL;
vesta_sqlite_exec(":memory:",
    "CREATE TABLE t(id INTEGER, name TEXT);"
    "INSERT INTO t VALUES(1,'a'),(2,'b');"
    "SELECT * FROM t;", &rows, &err);
/* rows == "[{\"id\":1,\"name\":\"a\"},{\"id\":2,\"name\":\"b\"}]" */
vesta_free(rows);
```

Compilar y ejecutar:

```bash
cd examples/ffi/c
gcc test_ffi_p1.c -I../../../include -L../../../cmake-build-release -lvesta -o test_ffi_p1.exe
PATH="../../../cmake-build-release:$PATH" ./test_ffi_p1.exe

# Python (mismo cobertura):
cd ../python
PATH="../../../cmake-build-release:$PATH" python test_ffi_p1.py
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

El script Python cubre `vesta_eval`, `vesta_compile_to_ir`, `vesta_diagram`
(Mermaid) y `vesta_vsh_eval`.

## Ejemplo en C++

`cpp/test_ffi.cpp` usa RAII (`unique_ptr` con deleter = `vesta_free`) y cubre
`vesta_compile_full` (vel + ir + velb en una compilacion), `vesta_run`,
`vesta_disasm` y `vesta_diagram` (Graphviz).

```bash
cd examples/ffi/cpp
g++ -std=c++17 test_ffi.cpp -I../../../include -L../../../cmake-build-release \
    -lvesta -o test_ffi.exe
PATH="../../../cmake-build-release:$PATH" ./test_ffi.exe
```

## Ejemplo en Vex (self-hosting)

`vx/self_host.vx` es un programa Vex que, via el FFI runtime dinamico
(`ffi_open` / `ffi_sym` / `ffi_call`), carga `libvesta` y llama a
`vesta_eval` para **compilar y ejecutar otro snippet Vex desde dentro de
Vex**.  Es decir: el lenguaje se ejecuta a si mismo a traves de su propia
DLL.

```vx
i32 main() {
    string snippet = "i32 main() { return 6 * 7; }";
    i32* slot = malloc(4); slot[0] = 0;

    i64 lib  = ffi_open("libvesta.dll");          // o "libvesta.so" en POSIX
    i64 eval = ffi_sym(lib, "vesta_eval");
    // vesta_eval(src, NULL, &out_exit, NULL)
    ffi_call(eval, str_cstr(snippet), 0, slot, 0);

    i32 inner = slot[0]; free(slot);
    return inner;                                  // 42
}
```

Compilar y ejecutar (con `libvesta.dll` junto al `.exe` o en el `PATH`):

```bash
vm --vx examples/ffi/vx/self_host.vx -o self_host
vm --run self_host.velb --stats          # imprime R00=0x2a (42)
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
- El preprocesador VPP no se aplica a la fuente (se compila directamente con
  el frontend Vex).
- `vesta_compile` / `vesta_assemble` / `vesta_compile_full` usan ficheros
  temporales para el paso de ensamblado/linkado interno; se borran solos.
- `vesta_diagram` con `format="html"` produce una pagina HTML autocontenida
  (CSS+JS embebidos) lista para abrir en el navegador.
- `vesta_vsh_eval` ejecuta el interprete tree-walking de VestaShellScript;
  la salida del script va al `stdout`/`stderr` del host.
