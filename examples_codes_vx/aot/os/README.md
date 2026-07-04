# VestaOS — mini sistema operativo con terminal y programas en disco

Un sistema operativo de juguete con **bootloader propio** (real → protegido →
largo), un **kernel modular** escrito en Vesta, una **terminal interactiva**
estilo MS-DOS, y **programas independientes cargados desde el disco** (una
calculadora y un editor de texto).

Todo se compila con el **AOT de Vesta** (`-m aot`): sin gcc, sin ld, sin nasm.
El builder (`run.py`) ensambla la imagen de disco (kernel + directorio +
programas) y genera además una **ISO booteable** (ISO9660 + El Torito) en Python
puro, sin `xorriso`.

## Lo que lo hace "de verdad"

Calc y notepad **no** son código del kernel: son **binarios separados** en el
disco.  La terminal, al teclear `calc 2+3*4`, busca el programa en un
**directorio de disco**, lo **carga en memoria** (0x100000) y **salta a él**
pasándole una **tabla de API** (punteros a las funciones de servicio del
kernel).  El programa llama de vuelta al kernel a través de esa tabla.  Es el
modelo clásico de un SO: kernel + programas de usuario + una ABI entre ambos.

## Estructura

| Fichero | Rol |
| :------ | :-- |
| `kernel.vx` | Raíz: bootloader (asm 16/32/64) + `kmain` + auto-test. |
| `prog.vx` | Cargador de programas + tabla de API del kernel. |
| `fat.vx` | Driver **FAT12** de solo-lectura (BPB, FAT, root dir, cadenas de cluster). |
| `fb.vx` | Consola de texto sobre framebuffer **VBE** 800×600×32 (fuente 8×16 del BIOS). |
| `shell.vx` | Terminal: builtins (help/ver/cls/echo/exit) + lanza programas de disco. |
| `term.vx` | E/S: teclado/serie de entrada; VGA/serie/debugcon de salida; `read_line`. |
| `kbd.vx` | Driver de teclado PS/2 (scancode → ASCII, Shift). |
| `console.vx` | Driver VGA 80×25 (cursor, scroll, color). |
| `serial.vx` | Driver COM1 (in/out). |
| `lib.vx` | Utilidades (`itoa`, `memset`). |
| `prog_calc.vx` | **Programa**: calculadora (evaluador de expresiones). |
| `prog_notepad.vx` | **Programa**: editor de texto por líneas. |
| `os_protected.vx` | Variante mínima de 32 bits (modo protegido). |
| `run.py` | Builder: construir / ejecutar / validar / ISO. |

## Requisitos

- El binario `vm`/`vm.exe` de Vesta (p. ej. en `cmake-build-release/`).
- **QEMU** (`qemu-system-x86_64`, y `qemu-system-i386` para la variante 32-bit).
- **Python 3** para `run.py`.

## Uso

```sh
# Desde esta carpeta (examples_codes_vx/aot/os):

python run.py run                 # construye y arranca el OS en QEMU (ventana)
python run.py run --iso           # arranca desde una ISO (CD) en vez del disco
python run.py run protected       # la variante de 32 bits

python run.py test all            # validacion headless (CI)
python run.py build kernel        # ensambla la imagen de disco (kernel.bin)
python run.py iso kernel          # genera kernel.iso (CD booteable)
```

En la ventana de QEMU teclea comandos en la terminal:

```
A:\> help
A:\> calc (2 + 3) * 4 - 5
A:\> edit
A:\> ver
```

`calc` y `edit` se cargan **desde el disco** cada vez que los invocas.

## Cómo funciona

```text
  DISCO (imagen FAT12)                    MEMORIA (en ejecucion)
  +------------------------+
  | sector 0     boot+BPB  |  BIOS -->   0x7C00  boot -> protegido -> largo
  | sect 1..21   kernel    |  int13h -->  ramdisk (imagen FAT12 completa)
  | sect 22..23  FAT x2    |             0x70000 tabla de API (punteros)
  | sect 24      root dir  |  carga -->  0x100000 programa en ejecucion
  | sect 25..    clusters  |             0x90000  pila del kernel
  |   CALC, EDIT (ficheros)|
  +------------------------+
```

La imagen es una **FAT12 estándar** (BPB en el boot sector, dos FATs, root
directory de 16 entradas, datos en clusters).  Es legible por herramientas
externas (`mtools`, `mount -t vfat`).

1. **Boot** (sector 0, asm): la BIOS lo carga en 0x7C00.  Empieza con
   `jmp short + BPB FAT12`.  Lee el resto de la imagen con `int 0x13 AH=42`
   (LBA extendido) como un *ramdisk* en memoria, habilita A20, GDT, paginación,
   y entra en modo largo (64-bit).
2. **kmain** ejecuta un auto-test (carga `calc` del disco y lo corre) y luego
   lanza la **terminal**.
3. La terminal lee una línea (teclado o serie), separa comando y argumentos.
   Si es un builtin lo ejecuta; si no, el driver **FAT** (`fat.vx`) busca el
   nombre 8.3 en el root directory, sigue su **cadena de clusters** para cargar
   el fichero a 0x100000 y **salta a su entry** vía puntero de función:
   `prog_main(tabla_api, args)`.
4. El **programa** usa la **tabla de API** (un array de punteros a `puts`,
   `dec`, `getline`, ...) para llamar a los servicios del kernel.  No está
   enlazado con el kernel: solo conoce los índices de la tabla.

## Capacidades de Vesta / AOT que esto ejercita

- **Punteros a función**: tomar la dirección de una función (`(u64) foo`),
  guardarla en una tabla, y **llamar por puntero** (`((fn(...)->R) addr)(args)`)
  → la base de la ABI kernel↔programa.
- Sistema de **módulos** (`import`) compilado en AOT a una sola imagen.
- **Globals mutables**, structs, punteros crudos, bucles, división/módulo.
- E/S por puerto con `asm { in/out }`; control de layout (`@section`, `@at`).
- Compilación a **binario plano** a distintas bases (`--bin-base`, `--no-pie`).
- El builder genera la **ISO booteable** en Python puro.

## Ampliarlo

- Más programas de disco: crea `prog_<x>.vx` (entry `prog_main(api, args)`) y
  añádelo a `KERNEL_PROGRAMS` en `run.py`.
- IDT + interrupciones (teclado/timer) con funciones `@Naked` como ISRs.
- Escritura FAT12 + driver ATA on-demand (hoy la FAT es solo-lectura sobre el
  ramdisk; un driver ATA permitiría leer/escribir el disco real sin cargarlo entero).
- Más servicios en la tabla de API (cls, color, lectura de teclado por scancode).
