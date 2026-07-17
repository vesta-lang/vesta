# Base de datos de instrucciones

Herramientas que construyen la base de datos de instrucciones de Vesta a partir
de fuentes microarquitecturales externas.  La base de datos alimenta al
compilador consciente del ensamblador (efectos y contratos de `asm { }`), al
modelo de coste/scheduling y al hover del LSP: para cada forma de instruccion
describe su **sintaxis** (operandos, encoding), su **semantica** (efectos,
barreras, atomicidad) y su **coste por microarquitectura** (latencias,
throughput, uops, puertos).

El compilador **nunca** lee el XML de la fuente: consume los ficheros propios
que genera este pipeline (`.vxisa`, `.vxarch`, `instr_form_ids.h`).

---

## Arquitectura del pipeline

```text
XML (uops.info)  ──uops_info──►  IR  ──optimize──►  IR optimizado  ──serialize──►  ficheros
                   (importa)          (transforma)                    (escribe)
                          build_database.py orquesta
```

Cada modulo tiene una unica responsabilidad:

| Modulo                       | Responsabilidad |
| :--------------------------- | :-------------- |
| `ir.py`                      | El IR neutral, independiente de la fuente (una `InstrForm` por forma, `RawSchedule`/`SchedulerClass` para el coste, `EncodingFeatures`, ...).  Define la identidad (`form_key`) y su comprobacion (`checksum`). |
| `uops_info.py`               | **Importa**: traduce `instructions.xml` de uops.info al IR.  No asigna identificadores ni optimiza. |
| `optimize.py`                | **Transforma**: asigna el `FormID`, normaliza los puertos y deduplica el coste en clases de scheduling. |
| `serialize.py`               | **Escribe**: emite los ficheros de la base de datos.  Fase pura, sin logica. |
| `build_database.py`          | **Orquesta**: encadena importar -> optimizar -> serializar y valida el overlay. |
| `database.py`                | Lector unico de `.vxisa`/`.vxarch` (usado por `dump_db.py`, `dump_html.py`, tests y verificadores). |
| `dump_db.py`                 | Inspeccion legible (texto) de la base de datos generada. |
| `dump_html.py`               | Genera el sitio visual (multipagina) desde las plantillas de `templates/`: tabla de instrucciones + analizador de asm + assets. |
| `overlay_x86_semantics.def`  | Semantica manual que la fuente no codifica (serializante, barrera, atomica, ...). |

Anadir una fuente nueva (guias de ARM, modelos de LLVM) es escribir otro
importador que produzca el mismo IR: `optimize`, `serialize` y el compilador no
cambian.

---

## Requisitos

- **Python 3.7+** (solo la libreria estandar; sin dependencias externas).
- El fichero **`instructions.xml`** de [uops.info](https://uops.info/) (~140 MB).
  No se versiona en el repositorio; descargalo aparte.

No hay que instalar nada: los scripts se ejecutan directamente con el interprete.

---

## Como ejecutar los scripts

Solo **dos** ficheros son ejecutables (tienen punto de entrada); el resto son
modulos de libreria que esos dos importan y **no se ejecutan sueltos**.

| Fichero               | Ejecutable | Como se usa |
| :-------------------- | :--------: | :---------- |
| `build_database.py`   | **Si**     | Genera la base de datos (ver abajo). |
| `dump_db.py`          | **Si**     | Inspecciona la base de datos (texto). |
| `dump_html.py`        | **Si**     | Genera el volcado visual (HTML autocontenido). |
| `ir.py`               | No         | Modulo de libreria (el IR).  Se importa. |
| `uops_info.py`        | No         | Modulo de libreria (el importador). |
| `optimize.py`         | No         | Modulo de libreria (la transformacion). |
| `serialize.py`        | No         | Modulo de libreria (la escritura). |
| `database.py`         | No         | Modulo de libreria (el lector). |

Los comandos de este README se lanzan **desde la raiz del repositorio** (las
rutas de ejemplo son relativas a ella).  El interprete se invoca segun la
plataforma:

```bash
# Windows (PowerShell o Git Bash)
python tools/import/build_database.py <instructions.xml> timings/x86 tools/import/overlay_x86_semantics.def

# Linux / macOS
python3 tools/import/build_database.py <instructions.xml> timings/x86 tools/import/overlay_x86_semantics.def
```

Los dos ejecutables resuelven ellos mismos donde estan sus modulos vecinos, asi
que se pueden lanzar desde cualquier directorio siempre que se den las rutas
correctas a los ficheros de entrada/salida.  Ejecutar un modulo de libreria
directamente (`python tools/import/ir.py`) no hace nada: no tienen `main`.

Sin argumentos, cada ejecutable imprime su modo de empleo:

```bash
python tools/import/build_database.py
# uso: python build_database.py <instructions.xml> <dir_salida> [overlay.def]

python tools/import/dump_db.py
# uso: python dump_db.py <dir_db> <consulta> [--limit N] [--debug-key]
```

---

## Generar la base de datos

```bash
python tools/import/build_database.py <instructions.xml> <dir_salida> [overlay.def]
```

Ejemplo:

```bash
python tools/import/build_database.py \
    ruta/a/instructions.xml \
    timings/x86 \
    tools/import/overlay_x86_semantics.def
```

Produce en `<dir_salida>`:

| Fichero                     | Contenido |
| :-------------------------- | :-------- |
| `x86.vxisa`                 | Sintaxis por ISA: una fila por forma (operandos completos, encoding, efectos, overlay). |
| `intel-skylake.vxarch`      | Modelo de coste de una microarquitectura (clases de scheduling + mapeo forma->clase). |
| `intel-alderlake-p.vxarch`  | idem. |
| `amd-zen4.vxarch`           | idem. |
| `instr_form_ids.h`          | Cabecera C: `enum InstrFormID`, `kInstrFormCount` y `kInstrChecksum[]`. |

La salida esta ignorada en git: es regenerable desde el XML.

Al terminar imprime un informe (formas unicas, operandos implicitos, filas
duplicadas exactas, clases por microarquitectura).

---

## Inspeccionar la base de datos

```bash
python tools/import/dump_db.py <dir_db> <consulta> [--limit N] [--debug-key]
```

`<consulta>` es una subcadena del nombre reconstruido de la forma.  Opciones:

- `--limit N` : maximo de formas a mostrar (`0` = todas).  Por defecto 20.
- `--debug-key` : imprime ademas la **clave estructural** (util para depurar por
  que dos formas comparten o difieren en su `FormID`).

Ejemplos:

```bash
# una forma concreta, con su clave estructural
python tools/import/dump_db.py timings/x86 ADD_GPRv_GPRv_01/64x64 --debug-key

# todas las variantes de DIV
python tools/import/dump_db.py timings/x86 DIV_GPR --limit 0
```

Salida (abreviada) para `ADD reg,reg` en 64 bits:

```text
form: ADD_GPRv_GPRv_01/64x64   (id=340  checksum=7d5b5e8e085c1d4c)
  iclass=ADD ext=BASE opcode=01 enc=isa_set=I86,eosz=3
  efectos: rmask=0x3 wmask=0x1 mem=0 imm=0 wflags=1 rflags=0 overlay=-
  operandos:
    op0 reg w64 [r|w] {RAX/RCX/...}
    op1 reg w64 [r]   {RAX/RCX/...}
    op2 flags w0 [w|impl|supp]
  timing intel-skylake       : recip_tp=0.25 uops=1 macro_fusible
      latencias: 0:0:0:1.00,0:2:0:1.00,1:0:0:1.00,1:2:0:1.00
      puertos  : 1.00xp0156
```

---

## Identidad de una forma (`FormID`)

Cada forma de instruccion tiene un **`FormID`**: un indice denso (`0..N-1`) que
el compilador usa como indice de array.  El `FormID` se asigna por el **orden
lexicografico de la clave estructural** (`ir.form_key`), no por un hash ni por el
nombre de la fuente.

La clave estructural se compone de:

```text
SEMANTIC_SCHEMA  +  iclass  +  extension  +  opcode  +  EncodingFeatures  +  operandos (con register_set)
```

y se serializa a bytes canonicos (codificacion propia con longitud prefijada,
independiente de detalles internos de Python).  Consecuencias de diseno:

- **Independiente del nombre.**  uops.info renombra las formas entre versiones
  (`ADD_GPRv_GPRv` -> `ADD_GPRv_GPRv_01`); como la clave no usa el nombre, un
  renombrado **no** cambia la identidad.  El nombre (`iform` / `uid`) es
  documentacion: da el identificador humano del `enum` y aparece en el LSP.
- **Identifica el encoding, no la semantica pura.**  El opcode y el conjunto de
  registros entran en la clave: `ADD 01/r` y `03/r` son formas distintas (no se
  pierde el encoding); `AL` y un `GPR8` cualquiera son distintos.
- **Robusta a atributos nuevos.**  Toda caracteristica de encoding relevante
  (EVEX, mascara, broadcast, rounding, `rep`, `lock`, ...) forma parte de la
  clave via `EncodingFeatures`.

`kInstrChecksum[]` (FNV1a-64 de la clave) se emite **solo como verificacion**:
permite comparar dos versiones de la base de datos o detectar que la semantica de
una forma cambio.  No es el identificador.

`kInstrSemanticSchema` versiona **que** entra en la clave.  Si algun dia una
caracteristica deja de contar para la identidad, se sube el esquema para no
reutilizar identificadores antiguos.

---

## El overlay semantico

uops.info da sintaxis y coste, pero no si una instruccion **serializa**, es una
**barrera** de memoria, es **atomica**, **salta**, **llama** o **falla**.  Eso es
conocimiento manual, pequeno y estable, que vive en
`overlay_x86_semantics.def` y se fusiona sobre la forma correspondiente.

Formato (la forma en su propia linea; las propiedades indentadas):

```text
CPUID
    serializing
    no_reorder

MFENCE
    barrier
    mem_seq_cst
```

Propiedades validas: `serializing`, `barrier`, `atomic`, `ll_sc`, `branch`,
`call`, `ret`, `syscall`, `stack_push`, `stack_pop`, `mem_acquire`,
`mem_release`, `mem_seq_cst`, `may_fault`, `privileged`, `no_reorder`.

El overlay se **valida** al construir la base de datos y **aborta** si:

- la forma referenciada no existe en el XML;
- una propiedad no esta en la lista de validas (p. ej. un error tipografico);
- una entrada mezcla propiedades incompatibles (`call`+`ret`, `atomic`+`ll_sc`,
  `stack_push`+`stack_pop`, `mem_acquire`+`mem_release`).

---

## Formato de los ficheros

Hoy los ficheros son **texto** (legibles y depurables).  Un formato binario
(lectura directa en el runtime) es un paso posterior; cuando llegue, solo cambia
`database.py` y los consumidores no se enteran.

### `x86.vxisa`

Cabecera:

```text
vxisa 1 schema=1 isa=x86 source=uops.info date=<fecha> forms=<N> xml_sha256=<hash>
```

Una fila por forma (separador `|`):

```text
id | checksum | uid | iclass | ext | opcode | enc | rmask | wmask | mem | imm | wflags | rflags | operands | overlay
```

- `id` : el `FormID` (indice denso).
- `checksum` : FNV1a-64 de la clave (verificacion).
- `uid` : nombre humano reconstruido (documentacion).
- `enc` : caracteristicas de encoding presentes, `clave=valor` separadas por coma.
- `rmask`/`wmask` : bitmask de operandos leidos/escritos (bit *i* = operando *i*).
- `operands` : `idx,kind,width,flags,regset` por operando, separados por `;`.
  `flags = r | w<<1 | implicit<<2 | suppressed<<3`.

### `<microarquitectura>.vxarch`

Cabecera:

```text
vxarch 1 name=<nombre> family=<intel|amd|arm> isa=<x86|arm> source=<fuente> src_arch=<nombre en la fuente> date=<fecha> xml_sha256=<hash> classes=<C> mapped=<M>
```

Leyenda de puertos (indice -> nombre del grupo de puertos):

```text
ports: 0=p0 1=p1 2=p06 ...
```

Clases de scheduling (formas que comparten coste comparten clase):

```text
class <cid> | recip_tp | uops | microcoded | macro_fusible | div_cycles | latencias | puertos
```

- `latencias` : `so:to:kind:ciclos[:ub]` por arista, separadas por coma.
  `so`/`to` son indices de operando; `kind` es `0`=result, `1`=address,
  `2`=flags, `3`=memory; `:ub` marca cota superior.
- `puertos` : `indice*uops` por grupo, separados por coma (el indice remite a la
  leyenda `ports:`).

Mapeo forma -> clase (una linea por forma con coste en esta microarquitectura):

```text
<form_id> | <class_id>
```

### `instr_form_ids.h`

Cabecera C generada:

```c
constexpr uint32_t kInstrSemanticSchema = 1;

enum InstrFormID : uint32_t {
    IFORM_ADD_GPRV_GPRV_01_64X64 = 340,
    // ...
};

constexpr uint32_t kInstrFormCount = 22252;
constexpr uint64_t kInstrChecksum[kInstrFormCount] = { /* ... */ };
```

---

## Extender el sistema

### Anadir una microarquitectura

Edita `MICROARCHS` en `build_database.py`:

```python
MICROARCHS = [
    ir.MicroArchSpec("SKL", "intel-skylake", "intel"),
    ir.MicroArchSpec("<nombre-en-uops>", "<nombre-canonico>", "<familia>"),
    # ...
]
```

Usa el nombre **real** de la microarquitectura en la fuente (no uno aproximado).

### El importador aborta con "atributos DESCONOCIDOS"

Es **deliberado**: el importador valida el esquema del XML de forma estricta y
falla si aparece un atributo que no reconoce, en lugar de ignorarlo en silencio.
uops.info evoluciona, y un campo nuevo debe ser una decision consciente.

Cuando ocurra:

1. Averigua que significa el atributo nuevo.
2. Si afecta al **encoding** (y por tanto a la identidad de la forma), anadelo a
   `ENCODING_MAP` en `uops_info.py` y como campo de `EncodingFeatures` en
   `ir.py`.  Sube `SEMANTIC_SCHEMA` si cambia la identidad de formas existentes.
3. Si es **documentacion** o de **medicion** que no afecta a la identidad,
   anadelo al conjunto conocido correspondiente (`_INSTR_IGNORED`,
   `_OPERAND_KNOWN`, `_LAT_BASE`, `_MEAS_BASE`).

### Reutilizar el lector desde otra herramienta

Usa `database.py` en lugar de parsear los ficheros a mano:

```python
import database
forms = database.load_vxisa("timings/x86/x86.vxisa")
name, ports, classes, form_class = database.load_vxarch(
    "timings/x86/intel-skylake.vxarch")
```

Devuelve diccionarios con claves nombradas: anadir una columna al formato no
rompe a los lectores.

---

## Convenciones

- Los scripts de automatizacion van en **Python** (nunca `.sh`).
- Comentarios y documentacion en espanol, ASCII salvo la letra ene.
