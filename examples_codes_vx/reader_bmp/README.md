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

- Lectura binaria del fichero: `import "vx_fileio"` (stdlib) ->
  `file_size(path)` + `file_read_into(path, buf, size)`.  Libre de libc de
  formato (Windows kernel32 / POSIX libc).
- Truecolor 24-bit: builtins `bg_rgb(r,g,b)` / `fg_rgb(r,g,b)` / `RESET` dentro
  de la interpolacion de `print`/`println` (no construyen `StringObject`;
  funcionan identico en interp/JIT/AOT).
- Flag `-r WxH`: parseado con `vio_parse_int` de la stdlib nativa.

Los antiguos `bmp_io.vx` y `colors.vx` se **eliminaron**.

## Modelo: overlays + funciones libres (2026-07-14)

El BMP se modela con VISTAS TIPADAS (`@overlay struct`) sobre el buffer host
crudo del fichero -- el idioma nativo de Vesta para formatos binarios (igual que
los parsers de PE/ELF).  **No hay clases**: el "objeto BMP" es simplemente un
`u8*` (buffer con cabecera + pixeles), cuyo tamano total vive en el propio campo
`BmpFileHeader.size`.  Las operaciones son FUNCIONES LIBRES que reciben ese `u8*`:

- `bmp_create(w, h, bpp, compression, planes, res_h, res_v, palette, important)`
  / `bmp_create24(w, h)` -- reservan el buffer y rellenan TODA la configuracion
  del BMP a traves de los overlays.
- `bmp_set_pixel(buf, x, y, r, g, b)` / `bmp_get_r|g|b(buf, x, y)` -- pixel BGR,
  bottom-up, con padding de fila a multiplo de 4.
- `bmp_save(buf, path)` -- vuelca el buffer a disco (`vx_fileio.file_write_from`).
- `bmp_file_size(path)` + `bmp_read_into(path, buf, size)` -- lectura en dos pasos
  (el caller reserva; ver LIM-10).
- `bmp_print_attributes(buf)` + `bmp_dump_terminal(buf)` -- atributos + truecolor.

## Ficheros

| Fichero | Rol |
|:--------|:----|
| `bmp.vx` | Overlays `BmpFileHeader`/`BmpInfoHeader` + funciones libres (crear/leer/pixel/guardar/volcar).  Usa `vx_fileio` + `bg_rgb`. |
| `writer_bmp.vx` | Crea una 8x8 24bpp con un gradiente, la escribe, la relee y verifica el round-trip.  `main` devuelve 42. |
| `main.vx` | Reader con CLI args (interp/JIT).  Flag opcional `-r WxH` via `vio_parse_int`. |
| `main_aot.vx` | Reader sin args (path fijo) para AOT.  Compila y corre nativo (PE/ELF). |
| `reader_bmp.vx` | Version single-file (overlays + `ffi_open` msvcrt).  Byte-exacta en interp/JIT/AOT-PE. |
| `Ejemplo60x3.bmp`, `Ejemplo3x60.bmp` | Imagenes de prueba (60x3 sin padding, 3x60 con padding). |

## Uso

Desde `examples_codes_vx/reader_bmp/`, con `vm` = `../../cmake-build-release/vm.exe`:

```sh
# Version modular con CLI (interp / JIT).  El separador `--` es OBLIGATORIO
# para pasar el path (ver LIM-4 en BUGS_LIMITACIONES.md):
vm --vx main.vx -o reader_bmp
vm --run reader_bmp.velb -m vm  -- Ejemplo60x3.bmp
vm --run reader_bmp.velb -m jit -- Ejemplo3x60.bmp
vm --run reader_bmp.velb -m vm  -- Ejemplo60x3.bmp -r 32x24   # demo parse_int

# Version single-file, portable a los 3 modos (path fijo Ejemplo60x3.bmp):
vm --vx reader_bmp.vx -o reader_bmp_sf
vm --run reader_bmp_sf.velb -m vm
vm -m aot --vx reader_bmp.vx --format pe --emit exe -o reader_bmp_sf_aot.exe
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

- Lectura binaria del fichero (stdlib `vx_fileio`, libc-free).
- Parseo little-endian de `BITMAPFILEHEADER` + `BITMAPINFOHEADER`.
- `printBMPAttributes()` (todos los campos de cabecera).
- `dump_buffer_cli` (BMP bottom-up + BGR -> ANSI truecolor), verificado
  byte-a-byte para imagenes con y sin padding de fila.

## Que NO se migro (y por que)

- Decodificacion JPG/PNG -> BMP (`javax.imageio.ImageIO`) y resize bilinear
  (`Graphics2D`): sin equivalente en Vex/stdlib.  Se migro solo la ruta
  BMP-directo.  El flag `-r WxH` solo parsea y reporta las dimensiones (demo de
  `vio_parse_int`); el redimensionado real queda fuera.  Ver LIM-9.
