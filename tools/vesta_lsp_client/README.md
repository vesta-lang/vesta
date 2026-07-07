# vesta_lsp_client

Cliente Python sencillo y portable para el **LSP de Vesta** (`vesta_lsp`).

Arranca el servidor `vesta_lsp` como subproceso, hace el handshake JSON-RPC y
ofrece **un metodo por cada peticion** que soporta el servidor: diagnosticos,
hover, completado, definicion/referencias, semantic tokens, y todas las vistas
propias `vesta/*` (IR, bytecode, asm JIT/AOT, diagramas, complejidad Big-O,
reporte multi-modo, macros, valores comptime...).

- **Portable**: solo usa la libreria estandar de Python (`subprocess`, `json`).
  Funciona en **Windows, Linux y macOS**, y en cualquier arquitectura donde
  corra el binario `vesta_lsp` (x86-64, x86-32, ARM, ...).
- **Completo**: cubre los 24 metodos del servidor.
- **Sin dependencias**: no requiere `pip install` de terceros.

> Requiere el binario `vesta_lsp` (se construye con el proyecto VestaVM:
> `cmake --build <build> --target vesta_lsp`).

---

## Estructura

La **libreria** (`vesta_lsp_client/`) es independiente y no depende de los
ejemplos; los **ejemplos** (`examples/`) la consumen.

```text
vesta_lsp_client/     libreria (importable):
  client.py             cliente LSP (VestaLspClient, discover_lsp)
  highlight.py          spans tipados SIN color (iter_token_spans, render)
examples/             scripts que CONSUMEN la libreria:
  basic_usage.py        diagnosticos, hover, completado, funciones, Big-O
  inspect_modes.py      modos, asm JIT/AOT por OS/arch, CFG, diagrama de tipos
  vxcat.py              cat con resaltado ANSI (su propio esquema de color)
setup.py  pyproject.toml  requirements.txt  README.md  LICENSE  .gitignore
```

## Instalacion

Es Python puro (sin dependencias de terceros). Instalable con `pip` o usable
directamente con `PYTHONPATH`.

```bash
# Opcion A: instalar la libreria (importable desde cualquier sitio).
cd tools/vesta_lsp_client
pip install .            # o: pip install -e .  (editable)
python -c "from vesta_lsp_client import VestaLspClient; print('ok')"

# Opcion B: sin instalar, apuntando PYTHONPATH.
export PYTHONPATH=/ruta/a/tools/vesta_lsp_client:$PYTHONPATH

# Ejecutar un ejemplo (consume la libreria):
python examples/basic_usage.py /ruta/a/vesta_lsp
```

Python 3.7 o superior. `pip install .` instala **solo la libreria**; los
ejemplos se ejecutan directamente (`python examples/<script>.py`).

---

## Uso rapido

```python
from vesta_lsp_client import VestaLspClient

# El cliente arranca el servidor y hace el handshake.  Usalo como context
# manager para que se cierre limpio (shutdown + exit).
with VestaLspClient("build/vesta_lsp.exe") as lsp:
    # Abrir un documento: desde disco o pasando el texto directamente.
    uri = lsp.open("programa.vx")
    # ... o:
    uri = lsp.open("scratch.vx", text="i64 main() { return 7; }")

    # Diagnosticos publicados al abrir.
    for d in lsp.diagnostics(uri):
        print(d["range"], d["message"])

    # Hover / info de simbolo (posiciones 0-based).
    print(lsp.hover(uri, line=0, character=4))
    print(lsp.symbol_info(uri, line=0, character=4))

    # Vistas del compilador.
    print(lsp.ir(uri, phase="post"))
    print(lsp.bytecode(uri))
    print(lsp.functions(uri))
    print(lsp.complexity(uri))
```

---

## Compilar y ejecutar

El servidor LSP **embebe el compilador**: `compile()` produce un `.velb` en
disco con el compilador embebido (sin lanzar procesos).  La EJECUCION corre en
un proceso aparte (el binario `vm`, aislado) via :class:`VestaRunner` — nunca
en el proceso del LSP, cuyo stdout es el canal JSON-RPC.

```python
from vesta_lsp_client import VestaLspClient, VestaRunner

with VestaLspClient() as lsp:
    uri = lsp.open("programa.vx")
    res = lsp.compile(uri, output="build/programa", mode="vm")  # -> .velb
    if not res["ok"]:
        for d in res["diagnostics"]:
            print(d["level"], d["line"], d["message"])

# Ejecutar (proceso vm aislado): captura salida + valor de retorno (R00).
runner = VestaRunner()
rr = runner.run("build/programa.velb", want_value=True)
print(rr.program_output)   # salida del programa (sin el volcado de --stats)
print("retorno:", rr.value)
```

Proyectos multi-fichero (resuelve `import`) con `compile_project()`:

```python
res = lsp.compile_project(root_uri, output="build/app")
```

`compile()` acepta `mode` (`"vm"`|`"jit"`), `debug=True` (info de depuracion),
`instrument=...` (instrumentacion), `keep_labels`, `emit_map`.  El compilador
embebido cubre `.velb` (vm/jit); para AOT nativo (`.exe`/`.o`/formatos PE/ELF)
usa `VestaRunner.compile_status(src, mode="aot", output=...)`, que delega en el
binario `vm` (soporta todos los flags).

---

## Multi-modo: interprete / JIT / AOT

El LSP **no asume** el modo de ejecucion del programa. `modes()` reporta los
tres; pasando `mode=` obtienes solo uno.

```python
report = lsp.modes(uri)                 # los tres modos
for m in report["modes"]:
    print(m["mode"], m.get("note"))

lsp.modes(uri, mode="aot", tier="bare") # solo AOT, tier bare
lsp.modes(uri, mode="jit")              # solo JIT
```

Cada entrada trae:

| modo     | campos utiles                                             |
|----------|-----------------------------------------------------------|
| `interp` | `ok`, `errors`, `warnings`                                |
| `jit`    | `compilable_functions`, `fallback_functions`              |
| `aot`    | `tier`, `compatible`, `issues`, `ok_functions`            |

---

## Desensamblado nativo (JIT / AOT) por OS y arquitectura

```python
# JIT (x86-64 host).
print(lsp.jit_asm(uri, function="suma")["text"])

# AOT nativo para un target concreto.
print(lsp.aot_asm(uri, function="suma", os_="windows", arch="x86-64")["text"])
print(lsp.aot_asm(uri, function="suma", os_="linux",   arch="x86-32")["text"])

# Compatibilidad AOT del modulo a un tier.
print(lsp.aot_compat(uri, tier="bare"))
```

---

## Diagramas

`diagram()` produce **mermaid**, **graphviz** o **html** de varias vistas:

```python
# Estructura y flujo.
lsp.diagram(uri, kind="ast",     fmt="mermaid")
lsp.diagram(uri, kind="ir-post", fmt="graphviz", cost=True)
lsp.diagram(uri, kind="vel",     fmt="mermaid")

# POO del modulo (classDiagram: clases, herencia, interfaces, structs, enums).
lsp.diagram(uri, kind="types",   fmt="mermaid")

# CFG del codigo maquina de una funcion.
lsp.diagram(uri, kind="asm",     fmt="mermaid", function="clasifica")
```

`kind`: `ast` · `ir-pre` · `ir-post` · `vel` · `types` · `asm`
`fmt` : `mermaid` · `graphviz` · `html`

---

## Semantic tokens y resaltado

El servidor devuelve los tokens como un array plano de deltas;
`decode_semantic_tokens` los pasa a posiciones absolutas.

```python
from vesta_lsp_client import decode_semantic_tokens

data = lsp.semantic_tokens(uri)["data"]        # array plano de deltas
legend = lsp.semantic_token_legend()           # indice de tipo -> nombre
for line, col, length, ttype, mods in decode_semantic_tokens(data):
    print(line, col, length, legend[ttype])
```

Para pintar el fuente, la libreria da los *spans* tipados **sin imponer
colores** (cada programa elige su esquema y formato: ANSI, HTML, tags de
editor...). `render` envuelve cada fragmento con el prefijo/sufijo que
devuelva tu funcion de estilo:

```python
from vesta_lsp_client import render, iter_token_spans

# 1) Iterar spans y renderizar como quieras.
for fragment, type_name in iter_token_spans(text, tokens, legend):
    ...  # type_name == "" en los huecos sin token

# 2) O usar render con tu propio estilo (aqui, ANSI en terminal).
ANSI = {"keyword": "\033[35m", "string": "\033[32m", "comment": "\033[90m"}
coloreado = render(text, tokens, legend,
                   lambda n: (ANSI.get(n, ""), "\033[0m" if n in ANSI else ""))

# Salida HTML con el MISMO api (otro consumidor, otro estilo):
html = render(text, tokens, legend,
              lambda n: (f"<span class='{n}'>", "</span>") if n else ("", ""))
```

Ver `examples/vxcat.py` para un `cat` completo con su propio esquema de color.

---

## Metaprogramacion

```python
lsp.macro_expand(uri)      # codigo que generan los @Macro
lsp.comptime_values(uri)   # consts + sizeof/kind/... evaluados en compilacion
lsp.param_hints(uri)       # inlay hints con el nombre de cada parametro
```

---

## Referencia de la API

Constructor:

```python
VestaLspClient(lsp_path, *, root_uri=None, timeout=30.0)
```

- `lsp_path`: ruta al ejecutable `vesta_lsp`.
- `root_uri`: raiz del workspace (para resolver `import "..."` multi-fichero).
- `timeout`: segundos maximos por lectura (`None` = sin limite).

Documentos:

| metodo | descripcion |
|--------|-------------|
| `open(path_or_uri, *, text=None, language_id="vx", version=1) -> uri` | Abre y devuelve el URI; captura los diagnosticos. |
| `change(uri, text, *, version=2)` | Reemplaza el contenido (`didChange`). |
| `close_document(uri)` | Cierra el documento (`didClose`). |
| `diagnostics(uri) -> list` | Diagnosticos de la ultima publicacion. |

Peticiones (todas devuelven el JSON ya parseado):

| metodo | LSP / vesta |
|--------|-------------|
| `hover`, `completion`, `definition`, `references`, `semantic_tokens` | estandar |
| `symbol_info`, `param_hints` | `vesta/symbolInfo`, `vesta/paramHints` |
| `ir`, `ir_diff`, `bytecode` | `vesta/ir`, `vesta/irDiff`, `vesta/bytecode` |
| `functions`, `complexity` | `vesta/functions`, `vesta/complexity` |
| `diagram` | `vesta/diagram` |
| `jit_asm`, `aot_asm`, `aot_compat` | `vesta/jitAsm`, `vesta/aotAsm`, `vesta/aotCompat` |
| `modes` | `vesta/modes` |
| `compile`, `compile_project` | `vesta/compile`, `vesta/compileProject` (compilador embebido) |
| `macro_expand`, `comptime_values` | `vesta/macroExpand`, `vesta/comptimeValues` |
| `request(method, params)` | escotilla generica para cualquier metodo |

Ejecucion (modulo `runtime`, no LSP): `VestaRunner(vm_path=None)` con
`run(velb, *, args, mode, schedulers, want_value)` -> `RunResult(exit_code,
stdout, stderr, value, program_output)`; `discover_vm()` auto-detecta el binario
`vm`.

Errores: cualquier respuesta con `error` del servidor lanza
`vesta_lsp_client.LspError`.

---

## Ejemplos incluidos

Todos auto-detectan los binarios (o pasa `--lsp` / `--vm`) y toman un fichero
opcional; su salida es coloreada y con resaltado de sintaxis.

| fichero | que muestra |
|---------|-------------|
| `examples/basic_usage.py` | diagnosticos, funciones, Big-O, completado (coloreado) |
| `examples/inspect_modes.py` | modos, asm JIT/AOT por OS/arch, CFG y diagrama de tipos |
| `examples/vxcat.py` | `cat` con resaltado de sintaxis y numeros de linea |
| `examples/compile_and_run.py` | resalta el fuente, compila (embebido) y ejecuta |
| `examples/project_build.py` | compila y ejecuta un proyecto multi-modulo |

```bash
python examples/basic_usage.py                 # demo incrustada
python examples/vxcat.py         programa.vx    # auto-detecta vesta_lsp
python examples/compile_and_run.py programa.vx  # compila + ejecuta
python examples/project_build.py raiz.vx        # proyecto (o demo)
```

> El color se activa si la salida es una terminal; forzarlo con
> `VESTA_FORCE_COLOR=1` (util al canalizar a un pager).

```bash
python examples/basic_usage.py   /ruta/a/vesta_lsp
python examples/inspect_modes.py /ruta/a/vesta_lsp
```

---

## Licencia

MIT. Ver [LICENSE](LICENSE).
