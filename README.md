<div align="center">
  <img src="./Component 1.svg" width="180" height="180" alt="VestaVM logo" />

# VestaVM

**Una máquina virtual distribuida y un lenguaje moderno, diseñados juntos desde cero.**

[![Licencia](https://img.shields.io/badge/licencia-VMProject-blue.svg)](./LICENSE.md)
[![Estado](https://img.shields.io/badge/estado-interp%20%2B%20JIT%20%2B%20AOT%20nativo-brightgreen.svg)](./doc/ROADMAP.md)
[![Tests](https://img.shields.io/badge/tests-314%2F314%20PASS-brightgreen.svg)](./tests/vex/)
[![JIT](https://img.shields.io/badge/JIT-C1%20geomean%2017.73×%20%2F%20peak%20301×-orange.svg)](./doc/BENCHMARKS.md)
[![Plataformas](https://img.shields.io/badge/plataformas-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey.svg)](#inicio-r%C3%A1pido)

[**Inicio rápido**](./doc/QUICKSTART.md) · [**El lenguaje Vex**](./doc/LANGUAGE.md) · [**Arquitectura**](./doc/ARCHITECTURE.md) · [**Benchmarks**](./doc/BENCHMARKS.md) · [**Roadmap**](./doc/ROADMAP.md)

</div>

---

## ¿Qué es VestaVM?

**VestaVM** es una plataforma completa de ejecución de programas que integra tres
componentes diseñados desde cero para trabajar juntos:

1. **[Vex](./doc/LANGUAGE.md)** — un lenguaje multi-paradigma estáticamente tipado
   con sintaxis C/Java/Python combinada. Soporta clases con herencia/interfaces/AOP,
   genéricos, async/await, pattern matching, borrow checker estilo Rust, smart
   pointers (`unique<T>`/`shared<T>`), FFI nativo declarativo, reflexión runtime,
   y un sistema de **metaprogramación** compile-time potente (`@Macro`, captura
   raw `expr`, introspección zero-overhead, FFI en tiempo de compilación).

2. **Compilador y runtime** — pipeline `Vex -> SSA IR -> bytecode .velb -> VM`. Más
   de 15 pasadas de optimización SSA, dispatcher threaded computed-goto
   (~340 MIPS sostenidos), GC generacional **preciso y movible** (young+old,
   scan por stackmaps + mark-compact deslizante del OldGen + nursery preciso
   + write-barrier old->young), y JIT C1 template-based con **hasta 301× speedup**
   sobre el intérprete en métodos hot (throughput efectivo ~7-20 GIPS de
   instr-VM equivalentes). Además, un **compilador AOT** que produce
   ejecutables nativos **standalone** (PE en Windows y ELF en Linux) sin
   runtime de la VM, con **linker y archivador propios** (`vm --link` /
   `vm --ar`, sin depender de `ld`/`gcc`/`ar` del sistema).

3. **Sistema distribuido nativo (VDP)** — protocolo propio sobre TCP/TLS para
   `rspawn` (spawn remoto), mensajería entre nodos, descubrimiento UDP en LAN,
   y autenticación token/mTLS. La distribución es parte del bytecode, no RPC
   externo.

El proyecto incluye también una shell interactiva (REPL), un lenguaje de scripting
embebido (VestaShellScript `.vsh`), un debugger TCP, y un editor TUI escrito
en el propio Vex.

---

## Ejemplo: cómo se ve Vex

```vex
// Pattern matching, genéricos, smart pointers y borrow checker en 20 líneas.

enum Result<V, E> {
    Ok(V),
    Err(E)
}

unique<HashMap> safe_divide(i32[] nums, i32 divisor) -> Result<unique<HashMap>, string> {
    if (divisor == 0) return Err("division by zero");
    
    unique<HashMap> result = unique_box(hashmap(16));
    borrow_mut<HashMap> view = lend_mut(result);
    
    for (i32 n in nums) {
        write_borrow(view).put(n, n / divisor);
    }
    return Ok(move(result));
}

i32 main() {
    i32[] data = {10, 20, 30, 40};
    match safe_divide(data, 2) {
        case Ok(map)  => println("Got ${map.size()} entries");
        case Err(msg) => println("Error: ${msg}");
    }
    return 0;
}
```

Más ejemplos completos en [`examples_codes_vex/`](./examples_codes_vex/) (180+
programas) y showcase curado en [doc/EXAMPLES.md](./doc/EXAMPLES.md).

---

## Características destacadas

### Lenguaje (Vex)

- **Multi-paradigma**: imperativo + POO + funcional ligero. Sin envoltura
  `class` obligatoria.
- **Tipado estático con inferencia local** y nullability explícita
  (`Optional<T>`, `Result<V,E>`, `nonnull T`, `T !!name`).
- **POO completa**: clases, herencia simple + interfaces, propiedades get/set,
  modificadores `public`/`private`/`protected`/`static`/`final`, destructores
  RAII.
- **AOP nativo**: `@Aspect` con `@Before`/`@After`/`@Around` y `proceed()`.
- **Reflexión runtime**: `forName`, `getClass`, `getField`, `getMethod`,
  `invoke`, `newInstance`.
- **Genéricos** por monomorphización compile-time + fallback runtime con
  `specialize`. Clases, enums, **structs** (`struct Caja<T>`) y **funciones
  libres** (`R id<T>(...)`) genéricas, con inferencia de tipos (`auto`/CTAD).
- **Async / concurrencia**: `@Async`/`await`/`Future<T>`, `spawn`/`spawn here`/
  `spawn on(N)`/`rspawn`, mailboxes (`msgsend`/`msgrecv`), `synchronized` con
  cleanup automático.
- **Fibras / green threads** cooperativos self-hosted en Vex (cuerpos de fibra
  en Vex normal, scheduler cooperativo `yield`/`resume`). Context-switch por
  backend (`swapctx` en intérprete, `fiber_switch` nativo en JIT/AOT) y
  comportamiento **idéntico en los 3 modos** (interp/JIT/AOT, PE y ELF); el JIT
  compila las fibras a nativo sin recaer en el intérprete.
- **Pattern matching** exhaustivo (`match`/`case` con bindings).
- **Smart pointers** zero-overhead: `unique<T>`, `shared<T>` con deleters custom
  para adoptar cualquier recurso del SO.
- **Borrow checker** compile-time estilo Rust con 4 reglas + NLL + reborrow con
  suspend stack + lifetime elision.
- **FFI** declarativo a DLLs (`extern "lib.dll" { fn ...; }`) y runtime
  (`ffi_open`/`ffi_sym`/`ffi_call`).
- **Punteros a función / funciones de primera clase** nativos: `cfn(...)->R`
  (puntero crudo, 8 bytes) distinto del lambda `fn(...)->R` (fat-pointer 16
  bytes), con `&funcion`/`&obj.metodo`; `CALLIND`/`&fn` resuelven a código
  nativo tanto en JIT como en AOT.
- **`synchronized` hookeable** via `@SyncImpl`: el programador sustituye el
  monitor por spinlock, pthread, disable-IRQ (kernel) o lock cooperativo de
  fibras — igual que `@AllocatorOverride`/`@PanicHandler`/`@CustomGC`.
- **Strings** UTF-8/16/32, interpolación `${expr}`, format specifiers
  `${expr:hex:>20}`, triple-quoted. Color de terminal via identificadores
  mágicos (`RED`/`GREEN`/`BOLD`/`RESET`) y **truecolor ANSI 24-bit** con
  `fg_rgb(r,g,b)`/`bg_rgb(r,g,b)` (componentes runtime).
- **Metaprogramación compile-time**: `@Macro` que genera código Vex inyectable,
  captura raw de expresiones arbitrarias (`asm walk(ptr -> 0x10 -> 0x20)`),
  introspección de tipos sin overhead (`sizeof<T>`, `typename<T>`, `kind<T>`,
  `field_count<T>`, `for_each_field<T>`), FFI en tiempo de compilación que
  invoca DLLs del sistema durante el build y embebe los resultados como
  literales, `static_assert(cond, "msg")` con condiciones comptime-evaluables.

### Runtime y compilador

- **Pipeline SSA completo**: ~15 pasadas (DCE, CSE, copy-prop, const-fold, TCO,
  LICM, DSE+SLF, devirt+inline, load_narrow, list scheduling para ILP).
- **Asignador de registros linear scan** con register hinting / coalesce
  (steal-from-active).
- **Dispatcher threaded computed-goto** + inline del icache hit path
  (intérprete ~340 MIPS promedio).
- **JIT C1** baseline (template-based, sin regalloc real) con stackmaps
  precisos para GC. Throughput efectivo **~7-20 GIPS** (instr-VM equivalentes
  por segundo), **17.73× speedup geomean** (hasta 301×) sobre intérprete en métodos hot.
- **Super-instrucciones**: `cmpjmp`/`cmpjmpu`, `decjnz`, `alu3` (9 variantes
  fusionando `mov+OP`), `loadz`/`loadzh` (zero-extend LOAD), `mvtake`,
  `gcallocp`, `spawnargs`, `fulfillhlt`.
- **GC generacional preciso y movible** Young/Old: scan preciso vía stackmaps
  + **mark-compact** (OldGen deslizante in-place) + nursery preciso +
  write-barrier old->young, funcionando en intérprete, JIT y AOT.
- **Multi-threading** real opcional con scheduler placement (`spawn here`,
  `spawn on(N)`).
- **Profile-Guided Optimization (PGO) foundation**: contadores runtime
  zero-overhead (~1 ciclo cuando inactivo, atomic load + branch
  predicted-not-taken) instrumentan branches condicionales (taken/not_taken
  por PC), tipos observados en call sites virtuales (`callvirt`/`callm`,
  hasta 4 tipos distintos por site + contador de megamorfismo polimórfico),
  y allocations por PC. Volcado a fichero binario `.vprof` al exit del
  proceso, consumible por el JIT (warm-start de funciones hot del run
  anterior) y por el compilador AOT (decisiones especulativas hard-coded
  en el ejecutable nativo).

### Sistema distribuido

- **Protocolo VDP** nativo sobre TCP, opcionalmente cifrado con TLS (mTLS o
  token CRAM SHA-256).
- **Descubrimiento UDP** automático de nodos en LAN.
- **rspawn** transparente: el bytecode envía procesos a nodos remotos como si
  fueran locales; `Future<T>` resuelve cross-node automáticamente.
- **memsync** para sincronización de regiones de memoria entre nodos.

### Herramientas

- **REPL** con TAB completion, historial, búsqueda incremental Ctrl+R, aliases,
  variables de entorno, scripts de inicio (`~/.vestarc`).
- **VestaShellScript (.vsh)** — lenguaje embebido para scripting del REPL.
- **Debugger TCP** con protocolo JSON: breakpoints (por addr o `file.vx:line`),
  step/continue, inspección de registros/memoria/stack, GC stats, source-aware.
- **Diagramas Mermaid** del pipeline: `--diagram-vex/ir/vel/all` para AST, SSA
  IR, bytecode visualizado.
- **Map file de simbolos** opt-in via `--emit-map` (debug; off por defecto
  porque cuesta ~60% del tiempo del linker).
- **Profiler del linker** integrado via `VESTA_LINKER_PROFILE=1` (timing
  fase a fase + bytes emitidos por sección).
- **Ensamblador/desensamblador nativo** integrado (Keystone + Capstone) para
  x86, x86_64, ARM, AArch64.
- **Profile dump** via `--profile [path]` (default: `program.vprof`).
  Alternativa por entorno: `VESTA_PROFILE_DUMP=path`. Genera `.vprof`
  binario al exit con counters de branches, tipos observados en cada
  call site polimórfico, y allocations por PC. Sirve tanto al JIT
  (warm-start) como al pipeline AOT futuro (PGO en ejecutables nativos).

---

## Inicio rápido

```bash
# Clonar (incluye submódulos: Keystone, Capstone, LibPEparse)
git clone --recursive https://github.com/desmonHak/VM.git
cd VM

# Compilar (CMake + GCC/Clang)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Hola Mundo en Vex
cat > hola.vx << 'EOF'
i32 main() {
    println("Hola desde Vex ${1 + 1}!");
    return 0;
}
EOF

./build/vm --vex hola.vx -o hola
./build/vm --run hola.velb
# -> Hola desde Vex 2!
```

Guía completa: [**doc/QUICKSTART.md**](./doc/QUICKSTART.md) (5 minutos).

---

## Estado del proyecto

VestaVM tiene el frontend Vex + intérprete + GC + distribuido completos, JIT C1
(libvesta_rt + selector con el path vreg de producción cubriendo el corpus
completo sin divergencia) y un **compilador AOT nativo funcional**: `-m aot`
produce ejecutables **standalone** (PE en Windows y ELF en Linux, este último
validado corriendo en WSL) sin runtime de la VM, con linker y archivador
propios (`vm --link`/`vm --ar`, sin `ld`/`gcc`/`ar` del sistema). Los tres
tiers de deployment (Full/Embed/Bare) están en construcción; el path nativo
core — enteros, float, structs, POO no-virtual, FFI e inline-asm — ya genera
binarios que corren. El **GC** es ahora **preciso y movible** (mark-compact +
nursery preciso + write-barrier) en los tres modos. Las **fibras nativas
suspendibles** (green threads cooperativos) funcionan idéntico en interp/JIT/AOT.
Los macros bajan al IR y se ejecutan vía VM con cache persistente y JIT opcional.
**PGO foundation** disponible vía `--profile <path>` que genera `.vprof`
consumible por el JIT (warm-start) y el compilador AOT.

**Suite de tests E2E**: **314/314 PASS** (`tests/vex/test_vex_e2e.sh`), cubriendo
los 180+ ejemplos del repo + 6 tests negativos del borrow checker + 11 tests
positivos realistas del borrow checker.

### Estadísticas clave

La suite de benchmarks abarca **29 workloads multi-lenguaje** con `main.vx`,
`main.c`, `main.cpp`, `main.py`, `Main.java` y `main.go` cada uno; el runner
`tools/bench/run_all_benches.py` incluye **Go (gc)** con su propio color en las
gráficas. Las cifras publicadas abajo provienen de la última corrida completa
multi-lenguaje (10 lenguajes, 29 workloads) en hardware Intel Core i7-13700KF
(16P/24L, 3.4 GHz base) + 63.8 GB RAM, Windows 10, 3 runs + 1 warmup por bench,
AV desactivado durante el bench, todos los benches completados sin timeouts.
`cmp_fusion` no tiene medición JIT, así que las métricas intérprete→JIT y las
comparativas con JIT se calculan sobre 28 workloads:

| Métrica | Valor |
|---|:---:|
| **Suite e2e Vex** | 314/314 PASS (0 FAIL) |
| **Intérprete: MIPS promedio** | ~340 (threaded computed-goto + super-instr) |
| **JIT C1: cobertura del selector** | path vreg de producción sin divergencia en el corpus |
| **JIT vs intérprete: speedup geomean** | **17.73×** sobre 28 benchmarks |
| **JIT vs intérprete: speedup median** | **23.3×** |
| **JIT vs intérprete: peak speedup** | **301×** (`vec_axpy`) |
| **JIT vs intérprete: ≥100×** | 1/28 benches |
| **JIT vs intérprete: ≥50×** | 4/28 benches |
| **JIT vs intérprete: ≥25×** | 14/28 benches |
| **JIT vs intérprete: ≥10×** | 20/28 benches (71%) |
| **JIT geomean slowdown vs C nativo** | **6.50×** |
| **HotSpot C2 (Java) geomean slowdown vs C** | 10.80× |
| **Go (gc) geomean slowdown vs C** | 2.42× |
| **C++ geomean slowdown vs C** | 0.97× (paridad) |
| **CPython 3.11 geomean slowdown vs C** | 141.26× |
| **Vex JIT vs HotSpot (geomean global)** | **JIT ~40% más rápido que Java** (6.50× vs 10.80×) |
| **JIT vs HotSpot: vence en** | **26/28** benches |
| **JIT vs HotSpot: Java vence en** | 2/28 benches (`fp_jit`, `string_workout`) |
| **JIT vs Go (gc): Go vence en** | 26/28 benches |
| **JIT vs CPython 3.11: supera en** | 27/28 benches (96%) |

**Top speedups intérprete → JIT** (mediana de 3 runs):

| Benchmark | Intérp. | JIT | Speedup |
|---|---:|---:|---:|
| `vec_axpy` (SAXPY float 128M) | 24 540 ms | **81 ms** | **301×** |
| `obj_accum` (acumulador OOP) | 3 976 ms | **72 ms** | **55×** |
| `int_mixed` (10 int ops/iter × 20M) | 2 872 ms | **57 ms** | **51×** |
| `memcpy_loop` (1MB × 100 iter) | 1 854 ms | **37 ms** | **50×** |
| `bitops` (popcnt/clz/ctz heavy) | 3 320 ms | **68 ms** | **49×** |
| `hash_lookup` (FNV-style 50M iter) | 4 342 ms | **93 ms** | **47×** |
| `tight_loop` (50M iter ALU) | 2 183 ms | **48 ms** | **45×** |
| `state_machine` (FSM 10M tokens) | 3 193 ms | **74 ms** | **43×** |
| `nested_loops` (1000×1000) | 2 227 ms | **51 ms** | **43×** |
| `intops_jit` (int ops mixtas) | 1 589 ms | **40 ms** | **39×** |
| `array_sum` (10M loads + ADD) | 1 632 ms | **43 ms** | **38×** |
| `rotops_jit` (rotaciones bitwise) | 1 048 ms | **36 ms** | **29×** |

**Intérprete promedio**: ~340 MIPS sobre la suite completa. Dispatcher
threaded computed-goto, super-instrucciones (alu3, loadz, cmpjmp, decjnz),
regalloc coalescing, IR pass schedule, load_narrow elision.

**JIT C1**: el path vreg de producción cubre el corpus completo sin divergencia
(`diff_harness` reporta interp==vreg siempre); los pocos casos no cubiertos caen
a un path legacy en jubilación. Hot loops aritméticos consiguen **29-301×
speedup** (`vec_axpy` vectorizable llega a 301×). La recursión profunda — caso histórico difícil para JITs C1 — pasa de
1.0× a **13× speedup** gracias a TAILCALL emitido nativamente
(`call + ret` fusionados sin FrameHeader pool) + self-recursive call
patcheado directo a `code_start` (sin trampoline JIT→intérprete). Los
no-soportados restantes (excepciones polimórficas, spawn/distrib,
futures) caen al intérprete graciosamente.

### Comparativa multi-lenguaje (workloads idénticos)

Tiempos wall en ms (mediana de 3 runs; hardware i7-13700KF). Para cada
fila, el **valor más rápido marcado en negrita** (entre C, C++, Vex JIT,
Java, Python y Go):

| Bench | C | C++ | Vex JIT | Java HotSpot 25 | Python 3.11 | Go (gc) |
|---|---:|---:|---:|---:|---:|---:|
| `alloc` | 4.2 | **3.7** | 34.8 | 84.4 | 643.2 | 49.8 |
| `array_sum` | 5.4 | **5.2** | 42.6 | 84.3 | 516.1 | 13.1 |
| `bitops` | **26.9** | 27.5 | 67.9 | 100.2 | 8321.0 | 33.2 |
| `branch_unpredict` | **19.7** | 20.2 | 165.4 | 187.3 | 3399.2 | 21.5 |
| `callvirt` | **3.6** | 3.8 | 43.3 | 77.9 | 2028.7 | 31.0 |
| `callvirt_hot` | 11.3 | **5.7** | 37.0 | 77.3 | 802.2 | 14.9 |
| `cmp_fusion` | **3.6** | 3.6 | — | 79.5 | 2021.2 | 15.9 |
| `fib_recursive` | 6.9 | **6.6** | 41.7 | 86.7 | 289.3 | 13.5 |
| `fp_jit` | 14.2 | **10.9** | 814.0 | 95.0 | 1462.8 | 23.2 |
| `hash_lookup` | **13.1** | 14.6 | 92.6 | 139.3 | 7184.9 | 68.0 |
| `int_mixed` | 20.5 | **19.9** | 56.7 | 95.4 | 11710.6 | 22.0 |
| `intops_jit` | **3.7** | 3.9 | 40.3 | 82.7 | 1185.7 | 8.9 |
| `jit_method` | 4.8 | **4.6** | 39.3 | 86.3 | 1305.5 | 15.2 |
| `mem_class` | **3.6** | 3.8 | 32.1 | 81.1 | 199.0 | 14.9 |
| `mem_malloc_free` | **4.4** | 4.6 | 35.9 | 77.8 | 644.8 | 96.3 |
| `mem_struct` | 3.9 | **3.6** | 34.9 | 83.5 | 464.3 | 21.1 |
| `memcpy_loop` | 8.5 | **6.7** | 37.0 | 109.5 | 3701.9 | 23.0 |
| `nested_loops` | 14.3 | **14.2** | 51.3 | 102.2 | 1827.1 | 25.2 |
| `obj_accum` | **29.1** | 31.3 | 72.3 | 107.6 | 4520.1 | 34.0 |
| `pic_real` | **6.0** | 8.3 | 40.9 | 85.3 | 338.3 | 8.4 |
| `polymorphic` | **10.0** | 10.2 | 60.0 | 89.0 | 1143.5 | 14.5 |
| `quicksort` | **8.0** | 8.5 | 38.0 | 86.3 | 176.6 | 10.9 |
| `rotops_jit` | 4.9 | **4.8** | 35.7 | 85.3 | 1497.8 | 7.6 |
| `state_machine` | 21.6 | **20.8** | 73.6 | 101.4 | 1701.0 | 25.1 |
| `string_hot` | 9.0 | **6.2** | 33.1 | 102.5 | 76.2 | 22.7 |
| `string_workout` | 31.7 | 42.3 | 647.5 | 197.8 | 574.7 | **22.0** |
| `struct_field` | **6.3** | 6.3 | 77.3 | 87.7 | 5488.8 | 20.5 |
| `tight_loop` | 14.3 | **14.0** | 48.4 | 89.4 | 1110.2 | 23.1 |
| `vec_axpy` | **17.8** | 19.1 | 81.4 | 135.5 | 6552.2 | 73.2 |

**Vex JIT C1 vence a HotSpot C2 (Java) en 26 de 28 benches**; Java solo
gana en `fp_jit` y `string_workout`. En geomean global de slowdown vs C,
**Vex JIT está en 6.50× mientras HotSpot está en 10.80×** — VestaVM es
~40% más rápido que Java en promedio y **bate a HotSpot en casi toda la
tabla** para un JIT C1 template-based (sin C2 optimizador todavía).

**Go (gc) es el nuevo referente rápido** de la tabla junto a C/C++: Go
compilado nativo alcanza **2.42× slowdown vs C** y supera al JIT C1 de
Vex en 26 de 28 benches (Vex gana en `alloc` y `mem_malloc_free`). Es
honesto reconocerlo — un compilador AOT maduro como el `gc` de Go queda
por delante de nuestro JIT C1; cerrar ese hueco es trabajo del C2
optimizador y del backend AOT nativo de Vex, ambos en desarrollo.

Casos donde el JIT queda detrás (targets de optimizaciones futuras):

- `fp_jit` (JIT 814 ms == intérprete) — el path float **escalar** no se
  acelera en el JIT actual; la auto-vectorización SSE2/AVX en curso lo cierra.
- `string_workout` (648 ms vs Java 198 ms) — HotSpot tiene
  small-string-optimization; mejora con SSO en StringObject.
- `branch_unpredict` (165 ms vs C 20 ms) — branches genuinamente
  impredecibles; mejora con branch hints del perfil PGO ya recolectado.
- `pic_real` (JIT 41 ms vs C 6 ms) — el JIT C1 no devirtualiza el
  patrón PIC con clases dispersas; el inliner futuro lo cerrará.

**Vex JIT supera a CPython 3.11 en 27 de 28 benches (96%)**. La única
excepción es `string_hot` (CPython tiene refcount + SSO nativos). El
peor caso de Vex sigue siendo dramáticamente mejor que el mejor caso de
Python en hot loops puros (geomean Python: 141× más lento que C).

Detalles + metodología + cómo correr los benchmarks en
[doc/BENCHMARKS.md](./doc/BENCHMARKS.md). Los **29 workloads** multi-lenguaje
viven en `examples_codes_vex/benchmark/<bench>/` con `main.vx`, `main.c`,
`main.cpp`, `main.py`, `Main.java` y `main.go` cada uno. Runner orquestador:
`python tools/bench/run_all_benches.py` (incluye **Go (gc)** con su propio
color; genera `bench_results.json` con `runs_individual`, `stats`
p50/p95/min/max/stddev por lenguaje, y 9 gráficas matplotlib en
`bench_plots/` + reporte HTML navegable).

### Visualización gráfica de resultados

El runner genera un dashboard completo en `bench_plots/index.html` con
9 vistas distintas. Las más representativas:

**Resumen geomean vs C** (`08_geomean_summary.png`): media geométrica
del slowdown vs C nativo por lenguaje. Vex JIT compite directamente
con HotSpot C2 y los supera ligeramente en promedio:

![Geomean vs C](./bench_plots/08_geomean_summary.png)

**Heatmap completo** (`02_heatmap.png`): tiempo wall por bench × lenguaje,
colores verde-amarillo-rojo según velocidad relativa:

![Heatmap](./bench_plots/02_heatmap.png)

**Ratio vs C nativo bench-by-bench** (`07_grouped_ratio.png`): ratio
de slowdown contra C para cada bench, agrupado por lenguaje:

![Ratio vs C](./bench_plots/07_grouped_ratio.png)



Vistas adicionales disponibles en `bench_plots/`:
[`00_system_info.png`](./bench_plots/00_system_info.png) (hardware +
toolchains usados),
[`03_radar_profile.png`](./bench_plots/03_radar_profile.png) (perfil
radar por lenguaje),
[`05_scatter_ratio_vs_c.png`](./bench_plots/05_scatter_ratio_vs_c.png)
(scatter de ratio vs C), y
[`06_ranking_lines.png`](./bench_plots/06_ranking_lines.png) (ranking
por bench mostrando consistencia entre lenguajes).  El runner además
genera una gráfica dedicada por cada bench en
`bench_plots/per_bench/` (no commiteadas, regenerar localmente con
`python tools/bench/run_all_benches.py`).

**Roadmap completo** (hasta JIT C2 con regalloc real + AOT con ejecutables
`.exe` nativos en 3 tiers): [doc/ROADMAP.md](./doc/ROADMAP.md).

---

## Comparativa de features

VestaVM no busca competir con runtimes maduros en velocidad bruta o ecosistema,
sino ofrecer un **ecosistema completo y autocontenido** donde lenguaje, runtime,
distribución y herramientas se diseñan juntos. Comparativa de features clave:

| Feature | VestaVM (Vex) | Java/JVM | Rust | C++ | Python | Go |
|---|:---:|:---:|:---:|:---:|:---:|:---:|
| Tipado estático con inferencia local | ✓ | ✓ | ✓ | ✓ | ✗ | ✓ |
| Pattern matching exhaustivo | ✓ | parcial (21+) | ✓ | parcial (C++26) | parcial | ✗ |
| Borrow checker compile-time | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ |
| Smart pointers con deleters custom | ✓ | ✗ | ✓ | ✓ | ✗ | ✗ |
| Reflexión runtime | ✓ | ✓ | parcial | ✗ | ✓ | ✓ |
| AOP nativo (`@Aspect`/proceed) | ✓ | externa (AspectJ) | ✗ | ✗ | externa | ✗ |
| Genéricos por monomorphización | ✓ | ✗ (erasure) | ✓ | ✓ | ✗ | ✓ |
| Async/await + futures | ✓ | ✓ | ✓ | parcial | ✓ | goroutines |
| Spawn distribuido transparente | ✓ | externa (Akka) | externa | ✗ | externa | ✗ |
| Metaprogramación compile-time | ✓ | ✗ | macros | templates | metaclasses | ✗ |
| FFI compile-time (DLLs en build) | ✓ | ✗ | macros build.rs | ✗ | ✗ | ✗ |
| `static_assert` con condiciones runtime-evaluables | ✓ | ✗ | const fn | ✓ | ✗ | ✗ |
| Captura raw de DSL embebido | ✓ (`expr`) | ✗ | `macro_rules!` | macros texto | ✗ | ✗ |
| Format specs en interpolación | ✓ | ✗ | ✓ | parcial (C++20) | ✓ | ✓ |
| Strings UTF-8/16/32 nativos | ✓ | UTF-16 | UTF-8 | varios | varios | UTF-8 |
| GC generacional preciso + movible (mark-compact) | ✓ | ✓ | ✗ | ✗ | refcount+gc | ✓ |
| JIT integrado | ✓ (C1) | ✓ (C1+C2+Graal) | ✗ | ✗ | parcial (PyPy) | ✗ |
| Bytecode portable | ✓ (`.velb`) | ✓ (.class) | ✗ | ✗ | ✓ (.pyc) | ✗ |
| Ejecutables nativos (3 tiers: con runtime / embebido / sin runtime) | funcional (PE+ELF standalone; 3 tiers en progreso) | parcial (GraalVM) | ✓ (no_std) | ✓ (freestanding) | externa | ✓ |
| Profile-Guided Optimization (PGO) | ✓ (foundation `.vprof`) | ✓ | externa (cargo-pgo) | ✓ (GCC/MSVC) | externa | parcial |
| Inline assembly | parcial (`@Asm` whole-function; `asm{}` inline en progreso) | ✗ | ✓ | ✓ | ✗ | parcial |
| REPL interactivo | ✓ | ✓ (JShell) | ✗ | ✗ | ✓ | ✗ |
| Debugger source-aware integrado | ✓ (TCP) | ✓ | ✓ | ✓ | ✓ | ✓ |
| Diagramas Mermaid del pipeline | ✓ | ✗ | ✗ | ✗ | ✗ | ✗ |
| Zero deps externas | ✓ | JVM | crates | varios | CPython | runtime Go |

### Donde brilla VestaVM

- **Ecosistema autocontenido**: lenguaje + runtime + JIT + distribución + REPL
  + debugger + visualización en un solo binario ~15 MB sin runtime externo.
- **Distribución como ciudadano de primera**: `rspawn(node) { ... }` + `await`
  cross-node es parte del bytecode, no una librería bolt-on.
- **Metaprogramación end-to-end**: macros con captura raw de DSLs, introspección
  zero-overhead, FFI en tiempo de compilación, todo en el mismo lenguaje.
- **Seguridad de memoria moderna**: borrow checker estilo Rust + smart pointers
  + GC para los casos en los que el borrow checker es demasiado restrictivo.
- **Hand-rolled donde importa**: encoder x86-64 propio para el JIT (10× más
  rápido que Keystone), GC propio, scheduler propio, protocolo distribuido
  propio. Sin "impedance mismatch" entre capas.

### Donde NO brilla (todavía)

- **Madurez del ecosistema**: 0 paquetes públicos vs millones en npm/cargo/maven.
- **Velocidad bruta**: el JIT C1 está al 5-10% de la velocidad de C nativo
  optimizado en hot loops (C2 cerrará la brecha cuando llegue).
- **Documentación en inglés**: toda la doc del proyecto está en español ASCII.
- **Plataformas ARM/AArch64**: el JIT solo soporta x86-64 hoy; ARM más adelante.
- **AOT nativo en construcción**: el path core (int/float/structs/POO
  no-virtual/FFI/inline-asm) ya produce ejecutables PE/ELF standalone, pero
  los tres tiers (Full/Embed/Bare) y features managed sobre AOT siguen en
  desarrollo activo. El JIT C1 cubre el corpus por el path vreg de producción;
  las pocas features no cubiertas (excepciones polimórficas, spawn/distrib,
  futures) caen a un path legacy o al intérprete graciosamente.

---

## Documentación

### Para empezar

| Documento | Para qué sirve |
|---|---|
| [QUICKSTART](./doc/QUICKSTART.md) | Instalación + primer programa en 5 minutos |
| [LANGUAGE](./doc/LANGUAGE.md) | Visión general del lenguaje Vex |
| [EXAMPLES](./doc/EXAMPLES.md) | Catálogo curado de ejemplos por tema |
| [ARCHITECTURE](./doc/ARCHITECTURE.md) | Arquitectura interna de la VM |
| [BENCHMARKS](./doc/BENCHMARKS.md) | Performance comparada + metodología |
| [ROADMAP](./doc/ROADMAP.md) | Plan de fases A-H, estado actual |

### Referencia del lenguaje (doc/VMdoc/Vex/)

| Sintaxis y semántica | Modelo de programación |
|---|---|
| [TiposDatos](./doc/VMdoc/Vex/TiposDatos.md) | [OOP](./doc/VMdoc/Vex/OOP.md) |
| [Operadores](./doc/VMdoc/Vex/Operadores.md) | [Generics](./doc/VMdoc/Vex/Generics.md) |
| [ControlFlow](./doc/VMdoc/Vex/ControlFlow.md) | [ReflexionAOP](./doc/VMdoc/Vex/ReflexionAOP.md) |
| [Strings](./doc/VMdoc/Vex/Strings.md) | [Metaprogramacion](./doc/VMdoc/Vex/Metaprogramacion.md) |
| [OptionalResult](./doc/VMdoc/Vex/OptionalResult.md) | [Colecciones](./doc/VMdoc/Vex/Colecciones.md) |
| [Closures](./doc/VMdoc/Vex/Closures.md) | [Excepciones](./doc/VMdoc/Vex/Excepciones.md) |

| Memoria y seguridad | Concurrencia y FFI |
|---|---|
| [SmartPointers](./doc/VMdoc/Vex/SmartPointers.md) | [Async](./doc/VMdoc/Vex/Async.md) |
| [BorrowChecker](./doc/VMdoc/Vex/BorrowChecker.md) | [Sincronizacion](./doc/VMdoc/Vex/Sincronizacion.md) |
| | [FFI](./doc/VMdoc/Vex/FFI.md) |

### Referencia de la VM (doc/VMdoc/)

- [SSA IR](./doc/VMdoc/IR/SSA.md) — formato intermedio + ~15 pasadas de optimización
- [SetInstruccionesVM/](./doc/VMdoc/SetInstruccionesVM/) — 50+ docs por familia
  de opcodes bytecode
- [SUPER_INSTRUCCIONES](./doc/VMdoc/SetInstruccionesVM/SUPER_INSTRUCCIONES.md) —
  opcodes fusionados (alu3, loadz/loadzh, cmpjmp, decjnz, mvtake, etc.)
- [runtime/](./doc/VMdoc/runtime/) — ProcessVM, scheduler, GC, plugins nativos
- [Generics](./doc/VMdoc/Generics/Generics.md), [Debug](./doc/VMdoc/Debug/),
  [Hilos](./doc/VMdoc/Hilos/), [Distribuido](./doc/VMdoc/Distribuido/)

### Herramientas y CLI

- [CLI_REPL](./doc/CLI_REPL.md) — REPL interactivo (edición de línea, historial,
  aliases, scripts)
- [CLI_COMMANDS](./doc/CLI_COMMANDS.md) — referencia completa de comandos
- [CLI_DIST](./doc/CLI_DIST.md) — runtime distribuido desde el REPL
- [CLI_VSH](./doc/CLI_VSH.md) — VestaShellScript (.vsh)

### Proyecto

- [LICENSE](./LICENSE.md) — licencia VMProject
- [CONTRIBUTING](./doc/CONTRIBUTING.md) — cómo contribuir
- [DEPENDENCIES](./doc/DEPENDENCIES.md) — librerías necesarias
- [github_work](./doc/github_work.md) — GitFlow del proyecto

---

## Construir desde código fuente

### Dependencias

| Plataforma | Comando |
|---|---|
| Linux (apt) | `sudo apt install build-essential cmake libssl-dev` |
| Arch Linux | `sudo pacman -S base-devel cmake openssl` |
| macOS | `brew install cmake openssl` |
| Windows | TDM-GCC-64 o MinGW + [precompiled OpenSSL](https://slproweb.com/products/Win32OpenSSL.html) |

Submódulos vendored (Keystone, Capstone, LibPEparse) se clonan automáticamente
con `--recursive`. Cero deps externas adicionales.

### Build

```bash
# Release (recomendado)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Debug (con símbolos + asserts)
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j

# Windows MinGW
cmake -G "MinGW Makefiles" -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Si la versión de CMake instalada es ≥ 4.x y falla por las versiones mínimas de
Keystone, añadir `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`.

Detalles completos (Valgrind, ASan, errores comunes, XMake alternativo) en
[doc/QUICKSTART.md](./doc/QUICKSTART.md).

---

## Casos de uso

- **Investigación**: prototipado rápido de runtimes, optimizaciones IR,
  esquemas de GC, JITs y sistemas distribuidos sin pelearse con LLVM.
- **Servicios distribuidos**: ejecutar workloads que cruzan nodos LAN con
  `rspawn` + `Future<T>` sin RPC manual.
- **Aplicaciones embebidas**: la VM compila a un único binario estático ~15 MB,
  sin dependencias dinámicas en Release.
- **DSLs y generación de código**: el sistema de macros `@Macro` + `expr`
  capture permite implementar mini-lenguajes embebidos (parsers, builders,
  pattern matching custom) cuyo código se reduce a literales en compile-time.
- **Aprendizaje de lenguajes**: el código del frontend Vex (~30K LOC) está
  diseñado para ser legible, con cada feature documentada y comentada
  exhaustivamente en español ASCII.

### Casos de uso: compilación nativa (AOT)

El compilador AOT (`-m aot`) ya produce ejecutables nativos standalone PE/ELF
para el subset core del lenguaje. La arquitectura está preparada para tres
tiers de deployment (en construcción); la **misma fuente** podrá compilar a:

- **Vex Full** (3-5 MB con runtime linkado dinámicamente) — apps managed
  con todo el lenguaje disponible: GC, async, reflexión, distribución.
  Modelo análogo a Go, Java, C#.
- **Vex Embed** (500 KB – 1 MB con mini-runtime embebido estáticamente) —
  CLI tools, ETL, scripts standalone. Subset del lenguaje sin reflexión
  ni distribución pero con clases/strings/closures/excepciones. Modelo
  análogo a Rust con std, Swift, OCaml native.
- **Vex Bare** (50-200 KB sin runtime, solo libc o freestanding) —
  desarrollo de sistemas operativos, drivers de kernel, firmware
  embedded ARM Cortex-M/RISC-V, bootloaders y aplicaciones UEFI,
  hot-path libs distribuidas como `.dll`/`.so` con cero overhead vs
  C++ optimizado. Modelo análogo a Zig minimal, Rust `#![no_std]`,
  C/C++ embedded.

Vex Bare incluirá mecanismos de extensibilidad para que el programador
implemente lo que falta según su caso de uso: `@AllocatorOverride` para
hookear `kmalloc/kfree` del kernel, `@PanicHandler` para reemplazar el
default `fputs+exit` por halt CPU / reboot / log a UART, `@CustomGC`
para implementaciones especializadas (refcount, region-based, Boehm
conservativo), `@StringImpl` para UTF-8 ligero sin GC, `@SyncImpl` para
spinlocks / IRQ-disable / futex según nivel, `@UnwindImpl` para
`setjmp/longjmp` como alternativa a `.pdata/.eh_frame`. Detalle completo
en el plan AOT del roadmap.

---

## Filosofía del proyecto

VestaVM no busca competir con JVM/CLR/Cranelift en madurez ni con C/Rust en
performance bruta. Su valor está en ser un **ecosistema completo y autocontenido**
donde lenguaje, runtime, distribución y herramientas se diseñan juntos:

- **Decisiones cohesionadas**: el GC sabe del JIT, el JIT sabe del bytecode, el
  bytecode sabe del lenguaje. No hay "impedance mismatch".
- **Hand-rolled cuando importa**: encoder x86-64 propio (10× más rápido que
  Keystone para JIT), object emitter PE/COFF/ELF integrado, linker propio.
  Sin dependencias externas en el path crítico.
- **Documentación binding**: cada feature del lenguaje y cada opcode del
  bytecode tienen doc autoritativa en español. 
  referencia para futuras decisiones).
- **Tests no negociables**: 314/314 e2e antes de cada commit. Cada nueva feature
  ships con su test en `tests/vex/`.

---

## Comunidad

- **Repositorio principal**: [github.com/desmonHak/VM](https://github.com/desmonHak/VM)
- **Documentación Obsidian-friendly**: [github.com/desmonHak/VMdoc](https://github.com/desmonHak/VMdoc)
- **Issues y feature requests**: [GitHub Issues](https://github.com/desmonHak/VM/issues)
- **Cómo contribuir**: [doc/CONTRIBUTING.md](./doc/CONTRIBUTING.md)

---

## Licencia

VestaVM se distribuye bajo la **licencia VMProject**. Lee
[LICENSE.md](./LICENSE.md) para los términos completos.

Submódulos con licencias propias:
- **Keystone** y **Capstone**: BSD 3-Clause.
- **LibPEparse**: ver `libs/SourceCode/LibPEparse/LICENSE`.
- **OpenSSL**: Apache 2.0 / OpenSSL License (según versión).
- **nlohmann/json**: MIT.
- **cxxopts**: MIT.
- **FTXUI**: MIT.

---

<div align="center">

**VestaVM** · una máquina virtual diseñada como ecosistema.

Hecho con C++17, GCC y mucho café por [desmonHak](https://github.com/desmonHak).

</div>
