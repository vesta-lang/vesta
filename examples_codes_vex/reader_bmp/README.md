# reader_bmp (ReaderBMP migrado a Vex)

Migracion a **Vex** del proyecto Java [ReaderBMP](https://github.com/desmonHak/ReaderBMP)
+ su libreria [JColorsTerm](https://github.com/desmonHak/JColorsTerm): lee un
BMP de 24 bits (BGR, sin compresion), imprime los atributos de cabecera y
vuelca los pixeles a la terminal como bloques de color ANSI truecolor
(secuencia `48;2;r;g;b`), replicando `dump_buffer_cli`.

El objetivo real del ejercicio era **destapar bugs y limitaciones** del
lenguaje/compilador/runtime.  Ver [`BUGS_LIMITACIONES.md`](BUGS_LIMITACIONES.md).

## Ficheros

| Fichero | Rol |
|:--------|:----|
| `reader_bmp.vex` | Version single-file, ANSI en contexto `print`.  **Byte-exacta en interp, JIT y AOT.**  Path fijo (`Ejemplo60x3.bmp`). |
| `main.vex` | Entry idiomatico con CLI args, importa los modulos.  Correcto en interp. |
| `main_aot.vex` | Variante sin args (path fijo) para compilar a AOT nativo. |
| `bmp.vex` | `class BMP_Image` (mirror de la clase Java): parseo + atributos + volcado. |
| `bmp_io.vex` | Lectura de fichero **libc-free** via kernel32 (candidata a stdlib). |
| `colors.vex` | Portado minimo de JColorsTerm: secuencias ANSI truecolor. |
| `Ejemplo60x3.bmp`, `Ejemplo3x60.bmp` | Imagenes de prueba (del repo JColorsTerm). |

## Uso

Desde el directorio `examples_codes_vex/reader_bmp/`, con `vm` = `../../cmake-build-release/vm.exe`:

```sh
# Version single-file, portable a los 3 modos (path fijo Ejemplo60x3.bmp):
vm --vex reader_bmp.vex -o reader_bmp_sf
vm --run reader_bmp_sf.velb -m vm        # interprete
vm --run reader_bmp_sf.velb -m jit       # JIT
vm --vex reader_bmp.vex -m aot --emit exe -o reader_bmp_sf_aot   # nativo PE

# Version modular con CLI (interp).  OJO: el separador `--` es OBLIGATORIO
# para pasar el path (ver LIM-4 en BUGS_LIMITACIONES.md):
vm --vex main.vex -o reader_bmp
vm --run reader_bmp.velb -m vm -- Ejemplo3x60.bmp
```

## Que se migro

- Lectura binaria del fichero (libc-free, kernel32).
- Parseo little-endian de `BITMAPFILEHEADER` + `BITMAPINFOHEADER`.
- `printBMPAttributes()` (todos los campos de cabecera).
- `dump_buffer_cli` (BMP bottom-up + BGR -> ANSI truecolor), verificado
  byte-a-byte contra referencia para imagenes con y sin padding de fila.

## Que NO se migro (y por que)

- Decodificacion JPG/PNG -> BMP (`javax.imageio.ImageIO`) y resize bilinear
  (`Graphics2D`): sin equivalente en Vex/stdlib.  Se migro solo la ruta
  BMP-directo.  Ver LIM-9.
