# ReaderBMP -> : bugs y limitaciones encontrados

Migracion del proyecto Java **ReaderBMP** (+ su libreria **JColorsTerm**) al
lenguaje **** de VestaVM.  El objetivo del ejercicio era destapar bugs y
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
  se usa la stdlib: `import "vx_fileio"` con `file_size(path)` +
  `file_read_into(path, buf, size)` (Windows kernel32 / POSIX libc, libc-free).
- `colors.vx` (truecolor ANSI local via `chr(27)`) -> **eliminado**.  Ahora
  se usan los **builtins del lenguaje** `bg_rgb(r,g,b)` / `fg_rgb(r,g,b)` /
  `RESET` dentro de la interpolacion de `print`/`println`.
- El flag opcional `-r WxH` se parsea con `vio_parse_int` de la stdlib nativa
  (`extern "stdlib/native/io/vesta_io"`), cerrando LIM-8.

### Matriz de resultados (version modular modernizada)

| Componente | interp (`-m vm`) | JIT (`-m jit`) | AOT PE | AOT ELF (WSL) |
|:-----------|:----------------:|:--------------:|:------:|:-------------:|
| Lectura de fichero (stdlib `vx_fileio`) | OK | OK | OK\* | OK\*\* |
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
auto-contenida usando `extern "libc.so.6"` DIRECTO, porque `import "vx_fileio"`
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
```vx
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
- Funciones LIBRES cruzadas de modulo (`vx_fileio.file_size`, etc.) -> AOT OK.
- interp y JIT de la version modular -> OK (byte-exacto).

**Repro minimo** (2 ficheros; `widget.vx` + `mainx.vx`):
```vx
// widget.vx
public class Widget {
    public i32 v;
    public Widget() { this.v = 0; }
    public i32 bump(i32 n) { this.v = this.v + n; return this.v; }
}
```
```vx
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
- `vm -m aot --vx mainx.vx --format pe --emit exe -o mainx.exe` -> el .exe
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
sigue siendo verdadero (el HOST es Windows), asi que `vx_fileio` selecciona la
rama **kernel32** y el ELF resultante referencia `CreateFileA` -> en Linux:
`undefined symbol: CreateFileA`.

- El **codegen ELF es correcto**: un ELF que usa `extern "libc.so.6"` DIRECTO
  (sin `@Target`) compilado desde Windows **arranca y funciona en WSL**
  (`read 54 bytes, magic=0x4d42`).
- El problema es solo la **seleccion de rama por plataforma**: `@Target` deberia
  poder resolverse contra el OS del TARGET de compilacion (`--format elf` ->
  `os:linux`), o exponer un flag `--target-os`.  Hoy el cross-compile de
  `vx_fileio` de Windows a ELF elige la rama equivocada.
- Workaround para la validacion ELF: variante auto-contenida con
  `extern "libc.so.6"` directo (sin depender de `@Target`).

---

## LIMITACIONES CERRADAS

### LIM-1 [CERRADO] Lectura de fichero binario a un buffer
Ahora en la stdlib: `import "vx_fileio"` -> `file_size(path)` +
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
bilinear para redimensionar.  Sin equivalente en /stdlib.  Se migro solo la
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

---

## Sprint 2026-07-14: ESCRITURA de BMP con overlays (@overlay struct)

Segunda fase del ejercicio: ademas de LEER, ahora se CREAN y ESCRIBEN ficheros
BMP.  El formato se modela con VISTAS TIPADAS (`@overlay struct`) sobre el buffer
host crudo del fichero (el idioma nativo de Vesta para formatos binarios, igual
que los parsers de PE/ELF), y las operaciones son FUNCIONES LIBRES que reciben un
`u8*` -- **sin clases**.  El "objeto BMP" es simplemente un `u8*` (buffer con
cabecera + pixeles) cuyo tamano total vive en el propio campo
`BmpFileHeader.size`.

## Que se anadio

- **`bmp.vx`** reescrito: dos overlays `BmpFileHeader` (14 bytes) +
  `BmpInfoHeader` (40 bytes, base = buf + 14) + funciones libres:
  - `bmp_create(w, h, bpp, compression, planes, res_h, res_v, palette, important)
    -> u8*` : reserva el buffer, lo pone a cero (qwords) y rellena TODA la
    configuracion del BMP a traves de los overlays.  `bmp_create24(w, h)` es el
    atajo 24bpp.
  - `bmp_set_pixel(buf, x, y, r, g, b)` / `bmp_get_r|g|b(buf, x, y)` : escritura/
    lectura de pixel (BGR, bottom-up, con padding de fila a multiplo de 4).
  - `bmp_save(buf, path) -> i32` : vuelca el buffer completo (tamano leido del
    header) a disco via `vx_fileio.file_write_from`.
  - `bmp_file_size(path)` + `bmp_read_into(path, buf, size)` : lectura en dos
    pasos (ver LIM-10).
  - Accesores overlay `bmp_magic/bmp_total_size/bmp_offset/bmp_width/bmp_height/
    bmp_bpp`.
  - `bmp_print_attributes(buf)` + `bmp_dump_terminal(buf)` (truecolor `bg_rgb`).
- **`writer_bmp.vx`** (nuevo): crea una 8x8 24bpp con un gradiente conocido, la
  escribe, la relee y verifica el round-trip (cabecera + pixeles).  `main`
  devuelve 42 en exito.
- **`vx_fileio.vx`** (stdlib) ampliado con `file_write_from(path, buf, size)`
  (Windows kernel32 `WriteFile` + `CREATE_ALWAYS`; POSIX `open O_WRONLY|O_CREAT|
  O_TRUNC` + `write`), simetrico a `file_read_into`.
- `reader_bmp.vx` / `main.vx` / `main_aot.vx` adaptados al modelo overlays +
  funciones libres (la clase `BMP_Image` se elimino).

## Matriz de resultados (writer_bmp.vx, round-trip 8x8)

| Modo | Escribe BMP | Round-trip | Exit-code de main |
|:-----|:-----------:|:----------:|:-----------------:|
| interp (`-m vm`)  | OK | OK | 42 (\*) |
| JIT (`-m jit`)    | OK | OK | 42 (\*) |
| AOT PE (nativo)   | OK | OK | **42** (medido) |
| AOT ELF (nativo)  | codegen OK | no verificable aqui (\*\*) | -- |

\* En interp/JIT, `vesta --run` devuelve exit 0 en exito por diseno; el valor 42
de `main` se comprueba con el mensaje "Round-trip OK" y con la vista previa
truecolor.  En AOT el exit-code del proceso SI es el `return` de `main` (medido:
42).

\*\* El ELF64 se genera correctamente (`file` lo reconoce como ELF x86-64), pero
no se pudo ejecutar en WSL en esta maquina ("no se admite la virtualizacion
anidada").  Ademas, compilado desde host Windows arrastra HALLAZGO-2 (vx_fileio
selecciona la rama kernel32 -> el ELF referencia `CreateFileA`/`WriteFile`, que
no existen en Linux).  Para un ELF ejecutable en Linux habria que usar
`extern "libc.so.6"` directo (sin `@Target`) o un flag `--target-os`.

El BMP generado es un fichero valido: `xxd` muestra `42 4d` ('BM'), size=246,
offset=54, header=40, 8x8, 24bpp, res=2835 px/m; el gradiente se relee identico.

---

## BUG NUEVO (destapado por la escritura)

### LIM-10 [lenguaje/codegen] El valor de RETORNO `u8*` de una funcion que llena su buffer via un `extern` pierde la naturaleza host-ptr; el caller lee memoria VM al parsear

**Sintoma minimo**: una funcion `u8* bmp_load(path)` que hace
`malloc` + `vx_fileio.file_read_into(path, buf, size)` (una llamada `extern`) +
`return buf`.  En el caller, deref-ear el puntero devuelto lee CEROS (memoria
VM), no el contenido del fichero -- de forma INCONSISTENTE entre accesos.

Repro (con `salida8x8.bmp` ya escrito, magic 0x4D42 @ offset 0):
```vx
u8* back = bmp.bmp_load("salida8x8.bmp");   // u8* devuelto por fn con extern
println("magic=${bmp.bmp_magic(back):hex}");   // -> 0x0   (ESPERADO 0x4d42)
println("width=${bmp.bmp_width(back)}");       // -> 8     (correcto!)
println("bytes=${back[0]}");                   // -> 0     (ESPERADO 66='B')
```

**Lo esperado**: `back` deberia comportarse como cualquier host-ptr; parsear la
cabecera via overlays deberia devolver los valores reales del fichero.

**Lo que pasa**: la naturaleza host del valor de RETORNO se degrada.  El sintoma
es INCONSISTENTE: `bmp_width(back)` (campo `i32 @0x04` del info-header) devuelve
8 correcto, pero `bmp_magic(back)` (campo `i16 @0x00` del file-header) devuelve 0.
`back[0]` directo tambien da 0.  Es decir, ALGUNOS overlays leen bien y otros
leen memoria VM, sin patron obvio (parece depender del offset/ancho del campo y
del codegen).

**Aislamiento** (que SI funciona, descartando causas):
- `u8* bmp_create(...)` que escribe su buffer via OVERLAYS (stores host) antes de
  `return buf` -> el retorno SI conserva la naturaleza host (el compilador la
  prueba por los stores).  `bmp_magic` sobre su retorno da 0x4d42 correcto.
- Hacer `malloc` LOCAL en el caller + pasar el buffer a `file_read_into` (dos
  pasos) -> el buffer local conserva la naturaleza host; TODOS los accesos y
  overlays leen bien.
- Pasar el puntero degradado como ARGUMENTO a una funcion cross-modulo a veces
  funciona (p.ej. `bmp_get_r(back, ...)` leyo pixeles correctos) y a veces no
  (`bmp_magic(back)` fallo).  La inconsistencia confirma que es un problema de
  tracking del flag `is_host_ptr`, no de valor.

**Relacion con BUG-1**: BUG-1 (return-null pierde is_host_ptr) se documento como
CERRADO para el caso `buf[0]` sobre el retorno de una fn con rama `return null`.
LIM-10 es un residuo del mismo problema pero con un disparador distinto: el
buffer se llena via una llamada `extern` (que el compilador no puede analizar), y
el sintoma aparece al PARSEAR via overlays cross-modulo, no solo al indexar.

**Workaround usado** (idiomatico, el que prescribe la propia `vx_fileio`): NO
devolver el `u8*` desde una funcion loader.  Patron de DOS PASOS: el caller mide
(`bmp_file_size`), reserva con `malloc` LOCAL, y pasa el buffer como ARGUMENTO a
`bmp_read_into`.  El malloc local conserva la naturaleza host y todos los overlays
parsean bien.  `bmp_load` se elimino en favor de `bmp_file_size` +
`bmp_read_into`.

### LIM-11 [lenguaje] La construccion de un `@overlay struct` importado NO admite la forma cualificada `modulo.Tipo(ptr)`

**Sintoma**: con `import "bmp"`, intentar overlayar en el caller:
```vx
bmp.BmpFileHeader fh = bmp.BmpFileHeader(back);
```
falla en compilacion:
```
error: llamada a 'bmp.BmpFileHeader': se esperaban 0 args, recibidos 1
error: tipo del inicializador (void) incompatible con tipo declarado (bmp__BmpFileHeader)
```

**Lo esperado**: igual que un overlay local `T(ptr)` mapea la vista, `mod.T(ptr)`
deberia mapear una vista de un overlay importado.

**Lo que pasa**: el constructor-overlay cualificado (`mod.Tipo(ptr)`) no se
reconoce como tal; se interpreta como una llamada normal de 0 args.  Los overlays
locales (single-file) SI funcionan con `Tipo(ptr)`.

**Workaround usado**: no overlayar el buffer directamente en el caller.  Se
exponen FUNCIONES LIBRES en `bmp.vx` (`bmp_magic`, `bmp_width`, `bmp_get_r`, ...)
que reciben el `u8*` y construyen el overlay DENTRO del modulo (donde el tipo es
local); el caller solo llama a esas funciones.  Es tambien el modelo pedido
(operaciones = funciones libres sobre el `u8*`), asi que el workaround coincide
con el diseno.
