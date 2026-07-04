# reader_bmp (ReaderBMP migrado a Vex)

Migracion a **Vex** del proyecto Java [ReaderBMP](https://github.com/desmonHak/ReaderBMP)
+ su libreria [JColorsTerm](https://github.com/desmonHak/JColorsTerm): lee un
BMP de 24 bits (BGR, sin compresion), imprime los atributos de cabecera y
vuelca los pixeles a la terminal como bloques de color ANSI truecolor
(secuencia `48;2;r;g;b`), replicando `dump_buffer_cli`.

El objetivo real del ejercicio es **destapar bugs y limitaciones** del
lenguaje/compilador/runtime.  Ver [`BUGS_LIMITACIONES.md`](BUGS_LIMITACIONES.md).

## Modernizado (2026-07): sin workarounds

La version modular ya NO usa modulos-parche locales.  Usa la **stdlib** y los
**builtins** del lenguaje:

- Lectura binaria del fichero: `import "vex_fileio"` (stdlib) ->
  `file_size(path)` + `file_read_into(path, buf, size)`.  Libre de libc de
  formato (Windows kernel32 / POSIX libc).
- Truecolor 24-bit: builtins `bg_rgb(r,g,b)` / `fg_rgb(r,g,b)` / `RESET` dentro
  de la interpolacion de `print`/`println` (no construyen `StringObject`;
  funcionan identico en interp/JIT/AOT).
- Flag `-r WxH`: parseado con `vio_parse_int` de la stdlib nativa.

Los antiguos `bmp_io.vx` y `colors.vx` se **eliminaron**.

## Ficheros

| Fichero | Rol |
|:--------|:----|
| `bmp.vx` | `class BMP_Image` (mirror de la clase Java): parseo + atributos + volcado.  Usa `vex_fileio` + `bg_rgb`. |
| `main.vx` | Entry con CLI args (interp/JIT).  Flag opcional `-r WxH` via `vio_parse_int`. |
| `main_aot.vx` | Variante sin args (path fijo) para AOT.  Codigo correcto; su AOT esta bloqueado por BUG-5 (ver abajo). |
| `reader_bmp.vx` | Version single-file historica (structs + `ffi_open` msvcrt).  Byte-exacta en interp/JIT/AOT-PE.  Referencia de validacion. |
| `Ejemplo60x3.bmp`, `Ejemplo3x60.bmp` | Imagenes de prueba (60x3 sin padding, 3x60 con padding). |

## Uso

Desde `examples_codes_vex/reader_bmp/`, con `vm` = `../../cmake-build-release/vm.exe`:

```sh
# Version modular con CLI (interp / JIT).  El separador `--` es OBLIGATORIO
# para pasar el path (ver LIM-4 en BUGS_LIMITACIONES.md):
vm --vex main.vx -o reader_bmp
vm --run reader_bmp.velb -m vm  -- Ejemplo60x3.bmp
vm --run reader_bmp.velb -m jit -- Ejemplo3x60.bmp
vm --run reader_bmp.velb -m vm  -- Ejemplo60x3.bmp -r 32x24   # demo parse_int

# Version single-file, portable a los 3 modos (path fijo Ejemplo60x3.bmp):
vm --vex reader_bmp.vx -o reader_bmp_sf
vm --run reader_bmp_sf.velb -m vm
vm -m aot --vex reader_bmp.vx --format pe --emit exe -o reader_bmp_sf_aot.exe
```

## Validacion byte-exacta

El volcado de pixeles (atributos + bloques truecolor) es **byte-identico** en
`interp == JIT == AOT-PE == AOT-ELF (WSL)` para ambas imagenes (con y sin
padding de fila), comparado contra la referencia `reader_bmp.vx`.

- `interp` / `JIT`: version modular con CLI args -> byte-exacto.
- `AOT` (PE y ELF-WSL): validado byte-exacto con una variante auto-contenida
  (misma clase + `bg_rgb`), porque la version **modular** en AOT dispara BUG-5
  (los metodos de una clase de un modulo importado quedan sin definir en el
  binario nativo).  Ver [`BUGS_LIMITACIONES.md`](BUGS_LIMITACIONES.md#bug-5).

## Que se migro

- Lectura binaria del fichero (stdlib `vex_fileio`, libc-free).
- Parseo little-endian de `BITMAPFILEHEADER` + `BITMAPINFOHEADER`.
- `printBMPAttributes()` (todos los campos de cabecera).
- `dump_buffer_cli` (BMP bottom-up + BGR -> ANSI truecolor), verificado
  byte-a-byte para imagenes con y sin padding de fila.

## Que NO se migro (y por que)

- Decodificacion JPG/PNG -> BMP (`javax.imageio.ImageIO`) y resize bilinear
  (`Graphics2D`): sin equivalente en Vex/stdlib.  Se migro solo la ruta
  BMP-directo.  El flag `-r WxH` solo parsea y reporta las dimensiones (demo de
  `vio_parse_int`); el redimensionado real queda fuera.  Ver LIM-9.
