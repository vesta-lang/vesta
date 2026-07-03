# ReaderBMP -> Vex: bugs y limitaciones encontrados

Migracion del proyecto Java **ReaderBMP** (+ su libreria **JColorsTerm**) al
lenguaje **Vex** de VestaVM.  El objetivo del ejercicio era destapar bugs y
limitaciones; la migracion es el vehiculo.

Binario usado: `cmake-build-release/vm.exe` (sin rebuild).  Compilador NO
modificado.  Imagenes de prueba: `Ejemplo60x3.bmp` (60x3, sin padding) y
`Ejemplo3x60.bmp` (3x60, padding de 3 bytes/fila), ambas 24bpp BGR.

**Verificacion**: el volcado de pixeles a terminal se comparo BYTE A BYTE
contra una referencia Python del algoritmo (`dump_buffer_cli`).  En modo
interprete el volcado es identico a la referencia para ambas imagenes
(con/sin padding).

---

## Resumen de estado de la migracion

| Componente | interp (`-m vm`) | JIT (`-m jit`) | AOT (`-m aot`) |
|:-----------|:----------------:|:--------------:|:--------------:|
| Lectura de fichero (kernel32, libc-free) | OK | OK | OK |
| Parseo de cabecera BMP (little-endian) | OK | OK | OK |
| Impresion de atributos (print interp.) | OK | OK | OK |
| Volcado ANSI truecolor **en contexto print** (`reader_bmp.vex`) | OK (byte-exacto) | OK (byte-exacto) | OK (byte-exacto) |
| Volcado ANSI via **helpers que construyen string** (`colors.vex`) | OK (byte-exacto) | **BUG (NUL)** | **no compila** |
| CLI args (`args_get`) | OK | OK | **no soportado** |

Hay DOS variantes del programa migrado:
- `reader_bmp.vex` (un solo fichero, ANSI en contexto `print`): **byte-exacto
  en los 3 modos**.  Path fijo para poder compilar a AOT.
- `main.vex` + modulos `bmp.vex` / `bmp_io.vex` / `colors.vex` (mirror
  idiomatico de las clases Java, ANSI via helpers que devuelven `string`):
  **correcto en interp**; JIT y AOT afectados por los bugs #2 y #9.

---

## BUGS (algo que deberia funcionar y no)

### BUG-1 [CRITICO] Un `T*` (host pointer) devuelto por una funcion con rama `return null` pierde su naturaleza host

**Que intentaba**: una funcion que hace `malloc`, llena el buffer, y lo
devuelve; con una rama de error `return null` (patron Java/C clasico:
`readBMPFile` que devuelve la imagen o lanza/nulo).

**Sintoma**: el puntero devuelto tiene el VALOR correcto (misma direccion host
impresa dentro y fuera), pero al dereferenciarlo en el llamante lee CERO /
basura, como si fuese una direccion de memoria de la VM en vez de host.  En el
caso real, guardar ese buffer en un campo y luego indexarlo -> **SIGSEGV**
(exit 139).

**Repro minimo** (mismo modulo, interp y JIT):
```vex
u8* make_buf(i32 bad) {
    if (bad != 0) { return null; }   // <-- rama null
    u8* p = malloc(8);
    p[0] = 66;
    return p;
}
i32 main() {
    u8* buf = make_buf(0);
    println("byte0=${buf[0]}");      // imprime 0 (deberia 66)
    free(buf);
    return 0;
}
```
`byte0=0` (esperado `66`).  Tambien ocurre pasando el puntero por
`u8**` out-param con una rama `*out = null`.

**Hipotesis de causa**: al fusionar los valores de retorno (`null` no-host y el
puntero host) se computa `is_host_ptr = false` para el resultado, y el llamante
emite un LOAD de memoria VM (`mov`) en vez de host (`movh`).

**Enmascarado por inlining**: si la funcion se inlinea (funcion libre llamada
una sola vez, sin `extern`), la naturaleza host fluye directamente del `malloc`
y NO se ve el bug.  Se expone cuando la funcion NO se inlinea: cruce de modulo,
o funciones con llamadas `extern` FFI estaticas (que bloquean el inlining).
Por eso `reader_bmp.vex` (funcion inlineada) funciona pero la version modular
crasheaba.

**Workaround aplicado**: patron de dos pasos que NO devuelve el host-ptr con
rama null.  `file_size(path) -> i64` + el llamante hace `malloc` LOCAL +
`file_read_into(path, buf, size)` recibiendo el buffer como ARGUMENTO (los
host-ptr pasados como argumento SI conservan su naturaleza).  Ver `bmp_io.vex`.

---

### BUG-2 [ALTO] Divergencia JIT vs interp: la interpolacion que CONSTRUYE un string produce bytes NUL

**Que intentaba**: helper `bg_rgb(r,g,b)` que devuelve la secuencia ANSI como
string: `return "${chr(27)}[48;2;${r};${g};${b}m";` (equivalente al constructor
de `JColorsTerm`).

**Sintoma**: en **JIT** los fragmentos interpolados (`chr(27)` y cada
`${int}`) salen como bytes **NUL (0x00)** en vez de los caracteres correctos;
la CANTIDAD de bytes es la correcta (misma longitud) pero los VALORES son cero.
Las partes literales (`[48;2;`, `;`, `m`) salen bien.  En **interp** es
correcto.

**Repro minimo** (un solo fichero):
```vex
string bg(i32 r, i32 g, i32 b) {
    return "${chr(27)}[48;2;${r};${g};${b}m";
}
i32 main() { print(bg(0,75,125)); println(""); return 0; }
```
- interp: `\e[48;2;0;75;125m`  (correcto)
- jit:    `\0[48;2;\0;\0\0;\0\0\0m`  (NUL en cada fragmento interpolado)

**Impacto**: el volcado de color de la version modular es correcto en interp
pero sale corrupto en JIT.

---

### BUG-3 [MEDIO] El especificador de formato `:char` se IGNORA en contexto de construccion de string

**Que intentaba**: incrustar el byte ESC via `${ESC:char}` dentro de un string
que se construye (return de una funcion / `string s = "..."`).

**Sintoma**: en contexto de CONSTRUCCION de string, `${x:char}` imprime el
valor DECIMAL (`27`) en lugar del caracter (0x1b).  En contexto de `print`
directo (`print("...${x:char}...")`) funciona bien.  Afecta por igual a
variables locales, literales y `comptime` const.

**Repro minimo**:
```vex
string mk() { i32 e = 27; return "${e:char}X"; }   // -> "27X"  (mal)
i32 main() {
    print(mk()); println("");                       // 27X
    print("${27:char}Y"); println("");              // \e Y  (bien, contexto print)
    return 0;
}
```

**Workaround aplicado**: usar `chr(27)` (devuelve un string de 1 caracter con el
byte) e interpolarlo sin `:char`: `"${chr(27)}[48;2;..."`.  Ver `colors.vex`.

---

### BUG-4 [MEDIO] `${x:hex}` diverge entre interp y AOT + ergonomia pobre

**Que intentaba**: imprimir el magic BMP en hex (Java: `get_string_hex()` ->
`"424D"`).

**Sintoma** (mismo fuente, `i32 v = 0x4D42; println("${v:hex}")`):
- interp / JIT: `0x0000000000004D42`  (mayusculas, rellenado a 16 digitos = 64 bits, con prefijo `0x`)
- AOT:          `0x4d42`               (minusculas, sin relleno, con prefijo `0x`)

Ademas `:hex` SIEMPRE antepone `0x` (si el usuario escribe un literal `"0x"`
antes, sale `0x0x...`) y no ofrece control de ancho/mayusculas/sin-prefijo para
replicar formatos tipo `%02X` de Java.

---

## LIMITACIONES (feature que no existe / hueco de stdlib)

### LIM-1 [stdlib] No hay primitiva para leer un fichero binario a un buffer
`stdlib/vex/vex_io.vex` solo cubre SALIDA a consola (WriteConsole/WriteFile) y
`panic`.  No existe algo tipo `read_file(path) -> buffer`.  Implementado en
`bmp_io.vex` con **kernel32 directo** (CreateFileA/GetFileSizeEx/ReadFile/
CloseHandle), CERO msvcrt, para que valga tambien en AOT/freestanding.
**Candidata a stdlib** (`file_size` + `file_read_into`, libc-free).

### LIM-2 [stdlib] No hay ANSI truecolor (48;2;r;g;b)
La stdlib solo expone identificadores magicos basicos (RED/GREEN/BOLD).  Para
replicar `dump_buffer_cli` de JColorsTerm hubo que construir a mano las
secuencias 24-bit (`colors.vex`).  **Candidata a stdlib**.

### LIM-3 [lenguaje] `main(string[] args)` es inerte; no hay longitud de array
- `len(args)` no existe (`funcion no declarada: 'len'`).
- No hay `args.length` ni acceso util al parametro `args`.
- Los argumentos de CLI SOLO se obtienen con los builtins `args_count()` /
  `args_get(i)`, y `args_get(0)` es el PRIMER argumento de usuario (no el nombre
  del programa).

### LIM-4 [CLI/runner] Los flags con guion son consumidos por el propio parser de la VM
`vm --run p.velb foo -r 32x32` -> el programa solo recibe `foo` y `32x32`
(el `-r` desaparece).  Hay que usar el separador `--`:
`vm --run p.velb -- foo -r 32x32`.  Esto bloquea migrar los flags de ReaderBMP
(`-k -s -r WxH -h`) sin el separador.

### LIM-5 [AOT] `args_count()`/`args_get()` no compilan a nativo
`op 'getarg' requiere scheduler/procesos (runtime)`.  Cualquier programa que lea
argumentos de CLI no compila en target `bare`.  Workaround: variante sin
argumentos con path fijo (`main_aot.vex`).

### LIM-6 [AOT] Construir un StringObject por interpolacion no compila en target bare
Una funcion que DEVUELVE un `string` interpolado (p.ej. `bg_rgb`) falla en AOT:
`op 'strmake' requiere strings GC (StringObject)` + `op 'getproc' requiere
contexto VM`.  IMPORTANTE: la interpolacion en contexto **print**
(`println("${x}")`) SI funciona en AOT.  Por eso la version single-file
(ANSI en `print`) compila a AOT y la modular (helpers que devuelven string) no.

### LIM-7 [lenguaje] `streq` es comptime-only; para runtime hay que usar `str_equals`
`streq(path, "-h")` con `path` runtime -> error `comptime_streq: argumento 0 no
es comptime-evaluable`.  Hay que usar `str_equals`.  El doble nombre (uno
comptime, otro runtime) es confuso.

### LIM-8 [lenguaje] No hay builtin de parseo de entero desde string
No existe `parse_int`/`str_to_int`/`atoi`.  Para migrar `-r 32x32`
(`Integer.parseInt`) habria que parsear a mano recorriendo los bytes de
`str_cstr(s)`.  (Hueco anotado; no se implemento el parse de `-r` en la version
final.)

### LIM-9 [no-portable] Codecs de imagen (JPG/PNG -> BMP) y resize AWT no migrables
ReaderBMP usa `javax.imageio.ImageIO` (decodifica JPG/PNG) y
`Graphics2D`/bilinear para redimensionar.  Sin equivalente en Vex/stdlib.  Se
migro solo la ruta BMP-directo (leer, atributos, volcado).  Los flags `-k`/`-s`
(mantener convertido/redimensionado) dependen de esos codecs y quedan fuera.

---

## Notas menores (no bloqueantes)

- **Warnings de narrowing** en cada `... << 24` y en el `return` de helpers
  `i32` (p.ej. `read_u32le`): ruido, pero obligan a `(i32)` explicitos para
  silenciar.  Correcto en semantica, molesto en volumen.
- **Cross-module: acceso siempre cualificado.**  Con `import "x"` (forma
  simple) hay que escribir `x.fn(...)` / `x.Tipo` / `new x.Tipo()`; un nombre
  desnudo da `funcion no declarada`.  Es por diseno (no bug), pero es friccion
  al migrar codigo de un solo namespace.
- **Line endings**: la VM emite `\n` (LF) puro; correcto.  (La referencia
  Python en Windows metia `\r\n`; tras normalizar, match byte-exacto.)
