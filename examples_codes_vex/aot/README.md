# Ejemplos AOT (Phase AOT.3 Paso 2b-ii)

Programas Vex que compilan a ejecutable nativo standalone (sin runtime VM)
con CALL intra-modulo + tail-call con TCO genuino.

Compilar (PE para Windows, ELF para Linux):

    vm --vex examples_codes_vex/aot/01_call.vex -m aot -o 01_call.exe        # PE (default host)
    vm --vex examples_codes_vex/aot/01_call.vex -m aot --format elf -o 01_call.elf

Ejecutar -> el `return` de `main` es el codigo de salida del proceso:

| Ejemplo                 | exit-code esperado |
|:------------------------|-------------------:|
| 01_call.vex             | 42                 |
| 02_call_chain.vex       | 40 (main->inc->triple) |
| 03_recursion.vex        | 120 (fact(5), recursion no-tail) |
| 04_tail_call_tco.vex    | 64 (loop(5000000,0) mod 256; TCO O(1) pila, no desborda) |

El codegen va por el path vreg en ABI HOST_LEAF (args en arg_regs, retorno en
RAX, sin ProcessVM*); las CALL cross-funcion y los tail-call se resuelven con
relocations rel32 parcheadas tras el layout de `.text`.

## Referencias a datos (.rodata) -- Paso 2b/2a

| Ejemplo            | exit-code | nota |
|:-------------------|----------:|:-----|
| 05_rodata.vex      | 67 ('C')  | `char* m="ABC"; m[2]` -> literal en `.rodata` |
| 06_call_rodata.vex | 69 ('E')  | CALL + dato `.rodata` cruzando la llamada |

Por defecto las refs a datos son **RIP-relativas** (position-independent, listo
para PIE/.so). Con `--no-pie` se emiten **absolutas** (`mov reg,imm64`, requieren
base de imagen fija; el emisor PE limpia DYNAMIC_BASE para fijarla), analogo a
`gcc/clang -no-pie`.

## Secciones definidas por el usuario (dev OS) -- Paso 2b

`@section(".name")` (datos o codigo) coloca la funcion/dato en una seccion
propia; permisos por convencion del nombre (`.text*`->rx, `.rodata*`->r,
`.data*`/`.bss*`->rw) o explicitos `@section(".boot","rwx")`.  Las llamadas
cross-seccion las resuelve el ObjectWriter (relocs rel32).  Una funcion con
`@section` NO se inlinea (permanece fisica en su seccion).

| Ejemplo               | exit-code | nota |
|:----------------------|----------:|:-----|
| 07_section_devos.vex  | 42        | `boot_entry` en `.boot` (RWX), main en `.text`, call cross-seccion |

## Simbolos de seccion (dev OS) -- Paso 2c

`section_start(".x") -> void*`, `section_end(".x") -> void*`,
`section_size(".x") -> u64` -- estilo `__start_NAME`/`__stop_NAME` del linker.
El writer AOT los resuelve tras el layout (relocs ADDR/END/SIZE).  En `-m vm`/`-m jit`
(sin secciones nativas) devuelven 0.

| Ejemplo               | exit-code | nota |
|:----------------------|----------:|:-----|
| 08_section_symbols.vex | size de .boot | valida `end-start == size` (28 con este codegen) |

## Objeto relocatable .o (AOT.4-ext) -- linkable con toolchains externos

`--emit obj --format elf` produce un ELF ET_REL (.o) en vez de un ejecutable:
SIN `_start`, con `main` como simbolo GLOBAL y las relocs como registros
(`.rela.text`: R_X86_64_PC32 / R_X86_64_64).  Se linka con `ld`/`gcc`/`clang`,
que aportan el crt (`_start` -> `main`) + libc + permiten linker scripts (dev OS):

    vm --vex examples_codes_vex/aot/01_call.vex -m aot --format elf --emit obj -o 01_call.o
    gcc 01_call.o -o prog        # el crt de gcc llama a main
    ./prog; echo $?              # 42

Validado: 01_call.o (call), 06_call_rodata.o (data reloc -> .rodata),
07_section_devos.o (.text + .boot cross-section) linkan con gcc y devuelven
42/69/42.  COFF (.obj para link.exe) pendiente.

## Objeto COFF .obj (Windows) -- linkable con link.exe / gcc-mingw

`--emit obj --format pe` produce un COFF .obj (Machine AMD64): SIN _start, `main`
como simbolo EXTERNAL, relocs COFF (IMAGE_REL_AMD64_REL32 / ADDR64) contra el
simbolo de seccion del target.  COFF lleva el addend EN el campo (no en un record
aparte), asi que el emisor pre-escribe target_off en el sitio.

    vm --vex examples_codes_vex/aot/01_call.vex -m aot --format pe --emit obj -o 01_call.obj
    gcc 01_call.obj -o prog.exe        # gcc-mingw aporta el crt -> main
    ./prog.exe; echo $?                # 42

Validado: 01_call.obj / 06_call_rodata.obj / 07_section_devos.obj linkan con
gcc-mingw (TDM-GCC) y devuelven 42/69/42.  (Reusa la API de LibCOFFparse.)

## Libreria compartida .so (ELF) -- dlopen/dlsym

`--emit shared --format elf` produce un ELF ET_DYN (.so) PIC que EXPORTA todas
sus funciones (sin main; sin _start).  Cargable con dlopen + dlsym:

    vm --vex examples_codes_vex/aot/09_shared_lib.vex -m aot --format elf --emit shared -o libfoo.so
    // host C: dlopen("./libfoo.so") + dlsym("add") -> add(40,2) == 42

Validado: libfoo.so (add/triple) cargado via dlopen+dlsym en Linux -> 42/42.
(.dll PE shared pendiente.)

## Cross-target: el ABI lo decide el FORMATO, no el host

El codegen usa el ABI del TARGET (SysV para `--format elf`, Win64 para
`--format pe`), no el del host -> se puede generar ELF en Windows y PE en Linux.
Validado: ELF generado en Windows corre en WSL Linux (01/03/06 = 42/120/69).

## Libreria compartida .dll (PE Windows) -- LoadLibrary/GetProcAddress

`--emit shared --format pe` produce una DLL PE32+ (IMAGE_FILE_DLL + tabla de
exports .edata).  Cargable con LoadLibrary + GetProcAddress:

    vm --vex examples_codes_vex/aot/10_shared_dll.vex -m aot --format pe --emit shared -o foo.dll
    // host C: LoadLibraryA("foo.dll") + GetProcAddress("add") -> add(40,2) == 42

Validado: foo.dll (add/triple) cargada via LoadLibrary+GetProcAddress -> 42/42.
Codigo PIC (RIP-rel); base fija (DYNAMIC_BASE limpiado, sin .reloc).

## Strings UTF-8 (Vex Embed Inc 6) -- .length() / .bytes() / .cstr() / .wstr()

El value-string nativo es UTF-8.  Los accesores distinguen code-points de bytes
y exponen las dos vistas que necesita el FFI de Windows:

- `.length()` -> numero de CODE-POINTS (cuenta los bytes que no son continuacion
  UTF-8).  Para ASCII coincide con el numero de bytes.
- `.bytes()`  -> numero de BYTES del buffer.
- `.cstr()`   -> `u8*` UTF-8 NUL-terminado (Win32 `*A` / libc).
- `.wstr()`   -> `u16*` UTF-16LE NUL-terminado (Win32 `*W`), con pares suplentes
  para code-points astrales (> U+FFFF).  El CALLER es dueno del buffer
  (transitorio para FFI).

    vm --vex examples_codes_vex/aot/59_string_utf8.vex -m aot --emit exe -o 59.exe

Validado (PE Windows + ELF WSL): "hello" con e-acento = 5 code-points, 6 bytes,
`wstr()[1]` == 0xE9; clef de sol (U+1D11E) = 1 code-point, 4 bytes, par suplente
0xD834 0xDD1E.  Ejemplo 59 -> exit 42.

## Linker propio (Phase AOT.5) -- enlazar .o sin ld/gcc

`vm --link a.o [b.o ...] -o prog [--format elf] [--entry sym] [--link-base 0xADDR]`
fusiona uno o mas objetos relocatables (ELF64 `ET_REL` -- los que emite
`--emit obj`, o un `.o` de gcc) en un ejecutable nativo, resolviendo los
simbolos cross-file y las relocaciones (`R_X86_64_PC32`/`PLT32`/`64`/`32`/`32S`)
sin depender de ld/gcc.  Reusa el motor de relocs del `ObjectWriter`.

- **Hosted** (sin `--entry`): sintetiza un `_start` que llama a `main` y termina
  el proceso.  Requiere un `main` global.
- **dev-OS** (`--entry _kstart`): usa ese simbolo como entrada SIN stub
  (kernel/bootloader cuyo punto de entrada es propio).  `--link-base` fija la
  base de carga (e.g. `0x100000`).

    vm --vex kernel.vex -m aot --emit obj --format elf -o kernel.o
    gcc -c -ffreestanding rt.c -o rt.o          # runtime/driver en C
    vm --link kernel.o rt.o -o kernel.elf --format elf --entry _kstart

Validado por ejecucion (ELF64, WSL): un solo .o Vex -> 42; .o Vex + .o de C
(gcc) cross-file (Vex referencia un extern que C define) -> 42; `--entry`
custom sin main/stub -> 42.  Test: `tests/aot/link_test.sh`.
Slice 1 = ELF64 in/out; PE/COFF y x86-32 son follow-ups.

### Slice 2: multi-.o Vex + .bss

- **Multi-.o Vex**: una libreria `.vex` SIN `main` compila a un `.o` que EXPORTA
  sus funciones de usuario como globales (los helpers internos `__vex_*`/`__new_*`
  quedan locales -> no colisionan al enlazar varios `.o` Vex).  Otro `.o` las
  referencia con `extern "lib" { fn ...; }` y el linker las resuelve cross-file.

      vm --vex lib.vex -m aot --emit obj --format elf -o lib.o   # sin main
      vm --vex app.vex -m aot --emit obj --format elf -o app.o   # extern + main
      vm --link app.o lib.o -o app.elf --format elf

- **.bss**: el emisor ELF EXEC soporta secciones NOBITS (globales sin
  inicializar, de Vex o de un `.o` de C) -- VA en el segmento R+W con
  `p_memsz > p_filesz`; el loader las zerifica.  Permite enlazar un runtime C
  (allocator con estado en `.bss`) o un kernel con globales sin inicializar.

### Inicializacion de programa cross-.o (CPU-dispatch)

Las operaciones de string y `memcpy` usan tablas de punteros elegidas en
runtime por CPUID (despacho a variantes SIMD / `@HelperOverride`).  Cada `.o`
lleva sus propios slots (`__vex_strlen_fp`, etc.) y sus inits
(`__vex_cpu_init` -> `__vex_memcpy_init` -> `__vex_strdisp_init`), que dejan
los slots apuntando a la variante elegida.  En un solo modulo, esos inits se
encadenan al arranque de `main`.

Al enlazar varios `.o` el linker garantiza que se ejecuten los inits de TODOS
los objetos (no solo los del `.o` que tiene `main`): exporta esos inits como
globales en cada `.o`, los recolecta de todos los objetos y sintetiza un
`__vex_premain` que los llama en orden (`cpu` -> `memcpy` -> `strdisp`) y salta
a `main`.  Asi un `.o` Vex SIN `main` que use strings/`memcpy` tambien inicializa
sus slots; sin esto, sus tablas quedarian sin inicializar.  Cada objeto conserva
sus propios slots (no se fusionan); el orden `cpu` antes que `memcpy`/`strdisp`
se respeta porque estos leen el global de features que `cpu_init` escribe.  El
init del `.o` de `main` ademas corre via su propio prologo: es idempotente
(re-ejecutarlo deja el mismo valor).

Validado: una libreria `.vex` SIN `main` que usa `.length()`, enlazada con el
`.o` de `main` y un runtime C (`malloc`/`free`), ejecuta correctamente.

Con `--entry` (sin `main`/stub) el `__vex_premain` no se sintetiza: el punto de
entrada propio (kernel/bootloader) es responsable de invocar los inits si usa
esas operaciones.

### Slice 3: enlazar .obj COFF -> PE (Windows)

El linker auto-detecta el formato de cada objeto de entrada por su magic
(ELF64 o COFF AMD64) y produce el ejecutable segun `--format` (elf|pe).  Asi el
mismo `vm --link` enlaza objetos de Windows:

    vm --vex kernel.vex -m aot --emit obj --format pe -o kernel.obj
    gcc -c rt.c -o rt.obj           # COFF de MinGW/TDM-GCC (o cl /c con MSVC)
    vm --link kernel.obj rt.obj -o kernel.exe --format pe

Diferencias COFF vs ELF que maneja el linker: el addend vive EN el campo (no en
un registro RELA), los nombres COMDAT/agrupados se pliegan a su seccion base
(`.text$mn` -> `.text`, `.rdata$zzz` -> `.rdata`), las secciones vacias que gcc
emite (`.data`/`.bss` de tamano 0) se descartan, y las tablas SEH
(`.pdata`/`.xdata`, relocs ADDR32NB/RVA) tambien se descartan (sin unwinding
nativo en esos frames, igual que el resto del AOT).  Relocs soportadas:
`REL32` / `ADDR64` / `ADDR32`.

Validado por ejecucion (Windows): un solo `.obj` Vex -> 42; multi-`.obj` Vex
(lib sin main + app) -> 42; `.obj` Vex + `.obj` de C (TDM-GCC) cross-file -> 42.

### Objetos relocatables de 32-bit (.o ELF32 / .obj COFF i386)

`--aot-arch x86-32 --emit obj` produce objetos de 32-bit que CONSERVAN la
extension estandar (`.o`/`.obj`) para enlazarlos con toolchains externos
(gcc -m32, ld, link.exe):

    vm --vex prog.vex -m aot --aot-arch x86-32 --emit obj --format elf -o prog.o
    gcc -m32 prog.o -o prog        # ELF32 i386, el crt llama a main

    vm --vex prog.vex -m aot --aot-arch x86-32 --emit obj --format pe -o prog.obj
    # COFF i386 (Machine 0x14c), linkable con link.exe / i686-mingw

ELF32 usa `SHT_REL` (i386: el addend vive EN el campo, no en un record RELA),
relocs `R_386_PC32` / `R_386_32`.  COFF i386 usa `IMAGE_REL_I386_REL32` /
`IMAGE_REL_I386_DIR32`.

Validado: `prog.o` (fib recursivo) enlaza con `gcc -m32` y devuelve 55;
`prog.obj` se reconoce como `pe-i386` con sus simbolos y relocs (ejecucion
end-to-end requiere un linker de 32-bit para Windows).

### Script de enlace ESCRITO EN VEX (configurable) -- `--link-script`

El linker es configurable con un `.vex` normal (sin sintaxis nueva): defines una
funcion `void link()` que llama a builtins de configuracion, y el linker la
compila + ejecuta para leer el layout.  Es intuitivo (es Vex) y potente (logica/
condicionales/comptime completos para CALCULAR las direcciones).

    vm --link kernel.o -o kernel.elf --format elf --link-script link_layout.vex

```vex
void link() {
    u64 load = 0x100000;
    if (debug_build()) { load = 0x200000; }   // logica Vex real
    base(load);                                // direccion de carga
    entry("_kstart");                          // entry propio (sin _start)
    stack_size(align_up(64 * 1024, 4096));
}
```

Builtins: `base(u64)`, `entry(string)`, `stack_size(u64)`, `place_section(string,
u64)`, `section_bytes(string)` (tamano de una seccion ya fusionada), `align_up(u64,
u64)`, `debug_build()` (true con `--link-debug`).  Los CLI `--link-base`/`--entry`
tienen prioridad sobre el script.  Mecanismo: el linker compila el `.vex` a `.velb`
in-process y lo ejecuta en una VM con los builtins registrados (FFI in-process);
estos escriben la config que el linker aplica.

Validado por ejecucion: `base(0x800000)` -> el ELF se carga en 0x800000;
`debug_build()` con `--link-debug` -> 0x900000; `entry("_kstart")` -> entry propio,
el binario corre (exit 42).  `base`/`entry`/`stack` se aplican ya; el placement por
seccion (`place_section`) se aplicara cuando el emisor soporte VAs fijas (siguiente).

### El linker enlaza tambien objetos de 32-bit (ELF32 / COFF i386)

`vm --link` auto-detecta la arquitectura de cada objeto por su cabecera
(ELF32/ELF64, COFF i386/AMD64) y produce el ejecutable correspondiente
(ELF32/PE32 vs ELF64/PE32+).  Todos los objetos de un enlace deben coincidir en
32 vs 64 bits.  Cierra el ciclo "sin toolchain externo" tambien para 32-bit:

    vm --vex kernel.vex -m aot --aot-arch x86-32 --emit obj --format elf -o k.o
    vm --link k.o -o k.elf --format elf        # ELF32 EXEC, sin ld

ELF32 i386 usa `SHT_REL` (addend en el campo, no en un record); el linker lo
lee igual que COFF.  El stub `_start` y el contenedor son de 32-bit
automaticamente.  Validado por ejecucion (WSL): un .o ELF32 (fib recursivo) ->
ELF32 EXEC = 55; cross-file 32-bit (lib sin main + app) = 42; COFF i386 ->
PE32 valido (ejecucion requiere un linker/host de 32-bit).  64-bit sin regresion.

### place_section: colocar una seccion en una VA fija (dev-OS)

El builtin `place_section(name, addr)` del link-script Vex coloca una seccion en
una direccion virtual EXACTA (header multiboot, MMIO, .text/.data en direcciones
del mapa de memoria del kernel).  El emisor EXEC rellena el fichero hasta
`addr - base` para que `vaddr == addr` (un PT_LOAD cubre el hueco; va en orden
de direccion ascendente -- una VA por debajo de la posicion actual es un error
claro).  Combina con `base()`/`entry()` para control total del layout:

```vex
void link() {
    base(0x400000);
    entry("_kstart");
    place_section(".boot", 0x410000);   // .boot en VA fija
}
```

Validado por ejecucion (WSL): un kernel con `_kstart` en `.boot`, colocado en
0x410000 por el script -> readelf confirma `.boot` y el entry en 0x410000; corre
(exit 42).  Con esto el link-script Vex controla base, entry, stack y la VA de
cada seccion -- configurable y potente, en el propio lenguaje.

### @Naked: funciones sin prologo/epilogo (ISRs, stubs, cambio de modo)

`@Naked` (Phase NR -- "Vesta sin runtime") marca una funcion cuyo cuerpo se
emite VERBATIM: el codegen NO inserta prologo (`push rbp`/`sub rsp`), NI
epilogo, NI el `ret` implicito.  El programador provee la salida real
(`ret`/`iretq`/`iret`).  Misma semantica que `__attribute__((naked))` de GCC.
Es el primitivo fundacional para dev OS: con `@Naked` + inline `asm { ... }` se
escriben ISRs, stubs de entry y rutinas de cambio de modo sin un solo byte
anadido por el compilador.

```vex
@Naked void isr_timer() {
    asm volatile {
        push rax
        mov al, 0x20
        out 0x20, al        // EOI al PIC maestro
        pop rax
        iretq
    }
}
```

Garantias del codegen AOT:

- cero prologo/epilogo/ret implicito -- el `objdump` del cuerpo es EXACTAMENTE
  el asm escrito.
- NO se elimina por dead-strip aunque ningun `CALL` la referencie (un ISR se
  referencia desde la IDT, invisible al compilador): las `@Naked` se siembran
  siempre en el BFS de compilacion.
- simbolo GLOBAL en el `.o` -> la rutina de setup de la IDT/GDT la referencia
  por nombre (`dq isr_timer` en un bloque `bytes`, o desde otro `.o`/asm).

Un `asm { ... }` SIN `register(...)` bindings (lee directamente los registros
del ABI) es valido en `@Naked`: el cuerpo se ensambla verbatim y no marca
in/out vregs.  Ejemplo completo: `60_naked_isr.vex`.  Test: `tests/aot/naked_test.sh`.
Validado por ejecucion (PE host): `add_naked(40,2)` -> exit 42; ISR void ->
`.o` ELF con `isr_timer` GLOBAL y cuerpo byte-exacto.

### Mini-SO (VestaOS): bootloader propio -> protegido -> modo largo

El subdirectorio **[`os/`](os/)** contiene un mini sistema operativo completo:
un bootloader propio que pasa a modo protegido (32) y luego a modo largo (64),
con un **kernel escrito en Vesta** (driver VGA, punteros, bucles, FFI inline-asm),
todo compilado por el AOT (sin gcc/ld/nasm).  Ver [`os/README.md`](os/README.md).

- `os/kernel.vex`: boot(16) -> protegido(32, paginacion PAE+LME+PG) ->
  largo(64) -> `kmain()` en Vesta sobre VGA.
- `os/os_protected.vex`: variante minima de 32 bits.
- `os/run.py`: construir / ejecutar / validar en QEMU (portable).

Las transiciones de modo usan FAR JUMPS SIMBOLICOS y la base del GDT es un
simbolo, resueltos por el emisor AOT cross-bloque:

```vex
jmp 0x08:pm32            // far jump 16->32 a otro bloque, por SIMBOLO
...
bytes gdtr { dw 0x0027
             dd gdt_ents }   // base del GDT por SIMBOLO (cross-bloque)
```

El emisor AOT resuelve estas referencias cross-bloque (un bloque `asm`/`bytes`
referencia a otro por nombre): near `jmp`/`call` -> REL32; `dd sym` -> IMM32
(VA absoluta de 32 bits); `dq sym` -> ABS64; `jmp SEG:sym` (far) -> IMM32 sobre
el offset.  Validado en QEMU (`python os/run.py test all`): ambos arrancan,
ejecutan las etapas y salen por isa-debug-exit (exit 133).

Limitacion conocida (Keystone): un solo bloque `asm` NO puede cambiar de modo
con `.code32`/`bits 32` (Keystone ignora la directiva intra-run).  Por eso cada
modo es un bloque `@bits` separado, referenciados por simbolo -- mas limpio.
Las referencias a simbolos en OPERANDOS DE MEMORIA (`lgdt [sym]`,
`mov rsi, sym`) tampoco se resuelven aun; se usa la direccion @at fija (p.ej.
`lgdt [0x7DC0]`).  El far jump y los datos (`dd sym`) SI son simbolicos.

## thread_local (TLS nativo) -- ejemplos 64-70

`thread_local <T> NAME = init;` declara una variable cuyo **almacenamiento es
privado de cada hilo**: cada hilo ve y muta su PROPIA copia, inicializada con la
plantilla `init`.  Es el modelo de C `__thread` / C++ `thread_local`, sin locks
ni sincronizacion (cada hilo accede a su copia directamente).

| Ejemplo | Que demuestra | exit |
|:--------|:--------------|-----:|
| [`64_thread_local_basico.vex`](64_thread_local_basico.vex) | una variable por-hilo basica | 5 |
| [`65_thread_local_multi.vex`](65_thread_local_multi.vex) | varias variables, tamanos mixtos, mutacion | 112 |
| [`66_thread_local_fn_loop.vex`](66_thread_local_fn_loop.vex) | persistencia por-hilo en funcion + loop | 10 |
| [`67_thread_local_hilos.vex`](67_thread_local_hilos.vex) | aislamiento por-hilo REAL con un hilo Win32 | 11099 |
| [`68_thread_local_float.vex`](68_thread_local_float.vex) | plantilla float (f64) + `comptime` const | 42 |
| [`69_thread_local_puntero.vex`](69_thread_local_puntero.vex) | `&` de un thread_local + `thread_local T*` | 33 |
| [`70_thread_local_dll.vex`](70_thread_local_dll.vex) | TLS en una `.dll` PE (plantilla via DllMain sintetico) | host C: 0 |

### Por que usarlo

Para estado por-hilo sin contencion: contadores, buffers scratch, el "errno"
de turno, el contexto de un scheduler, etc.  Cada hilo tiene su instancia; no
hace falta un mutex porque nadie mas la toca.  La plantilla `init` se copia a
cada hilo nuevo automaticamente.

La plantilla debe ser una **constante de compilacion** (literal entero / float /
bool / char, o una referencia a un `comptime` const): es la imagen que el
cargador copia a cada hilo, no un store en runtime.  Sin `init` la copia
arranca a cero.

### Como funciona (codegen AOT, sin runtime C)

Vex emite TLS NATIVO -- nada de emutls ni librerias auxiliares dentro del
binario.  El mecanismo difiere por formato:

**ELF (Linux).**  La plantilla va a una seccion `SHF_TLS` (`.tdata`) y se emite
un segmento `PT_TLS`; el cargador dinamico monta el bloque TLS de cada hilo
antes del entry.  El acceso (modelo *local-exec*) es:

```asm
mov rax, %fs:0           ; thread pointer
lea rax, [rax + x@tpoff] ; &x ; tpoff = offset negativo (constante de enlace)
```

El `tpoff` lo resuelve el emisor de ejecutable (`--emit exe`) o el enlazador
propio (`--emit obj` + `vm --link`) -- ambos calculan el offset del simbolo
dentro del bloque TLS.  El linker propio ademas INTEROPERA con objetos TLS de
gcc (los 4 modelos: local-exec, initial-exec, general-dynamic, x86-32).

**PE (Windows).**  El emisor sintetiza la infraestructura nativa del PE: una
seccion `.tls$d` con `_tls_index` (lo rellena el cargador con el indice del
slot), un array de callbacks y el `IMAGE_TLS_DIRECTORY`; pone `DataDirectory[9]`
(TLS) apuntando ahi.  El acceso lee el bloque TLS del hilo desde el TEB:

```asm
mov r10, gs:[0x58]        ; TEB->ThreadLocalStoragePointer
mov r11d, [rip+_tls_index]; indice del slot de este modulo
mov r10, [r10 + r11*8]    ; base del bloque TLS del hilo
lea rax, [r10 + x@secrel] ; &x ; offset del var dentro de .tls
```

El cargador del SO asigna y monta el bloque de cada hilo (tambien para los
hilos creados con `CreateThread`), garantizando el aislamiento.

**PE shared (.dll).**  Una `.dll` necesita un paso extra: el cargador de Windows
NO copia la plantilla (los valores iniciales) a los hilos a menos que la `.dll`
tenga un `DllMain` que dispare la init en cada attach.  Por eso el emisor, ademas
del `IMAGE_TLS_DIRECTORY`, sintetiza `__vex_tls_init` (escribe los valores
iniciales no-cero en el bloque por-hilo y devuelve TRUE) y fija el
`AddressOfEntryPoint` de la `.dll` a esa funcion -- un `DllMain` minimo.  ntdll
lo invoca en cada `DLL_PROCESS_ATTACH` / `DLL_THREAD_ATTACH`, de modo que cada
hilo recibe su copia inicializada.  Funciona con cualquier consumidor (con o sin
CRT).  Ver [`70_thread_local_dll.vex`](70_thread_local_dll.vex) +
[`70_thread_local_dll_host.c`](70_thread_local_dll_host.c) (host C que verifica
plantilla + aislamiento por-hilo).

### Compilar

```
# PE (Windows): ejecutable directo
vm -m aot --vex 64_thread_local_basico.vex --format pe --emit exe -o t.exe

# ELF (Linux): ejecutable directo
vm -m aot --vex 64_thread_local_basico.vex --format elf --emit exe -o t

# ELF: objeto + enlazado con el linker propio (interopera con libc)
vm -m aot --vex 64_thread_local_basico.vex --format elf --emit obj -o t.o
vm --link t.o libc.so.6 -o t --format elf
```

### Limitaciones

- `thread_local` es valido en **ejecutables** (`--emit exe`/`obj`) y en
  **`.dll` PE** (`--format pe --emit shared`, via el `DllMain` sintetico de
  arriba).  En `--emit shared --format elf` (`.so` con `dlopen`) y en
  `--emit bin` (binario plano) el driver lo rechaza con un error claro: una
  `.so` necesita el modelo *initial-exec/general-dynamic* o el TLS dinamico, y
  un binario plano no tiene cargador que monte el bloque.
- La plantilla debe ser constante (no una expresion de runtime).
- Tipos soportados como plantilla: enteros, `bool`, `char`, `f32`/`f64`.  Tipos
  gestionados (`string`, clases) no son plantillas TLS validas.

El test `tests/aot/tls_test.py` compila estos ejemplos (PE nativo + ELF via WSL)
y verifica el exit; los `.c` que aparecen ahi son fixtures de gcc para probar la
INTEROP del linker, no ejemplos del lenguaje.
