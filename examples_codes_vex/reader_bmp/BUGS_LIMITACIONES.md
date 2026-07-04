# ReaderBMP -> Vex: bugs y limitaciones encontrados

Migracion del proyecto Java **ReaderBMP** (+ su libreria **JColorsTerm**) al
lenguaje **Vex** de VestaVM.  El objetivo del ejercicio era destapar bugs y
limitaciones; la migracion es el vehiculo.

Binario usado: `cmake-build-release/vm.exe` (sin rebuild).  Compilador NO
modificado.  Imagenes de prueba: `Ejemplo60x3.bmp` (60x3, sin padding) y
`Ejemplo3x60.bmp` (3x60, padding de 2 bytes/fila), ambas 24bpp BGR.

**Verificacion byte-exacta**: el volcado de pixeles a terminal se comparo BYTE
A BYTE entre modos y contra la referencia single-file `reader_bmp.vx` (que ya
era byte-exacta).  Ambas imagenes (con y sin padding de fila) coinciden.

---

## Estado tras la MODERNIZACION (2026-07)

La version modular fue modernizada para usar la stdlib y los builtins nuevos,
eliminando TODOS los workarounds locales:

- `bmp_io.vx` (lectura binaria via kernel32 local) -> **eliminado**.  Ahora
  se usa la stdlib: `import "vex_fileio"` con `file_size(path)` +
  `file_read_into(path, buf, size)` (Windows kernel32 / POSIX libc, libc-free).
- `colors.vx` (truecolor ANSI local via `chr(27)`) -> **eliminado**.  Ahora
  se usan los **builtins del lenguaje** `bg_rgb(r,g,b)` / `fg_rgb(r,g,b)` /
  `RESET` dentro de la interpolacion de `print`/`println`.
- El flag opcional `-r WxH` se parsea con `vio_parse_int` de la stdlib nativa
  (`extern "stdlib/native/io/vesta_io"`), cerrando LIM-8.

### Matriz de resultados (version modular modernizada)

| Componente | interp (`-m vm`) | JIT (`-m jit`) | AOT PE | AOT ELF (WSL) |
|:-----------|:----------------:|:--------------:|:------:|:-------------:|
| Lectura de fichero (stdlib `vex_fileio`) | OK | OK | OK\* | OK\*\* |
| Parseo de cabecera BMP (little-endian) | OK | OK | OK | OK |
| Impresion de atributos | OK | OK | OK | OK |
| Volcado ANSI truecolor (builtin `bg_rgb`) | **byte-exacto** | **byte-exacto** | **byte-exacto** | **byte-exacto** |
| CLI args (`args_get`) | OK | OK | no (LIM-5) | no (LIM-5) |
| **Modulo con clase cruzada** (`main_aot.vx` + `bmp.vx`) | OK | OK | **OK** | **OK** |

\* AOT PE: la clase `BMP_Image` vive en un modulo importado.  Los metodos de
instancia cruzando modulo ya compilan a nativo (BUG-5 CERRADO 2026-07-04); el
unico bloqueante restante para el `main.vx` modular completo es el gap de
`args_get` (LIM-5), no el codegen de metodos de clase.

\*\* AOT ELF (cross-compile desde Windows): validado byte-exacto con la variante
auto-contenida usando `extern "libc.so.6"` DIRECTO, porque `import "vex_fileio"`
en ELF-desde-Windows selecciona la rama kernel32 por el HALLAZGO-2 (`@Target`
evalua contra el HOST, no contra el formato de salida).

Resultado byte-exacto confirmado: **interp == JIT == AOT-PE == AOT-ELF(WSL)**
para ambas imagenes (60x3 sin padding y 3x60 con padding).

---

## BUGS CERRADOS (verificados con su repro minimo)

### BUG-1 [CERRADO] `T*` (host pointer) devuelto por funcion con rama `return null`

Antes: un `u8* f()` con una rama `return null` perdia el flag `is_host_ptr` de
su valor de retorno; el llamante deref-eaba leyendo memoria VM (byte0=0 /
SIGSEGV al indexar).  **Ahora arreglado.**

Repro (interp y JIT dan `byte0=66`):
```vex
u8* make_buf(i32 bad) { if (bad != 0) { return null; } u8* p = malloc(8); p[0] = 66; return p; }
i32 main() { u8* buf = make_buf(0); println("byte0=${buf[0]}"); free(buf); return 0; }
```

### BUG-2 [CERRADO] JIT emitia NUL en la interpolacion que CONSTRUYE un string

Antes: en JIT, `return "${chr(27)}[48;2;${r};${g};${b}m";` producia bytes NUL en
cada fragmento interpolado.  **Ahora interp == JIT** (secuencia ANSI correcta).

### BUG-3 [CERRADO] El especificador `:char` se ignoraba al construir un string

Antes: `return "${e:char}X"` con `e=27` producia `"27X"` en vez de `"\x1bX"`.
**Ahora** produce el byte ESC (0x1b) correctamente.

### BUG-4 [CERRADO] `${x:hex}` divergia entre interp / JIT / AOT

Antes: interp/JIT daban `0x0000000000004D42` (64 bits, mayusculas) y AOT
`0x4d42`.  **Ahora los tres modos coinciden**: `0x4d42` (minusculas, sin
relleno).  (El control de ancho/mayusculas/sin-prefijo estilo `%02X` sigue
sin exponerse; ergonomia menor, no bug.)

---

## BUGS NUEVOS (destapados por la modernizacion)

### BUG-5 [CERRADO 2026-07-04] Los metodos de una CLASE definida en un modulo IMPORTADO no se compilan en AOT

**Fix**: la devirtualizacion nativa (`native_poo`) construia el callee del CALL
directo con el nombre MANGLED de la clase importada (`widget__Widget__bump`),
pero el body del metodo en el dep se emite con el nombre LOCAL
(`Widget__bump`, igual que el ctor `__new_Widget`).  El linker AOT dejaba el
simbolo mangled indefinido (y el dead-elim descartaba el body por no-usado).
Solucion en `lower_class_method_call` (lowering.cpp): al devirtualizar, resolver
el nombre de la clase via `imported_helper_suffix` del layout (nombre local),
igual que ya hacia el ctor.  Verificado byte-exacto en interp/JIT/AOT-PE/AOT-ELF
con el repro minimo (`widget.vx` + `mainx.vx`, `w.bump(41)` -> 42).

**Que intentaba**: la version modular (`main_aot.vx` importa `bmp.vx`, que
define `class BMP_Image`) compilada a nativo.  `main` hace
`new bmp.BMP_Image()` y luego `img.load(...)`, `img.printBMPAttributes()`,
`img.dumpTerminal()`.

**Sintoma**: el AOT enlaza, pero el binario NO ARRANCA:
- PE: `ExitCode 0xC0000139` (STATUS_ENTRYPOINT_NOT_FOUND) — 0 bytes de salida.
- ELF (WSL): `symbol lookup error: undefined symbol: bmp__BMP_Image__load`.

**Causa observada**: los tres metodos de instancia (`load`,
`printBMPAttributes`, `dumpTerminal`) NO se emiten en `.text`; se emiten como
**simbolos externos indefinidos** y el linker AOT los mete por error en la
tabla de imports de **msvcrt.dll**:
```
$ objdump -x main_aot.exe | grep msvcrt
msvcrt.dll: bmp__BMP_Image__load
msvcrt.dll: bmp__BMP_Image__printBMPAttributes
msvcrt.dll: bmp__BMP_Image__dumpTerminal
```
El constructor (`new` / `__new_bmp__BMP_Image`) y el destructor SI se resuelven;
solo fallan las llamadas a metodos regulares sobre el objeto.

**Aislamiento** (que SI funciona, descartando causas):
- Clase + `main` en el **mismo fichero** (sin `import`) -> AOT OK.
- Funciones LIBRES cruzadas de modulo (`vex_fileio.file_size`, etc.) -> AOT OK.
- interp y JIT de la version modular -> OK (byte-exacto).

**Repro minimo** (2 ficheros; `widget.vx` + `mainx.vx`):
```vex
// widget.vx
public class Widget {
    public i32 v;
    public Widget() { this.v = 0; }
    public i32 bump(i32 n) { this.v = this.v + n; return this.v; }
}
```
```vex
// mainx.vx
import "widget";
i32 main() {
    widget.Widget w = new widget.Widget();
    i32 r = w.bump(41);            // <- callee emitido como import de msvcrt.dll
    println("r=${r}");
    return 0;
}
```
- `vm --run mx.velb -m vm`  -> `r=41`  (interp OK)
- `vm --run mx.velb -m jit` -> `r=41`  (JIT OK)
- `vm -m aot --vex mainx.vx --format pe --emit exe -o mainx.exe` -> el .exe
  hace `STATUS_ENTRYPOINT_NOT_FOUND` (importa `widget__Widget__bump` de
  msvcrt.dll).  Igual en ELF.

**Conclusion (historica)**: el sintoma era un mismatch de mangling entre la
llamada devirtualizada (nombre mangled del consumer) y el body (nombre local del
dep).  Ya CERRADO (ver el fix arriba): los metodos de instancia de una clase
importada compilan a nativo en PE y ELF.  El unico bloqueante restante del
`main.vx` modular completo en AOT es el gap de `args_get`/`args_count` (LIM-5),
independiente de este bug.

---

## HALLAZGOS (limitaciones de cross-compile, no bugs de codegen)

### HALLAZGO-2 [cross-compile] `@Target(os:...)` evalua contra el HOST, no contra el formato de salida

Al compilar `--format elf` **desde un host Windows**, `@Target("os:windows")`
sigue siendo verdadero (el HOST es Windows), asi que `vex_fileio` selecciona la
rama **kernel32** y el ELF resultante referencia `CreateFileA` -> en Linux:
`undefined symbol: CreateFileA`.

- El **codegen ELF es correcto**: un ELF que usa `extern "libc.so.6"` DIRECTO
  (sin `@Target`) compilado desde Windows **arranca y funciona en WSL**
  (`read 54 bytes, magic=0x4d42`).
- El problema es solo la **seleccion de rama por plataforma**: `@Target` deberia
  poder resolverse contra el OS del TARGET de compilacion (`--format elf` ->
  `os:linux`), o exponer un flag `--target-os`.  Hoy el cross-compile de
  `vex_fileio` de Windows a ELF elige la rama equivocada.
- Workaround para la validacion ELF: variante auto-contenida con
  `extern "libc.so.6"` directo (sin depender de `@Target`).

---

## LIMITACIONES CERRADAS

### LIM-1 [CERRADO] Lectura de fichero binario a un buffer
Ahora en la stdlib: `import "vex_fileio"` -> `file_size(path)` +
`file_read_into(path, buf, size)`.  Windows kernel32, POSIX libc; libc-free.
(Sujeto a HALLAZGO-2 al cross-compilar a ELF desde Windows.)

### LIM-2 [CERRADO] ANSI truecolor (48;2;r;g;b)
Ahora son builtins del lenguaje: `bg_rgb(r,g,b)`, `fg_rgb(r,g,b)` y `RESET`
dentro de la interpolacion de `print`/`println`.  No construyen `StringObject`,
por lo que funcionan identico en interp / JIT / AOT.

### LIM-8 [CERRADO] Parseo de entero desde string
`extern "stdlib/native/io/vesta_io" { fn vio_parse_int(u64 s, u64 len) -> i64;
fn vio_parse_ok() -> i64; }`.  Detecta base por prefijo (0x/0b/0o), signo,
espacios y overflow.  Usado en `main.vx` para el flag `-r WxH`.

---

## LIMITACIONES VIGENTES

### LIM-3 [lenguaje] `main(string[] args)` es inerte; no hay longitud de array
- `len(args)` no existe.  No hay `args.length`.
- Los argumentos SOLO se obtienen con `args_count()` / `args_get(i)`;
  `args_get(0)` es el primer argumento de usuario (no el nombre del programa).

### LIM-4 [CLI/runner] Los flags con guion los consume el parser de la VM
`vm --run p.velb foo -r 32x32` -> el programa solo recibe `foo` y `32x32`.
Hay que usar el separador `--`: `vm --run p.velb -m vm -- foo -r 32x32`.

### LIM-5 [AOT] `args_count()`/`args_get()` no compilan a nativo
`op 'getarg' requiere scheduler/procesos (runtime)`.  Cualquier programa que lea
argumentos de CLI no compila en target nativo.  Por eso el AOT usa `main_aot.vx`
(path fijo).  ORTOGONAL a BUG-5 (esta es una limitacion conocida de args).

### LIM-7 [lenguaje] `streq` es comptime-only; para runtime hay que usar `str_equals`
`streq(path, "-h")` con `path` runtime -> error comptime.  Hay que usar
`str_equals`.

### LIM-9 [no-portable] Codecs de imagen (JPG/PNG -> BMP) y resize AWT no migrables
ReaderBMP usa `javax.imageio.ImageIO` (decodifica JPG/PNG) y `Graphics2D`/
bilinear para redimensionar.  Sin equivalente en Vex/stdlib.  Se migro solo la
ruta BMP-directo.  Los flags `-k`/`-s` (mantener convertido/redimensionado) y el
redimensionado real de `-r` dependen de esos codecs y quedan fuera; `-r` solo
parsea y reporta las dimensiones (demo de `vio_parse_int`).

---

## Notas menores (no bloqueantes)

- **Warnings de narrowing** en cada `... << 24` y en los `return i32`: ruido;
  obligan a casts `(i32)` explicitos para silenciar.
- **Warning "import 'bmp' no se usa"** en `main.vx` aunque `bmp.BMP_Image` SI
  se usa (acceso cualificado): falso positivo del detector de imports sin usar.
- **Cross-module: acceso siempre cualificado.**  Con `import "x"` hay que
  escribir `x.fn(...)` / `x.Tipo` / `new x.Tipo()`.  Por diseno.
- **Line endings**: la VM emite `\n` (LF) puro; correcto.
