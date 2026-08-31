#!/usr/bin/env python3
"""Comprueba contra el servidor REAL lo que la extension da por supuesto.

La extension habla con `vesta_lsp` y lee campos concretos de sus respuestas
(`asm_lines`, `ir_listing`, `ir_by_id`, `functions`, `hints`, ...).  Si el
servidor renombra uno, el editor no falla: simplemente deja de mostrar algo, y
eso es peor que un error.  Este script fija esas suposiciones por escrito y
avisa en cuanto una deja de cumplirse.

Reutiliza la libreria cliente que ya vive en `tools/vesta_lsp_client`, para no
tener dos implementaciones del mismo protocolo en el mismo repositorio.

Uso:
    python test/smoke_lsp.py [--lsp RUTA] [--file EJEMPLO.vx]

Sin argumentos localiza el servidor solo y usa un ejemplo del repositorio.
Devuelve 0 si todas las comprobaciones pasan.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import tempfile
import time
from typing import Any, Callable, Dict, List, Optional, Tuple

# La libreria cliente vive fuera de la extension; se anade su carpeta al path
# en lugar de instalarla, para que el script corra sobre un clon recien hecho.
_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.abspath(os.path.join(_HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(_REPO, "tools", "vesta_lsp_client"))

try:
    from vesta_lsp_client import VestaLspClient, discover_lsp  # type: ignore
except ImportError as exc:  # pragma: no cover - solo si falta el repositorio
    print(f"no se pudo importar la libreria cliente: {exc}")
    print(f"se buscaba en {os.path.join(_REPO, 'tools', 'vesta_lsp_client')}")
    sys.exit(2)


# Metodos propios que la extension invoca; deben seguir anunciandose.
METODOS_ESPERADOS = [
    "vesta/ir",
    "vesta/bytecode",
    "vesta/complexity",
    "vesta/diagram",
    "vesta/functions",
    "vesta/aotCompat",
    "vesta/jitAsm",
    "vesta/aotAsm",
    "vesta/modes",
    "vesta/compile",
]

# Tipos de resaltado que la extension declara como propios en su manifiesto.
TIPOS_PROPIOS = ["escapeSequence", "interpolation", "register"]


class Comprobaciones:
    """Lleva la cuenta de lo comprobado y de lo que ha fallado."""

    def __init__(self) -> None:
        self.pasadas = 0
        self.fallos: List[str] = []

    def exigir(self, condicion: bool, descripcion: str, detalle: str = "") -> bool:
        """Registra una comprobacion.

        :param condicion: Resultado de la comprobacion.
        :param descripcion: Que se estaba comprobando.
        :param detalle: Informacion extra que ayuda a entender un fallo.
        :returns: La propia condicion, para poder encadenar.
        """
        if condicion:
            self.pasadas += 1
            print(f"  ok    {descripcion}")
        else:
            mensaje = descripcion if not detalle else f"{descripcion} -- {detalle}"
            self.fallos.append(mensaje)
            print(f"  FALLA {mensaje}")
        return condicion


def campos(objeto: Any) -> str:
    """Describe las claves de un objeto, para los mensajes de fallo."""
    if isinstance(objeto, dict):
        return "claves: " + ", ".join(sorted(objeto.keys()))
    return f"tipo inesperado: {type(objeto).__name__}"


def ejemplo_por_defecto() -> Optional[str]:
    """Busca un ejemplo del repositorio con el que hacer las pruebas."""
    directorio = os.path.join(_REPO, "examples_codes_vx")
    if not os.path.isdir(directorio):
        return None
    # Se prefiere un ejemplo sin dependencias externas para que el analisis no
    # dependa de tener la biblioteca instalada.
    preferidos = ["01_hello.vx", "02_fib.vx", "1_hello.vx"]
    for nombre in preferidos:
        ruta = os.path.join(directorio, nombre)
        if os.path.isfile(ruta):
            return ruta
    for nombre in sorted(os.listdir(directorio)):
        if nombre.endswith(".vx"):
            return os.path.join(directorio, nombre)
    return None


def comprobar_capacidades(lsp: VestaLspClient, c: Comprobaciones) -> None:
    """Comprueba lo que el servidor anuncia en el saludo inicial."""
    print("\n[capacidades anunciadas]")
    capacidades = lsp.capabilities if hasattr(lsp, "capabilities") else {}
    if not capacidades:
        # La libreria guarda la leyenda; el resto se consulta por su metodo.
        leyenda = lsp.semantic_token_legend()
        c.exigir(bool(leyenda), "el servidor anuncia una leyenda de resaltado")
        for tipo in TIPOS_PROPIOS:
            c.exigir(
                tipo in leyenda,
                f"la leyenda incluye el tipo propio '{tipo}'",
                "el manifiesto de la extension lo declara y quedaria sin color",
            )
        return

    leyenda = (
        capacidades.get("semanticTokensProvider", {})
        .get("legend", {})
        .get("tokenTypes", [])
    )
    for tipo in TIPOS_PROPIOS:
        c.exigir(tipo in leyenda, f"la leyenda incluye el tipo propio '{tipo}'")
    for capacidad in ("hoverProvider", "definitionProvider", "referencesProvider",
                      "documentFormattingProvider"):
        c.exigir(bool(capacidades.get(capacidad)), f"anuncia {capacidad}")
    c.exigir(
        "completionProvider" in capacidades,
        "anuncia completionProvider",
    )
    anunciados = capacidades.get("experimental", {}).get("vestaMethods", [])
    for metodo in METODOS_ESPERADOS:
        c.exigir(metodo in anunciados, f"anuncia el metodo propio {metodo}")


def comprobar_formato(lsp: VestaLspClient, c: Comprobaciones) -> None:
    """Comprueba que el servidor da formato de verdad, no solo que lo anuncia.

    Es lo que hace `editor.formatOnSave` cada vez que se guarda un `.vx`, asi
    que un fallo aqui se nota en cada guardado.  Se manda un fichero SUCIO a
    proposito y se mira que vuelva puesto: anunciar la capacidad y devolver una
    lista vacia seria un fallo silencioso.
    """
    print("\n[formato]")
    sucio = "i32  f( )  {\ni32 a=1;\ni32 bb=2;\nreturn a+bb;\n}\n"
    uri = lsp.open("file:///smoke_formato.vx", text=sucio)
    try:
        ediciones = lsp.request(
            "textDocument/formatting",
            {"textDocument": {"uri": uri},
             "options": {"tabSize": 4, "insertSpaces": False}},
        )
    except Exception as e:  # noqa: BLE001 -- el motivo se cuenta, no se traga
        c.exigir(False, "el servidor responde a textDocument/formatting", str(e))
        return
    c.exigir(bool(ediciones), "devuelve alguna edicion para un fichero sucio")
    if not ediciones:
        return
    texto = ediciones[0].get("newText", "")
    c.exigir("i32 f() {" in texto, "quita el espacio de mas en la firma")
    c.exigir("	i32 a  = 1;" in texto, "indenta con tabulador y alinea el `=`")
    c.exigir("return a + bb;" in texto, "separa los operadores")


def comprobar_navegacion(lsp: VestaLspClient, uri: str, texto: str,
                         c: Comprobaciones) -> None:
    """Comprueba el resaltado y la navegacion sobre un simbolo real."""
    print("\n[resaltado y navegacion]")

    tokens = lsp.semantic_tokens(uri)
    datos = tokens.get("data") if isinstance(tokens, dict) else None
    c.exigir(
        isinstance(datos, list) and len(datos) % 5 == 0,
        "los tokens llegan como quintetos",
        campos(tokens),
    )

    # Se busca una funcion declarada para pedir su definicion en su propio uso.
    objetivo: Optional[Tuple[int, int, str]] = None
    for numero, linea in enumerate(texto.splitlines()):
        recortada = linea.strip()
        if recortada.startswith("//") or "(" not in recortada:
            continue
        antes = recortada.split("(", 1)[0].split()
        if len(antes) >= 2 and antes[-1].isidentifier():
            objetivo = (numero, linea.index(antes[-1]), antes[-1])
            break

    if objetivo is None:
        print("  (no se encontro ninguna funcion en el ejemplo; se omite)")
        return

    linea, columna, nombre = objetivo
    definicion = lsp.definition(uri, linea, columna)
    c.exigir(
        definicion is not None,
        f"ir a la definicion responde sobre '{nombre}'",
        "el menu contextual del editor depende de esto",
    )

    hover = lsp.hover(uri, linea, columna)
    c.exigir(hover is not None, f"el hover responde sobre '{nombre}'")


def comprobar_vistas(lsp: VestaLspClient, uri: str, c: Comprobaciones) -> None:
    """Comprueba las vistas del compilador que abre la extension."""
    print("\n[vistas del compilador]")

    funciones = lsp.functions(uri)
    lista = funciones.get("functions") if isinstance(funciones, dict) else None
    c.exigir(
        isinstance(lista, list),
        "vesta/functions devuelve 'functions'",
        campos(funciones),
    )
    nombres = [f.get("name", "") for f in lista or []]
    if lista:
        c.exigir(
            all("name" in f and "line" in f for f in lista),
            "cada funcion trae 'name' y 'line'",
        )

    ir = lsp.ir(uri, "post")
    c.exigir(
        isinstance(ir, dict) and ("text" in ir or "error" in ir),
        "vesta/ir devuelve 'text'",
        campos(ir),
    )

    bytecode = lsp.bytecode(uri)
    c.exigir(
        isinstance(bytecode, dict) and ("text" in bytecode or "error" in bytecode),
        "vesta/bytecode devuelve 'text'",
        campos(bytecode),
    )

    coste = lsp.complexity(uri)
    entradas = coste.get("functions") if isinstance(coste, dict) else None
    c.exigir(
        isinstance(entradas, list),
        "vesta/complexity devuelve 'functions'",
        campos(coste),
    )
    if entradas:
        esperados = {"name", "partial", "total", "confidence", "total_confidence",
                     "max_loop_depth", "recursive"}
        faltan = esperados - set(entradas[0].keys())
        c.exigir(not faltan, "cada coste trae los campos que se muestran",
                 f"faltan: {sorted(faltan)}")

    modos = lsp.modes(uri)
    c.exigir(
        isinstance(modos, dict) and isinstance(modos.get("modes"), list),
        "vesta/modes devuelve 'modes'",
        campos(modos),
    )

    hints = lsp.param_hints(uri)
    lista_hints = hints.get("hints") if isinstance(hints, dict) else None
    c.exigir(
        isinstance(lista_hints, list),
        "vesta/paramHints devuelve 'hints'",
        campos(hints),
    )
    if lista_hints:
        primera = lista_hints[0]
        c.exigir(
            {"line", "character", "label"} <= set(primera.keys()),
            "cada pista trae linea, columna y etiqueta",
            campos(primera),
        )

    return nombres


def comprobar_correlacion(lsp: VestaLspClient, uri: str, funcion: str,
                          c: Comprobaciones) -> None:
    """Comprueba los datos de la vista correlacionada fuente / IR / maquina."""
    print("\n[vista correlacionada]")

    respuesta = lsp.jit_asm(uri, funcion)
    if not isinstance(respuesta, dict):
        c.exigir(False, "vesta/jitAsm devuelve un objeto", campos(respuesta))
        return

    if respuesta.get("error"):
        c.exigir(False, "vesta/jitAsm compila la funcion", respuesta["error"])
        return
    if respuesta.get("unsupported"):
        print(f"  (el generador no cubre '{funcion}': {respuesta.get('reason', '')})")
        return

    for campo in ("asm_lines", "source", "ir_listing", "ir_by_id", "function",
                  "bytes", "instructions"):
        c.exigir(campo in respuesta, f"vesta/jitAsm devuelve '{campo}'",
                 campos(respuesta))

    filas = respuesta.get("asm_lines") or []
    if filas:
        primera = filas[0]
        c.exigir(
            {"addr", "text", "line"} <= set(primera.keys()),
            "cada instruccion trae direccion, texto y linea",
            campos(primera),
        )
        con_linea = [f for f in filas if f.get("line", 0) > 0]
        c.exigir(
            len(con_linea) > 0,
            "al menos una instruccion apunta a su linea fuente",
            "sin esto la correlacion de la vista no marca nada",
        )
        con_id = [f for f in filas if f.get("ir_id") is not None]
        c.exigir(len(con_id) > 0, "al menos una instruccion trae su 'ir_id'")

    listado = respuesta.get("ir_listing") or []
    if listado:
        c.exigir(
            {"kind", "line", "text"} <= set(listado[0].keys()),
            "cada fila del IR trae tipo, linea y texto",
            campos(listado[0]),
        )
        c.exigir(
            all(f.get("kind") in ("label", "op") for f in listado),
            "el tipo de cada fila del IR es etiqueta u operacion",
        )


def comprobar_navegacion_cross_module(c: Comprobaciones, binario: str) -> None:
    """Comprueba que ir a la definicion salta a un modulo importado.

    Es la ruta que lleva a la biblioteca estandar, y la que devolvia una
    direccion mal formada (`file://F:/...`, con dos barras: ahi la letra de
    unidad se lee como nombre de maquina y el editor no abre nada).  Se exige
    forma absoluta y con las tres barras.
    """
    print("\n[navegacion a un modulo importado]")

    ejemplo = os.path.join(_REPO, "examples_codes_vx", "238_real_threads.vx")
    if not os.path.isfile(ejemplo):
        print("  (no esta el ejemplo con imports; se omite)")
        return

    texto = open(ejemplo, "r", encoding="utf-8").read()
    lineas = texto.splitlines()

    # Un uso de un simbolo que viene de otro modulo (no la linea del import).
    objetivo = None
    for numero, linea in enumerate(lineas):
        recortada = linea.strip()
        if recortada.startswith(("import", "//")):
            continue
        if "vx_lock(" in linea:
            objetivo = (numero, linea.index("vx_lock") + 2)
            break
    if objetivo is None:
        print("  (no se encontro ningun uso de un simbolo importado; se omite)")
        return

    with VestaLspClient(binario, root_uri=os.path.dirname(ejemplo)) as lsp:
        uri = lsp.open(ejemplo, text=texto)
        definicion = lsp.definition(uri, objetivo[0], objetivo[1])

    if not c.exigir(bool(definicion), "hay definicion para el simbolo importado"):
        return

    destino = definicion[0].get("uri", "")
    c.exigir(
        destino.startswith("file:///"),
        "la direccion del modulo importado lleva las tres barras",
        f"llego '{destino}'",
    )
    c.exigir(
        "/stdlib/" in destino.replace("\\", "/"),
        "la definicion cae dentro de la biblioteca estandar",
        f"llego '{destino}'",
    )
    resto = destino[len("file:///"):]
    c.exigir(
        len(resto) > 2 and resto[1] == ":" or resto.startswith("/"),
        "la ruta del modulo es absoluta",
        f"llego '{destino}'",
    )


def comprobar_diagrama(lsp: VestaLspClient, uri: str, c: Comprobaciones) -> None:
    """Comprueba que el diagrama en HTML es una pagina que el panel puede abrir."""
    print("\n[diagrama]")

    respuesta = lsp.diagram(uri, "ir-post", "html")
    if not isinstance(respuesta, dict) or respuesta.get("error"):
        detalle = respuesta.get("error", campos(respuesta)) if isinstance(respuesta, dict) else ""
        c.exigir(False, "vesta/diagram responde en HTML", detalle)
        return

    html = respuesta.get("text", "")
    c.exigir(bool(html), "el diagrama en HTML no viene vacio")
    c.exigir("<script" in html, "la pagina trae guiones que hay que autorizar")
    c.exigir(
        "http://" not in html.replace("http://www.w3.org", ""),
        "la pagina es autocontenida (no carga nada de la red)",
        "el panel bloquea cualquier recurso externo",
    )
    c.exigir(
        "onclick=" not in html.lower(),
        "la pagina no usa manejadores en linea",
        "el panel los bloquea al autorizar los guiones por valor unico",
    )


def comprobar_instruccion(c: Comprobaciones, binario: str) -> None:
    """Comprueba lo que el compilador sabe de una instruccion de un bloque asm.

    Lo que se fija aqui es que la base responda por el mnemonico TAL Y COMO SE
    ESCRIBE: la base nombra las clases como su fuente (`RET_NEAR`, `JNZ`,
    `SETZ`) y quien escribe ensamblador escribe `ret`, `jne`, `sete`.  Sin la
    equivalencia, los mnemonicos mas comunes eran los unicos que no salian.
    """
    print("\n[instruccion]")

    raiz = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))))
    fuente = os.path.join(raiz, "examples_codes_vx", "198_inline_asm_symbols.vx")
    if not os.path.isfile(fuente):
        print(f"  (no esta {fuente})")
        return

    with open(fuente, "r", encoding="utf-8") as fh:
        texto = fh.read()
    lineas = texto.splitlines()

    def linea_de(prefijo: str) -> int:
        for i, l in enumerate(lineas):
            if l.strip().startswith(prefijo):
                return i + 1
        return 0

    with VestaLspClient(binario, root_uri=os.path.dirname(fuente)) as lsp:
        uri = lsp.open(fuente, text=texto)

        def ficha(n: int, arch: str = "x86-64") -> dict:
            r = lsp.request("vesta/instruction",
                            {"uri": uri, "line": n, "cpu": "intel-skylake",
                             "arch": arch})
            return r if isinstance(r, dict) else {}

        for prefijo, clase in (("call ", "CALL_NEAR"), ("ret", "RET_NEAR"),
                               ("mov ", "MOV"), ("lea ", "LEA")):
            n = linea_de(prefijo)
            if n == 0:
                continue
            f = ficha(n)
            c.exigir(f.get("found") is True and f.get("known") is True,
                     f"'{prefijo.strip()}' la conoce la base", campos(f))
            c.exigir(f.get("iclass") == clase,
                     f"'{prefijo.strip()}' resuelve a {clase}",
                     str(f.get("iclass")))
            c.exigir(isinstance(f.get("cost"), dict) and f["cost"].get("timed"),
                     f"'{prefijo.strip()}' trae coste de la microarquitectura",
                     campos(f.get("cost")))

        # El control de flujo es barrera, y por lo que ES: escribe el contador
        # de programa.  No por no reconocerlo, que era lo de antes.
        for prefijo in ("call ", "ret"):
            n = linea_de(prefijo)
            if n:
                c.exigir(ficha(n).get("barrier") is True,
                         f"'{prefijo.strip()}' es barrera")
        n = linea_de("mov ")
        if n:
            c.exigir(ficha(n).get("barrier") is False,
                     "'mov' no es barrera")

        # Lo que no es una instruccion no tiene ficha; lo que lo es pero no se
        # reconoce, si -- y lo dice.
        n = linea_de("asm ")
        if n:
            c.exigir(ficha(n).get("found") is False,
                     "el abridor del bloque no es una instruccion")
        n = linea_de("}")
        if n:
            c.exigir(ficha(n).get("found") is False,
                     "la llave de cierre no es una instruccion")

        # Multi-ISA: preguntar a la base equivocada no inventa una respuesta.
        n = linea_de("call ")
        if n:
            f = ficha(n, "aarch64")
            c.exigir(f.get("isa") == "arm64",
                     "la arquitectura elige a que base se pregunta",
                     str(f.get("isa")))
            c.exigir(f.get("known") is False and bool(f.get("unknownReason")),
                     "lo que la base no conoce se reporta, no se calla",
                     campos(f))


def comprobar_campos_y_opt(c: Comprobaciones, binario: str) -> None:
    """Disposicion en memoria de un campo, y que el nivel de optimizacion llegue.

    Dos cosas que se veian bien y no lo estaban: el hover de un campo se
    quedaba con la PRIMERA definicion que se llamara igual -- dos structs con
    un campo `b` y el de uno contaba el del otro --, y el nivel de
    optimizacion no lo leia ninguna vista, asi que `-O0` y `-O3` daban el mismo
    texto.
    """
    print("\n[campos y nivel de optimizacion]")

    raiz = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))))
    fuente = os.path.join(raiz, "examples_codes_vx", "306_align_struct.vx")
    if not os.path.isfile(fuente):
        print(f"  (no esta {fuente})")
        return

    with open(fuente, "r", encoding="utf-8") as fh:
        texto = fh.read()
    lineas = texto.splitlines()

    with VestaLspClient(binario, root_uri=os.path.dirname(fuente)) as lsp:
        uri = lsp.open(fuente, text=texto)

        def hover_de(n: int, aguja: str) -> str:
            col = lineas[n - 1].index(aguja) + 1
            r = lsp.request("textDocument/hover",
                            {"textDocument": {"uri": uri},
                             "position": {"line": n - 1, "character": col}})
            cont = (r or {}).get("contents") or {}
            return (cont.get("value") if isinstance(cont, dict) else str(cont)) or ""

        # Cada `b` es el de SU struct: contenedor, doc y disposicion.
        esperado = [("struct Small", "Small"), ("struct Aligned16", "Aligned16")]
        for prefijo, contenedor in esperado:
            n = next((i + 1 for i, l in enumerate(lineas)
                      if l.strip().startswith(prefijo)), 0)
            if n == 0:
                continue
            md = hover_de(n, " b;")
            c.exigir(f"`{contenedor}`" in md,
                     f"el campo de {contenedor} dice que es de {contenedor}",
                     md.replace("\n", " | ")[:120])
            c.exigir("+1" in md,
                     f"el campo de {contenedor} dice donde cae en memoria",
                     md.replace("\n", " | ")[:120])
        md16 = hover_de(next(i + 1 for i, l in enumerate(lineas)
                             if l.strip().startswith("struct Aligned16")), " b;")
        c.exigir("16" in md16,
                 "el struct alineado a 16 lo dice en su campo",
                 md16.replace("\n", " | ")[:120])

        # El nivel de optimizacion cambia lo que sale.
        for metodo, extra in (("vesta/ir", {"phase": "post"}),
                              ("vesta/bytecode", {})):
            salidas = {}
            for nivel in (0, 3):
                par = {"uri": uri, "opt": nivel}
                par.update(extra)
                r = lsp.request(metodo, par) or {}
                salidas[nivel] = r.get("text") or r.get("asm") or ""
            c.exigir(bool(salidas[0]) and bool(salidas[3]),
                     f"{metodo} responde a los dos niveles")
            c.exigir(salidas[0] != salidas[3],
                     f"{metodo} cambia con el nivel de optimizacion",
                     "si sale lo mismo, el nivel no llego al compilador")


def comprobar_stdlib_analiza(c: Comprobaciones, binario: str) -> None:
    """Que un fichero DE la biblioteca estandar se pueda analizar.

    Abrir uno directamente hacia que sus directorios de encima entraran como
    raices de busqueda, y esos CONTIENEN al paquete: el mismo namespace
    aparecia dos veces, el resolutor avisaba de dos librerias en disputa y el
    modulo no producia nada.  Sin IR no hay disposicion de tipos, ni coste, ni
    ninguna de las vistas.
    """
    print("\n[analisis dentro de la biblioteca estandar]")

    raiz = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))))
    fuente = os.path.join(raiz, "stdlib", "vx", "std", "syscall", "linux.vx")
    if not os.path.isfile(fuente):
        print(f"  (no esta {fuente})")
        return

    with open(fuente, "r", encoding="utf-8") as fh:
        texto = fh.read()

    with VestaLspClient(binario, root_uri=os.path.dirname(fuente)) as lsp:
        uri = lsp.open(fuente, text=texto)
        diags = lsp.diagnostics(uri) or []
        disputa = [d for d in diags
                   if "namespace" in str(d.get("message", "")).lower()
                   and "dos" in str(d.get("message", "")).lower()]
        c.exigir(not disputa,
                 "el paquete no se pelea consigo mismo",
                 str(disputa[:1])[:160])

        # Es un modulo de Linux: con ese objetivo tiene que producir IR.
        r = lsp.request("vesta/ir",
                        {"uri": uri, "phase": "post", "os": "linux",
                         "arch": "x86-64"}) or {}
        c.exigir(len(r.get("text", "")) > 0,
                 "con objetivo linux el modulo produce IR",
                 str(r.get("error", ""))[:120])

        # Y cuando no lo produce, el hover DICE por que en vez de callarse.
        # La linea que lo DECLARA, no la del comentario que lo menciona.
        lineas = texto.splitlines()
        n = next((i + 1 for i, l in enumerate(lineas)
                  if "invoke_method" in l and not l.strip().startswith("//")), 0)
        if n:
            col = lineas[n - 1].index("invoke_method") + 1
            h = lsp.request("textDocument/hover",
                            {"textDocument": {"uri": uri},
                             "position": {"line": n - 1, "character": col}})
            cont = (h or {}).get("contents") or {}
            md = (cont.get("value") if isinstance(cont, dict) else str(cont)) or ""
            c.exigir("no compila" in md or "+" in md,
                     "el hover dice donde cae el campo, o por que no lo sabe",
                     md.replace("\n", " | ")[:140])


def comprobar_objetivo_del_analisis(c: Comprobaciones, binario: str) -> None:
    """Que los diagnosticos sigan al objetivo configurado, no al anfitrion.

    Los errores salen de COMPILAR, asi que dependen de para que maquina se
    compila: lo que esta bajo un `@Target` que no encaja no existe, y con ello
    se van sus funciones y sus tipos.  Analizando siempre contra el anfitrion,
    un modulo de Linux leido desde Windows es un muro de errores ciertos y sin
    ningun valor para quien lo edita.
    """
    print("\n[objetivo del analisis]")

    fuente = os.path.join(tempfile.gettempdir(), "vesta_smoke_solo_linux.vx")
    with open(fuente, "w", encoding="utf-8") as fh:
        fh.write(
            "// Solo existe en Linux: sirve para comprobar contra que maquina\n"
            "// analiza el servidor.\n"
            "\n"
            '@Target("os:linux")\n'
            "i64 numero_de_llamada() { return 60; }\n"
            "\n"
            "i32 main() { return (i32)numero_de_llamada(); }\n"
        )

    with VestaLspClient(binario, root_uri=os.path.dirname(fuente)) as lsp:
        uri = lsp.open(fuente, text=open(fuente, encoding="utf-8").read())
        antes = len(lsp.diagnostics(uri) or [])
        c.exigir(antes > 0,
                 "para el anfitrion, lo que es de otra maquina no existe",
                 "sin errores aqui el caso no prueba nada")

        lsp._notify("workspace/didChangeConfiguration",
                    {"settings": {"vesta": {"inspect": {"os": "linux",
                                                        "arch": "x86-64"}}}})
        # El servidor reanaliza y republica; el cliente recoge las
        # notificaciones cuando habla con el, asi que se le da conversacion.
        despues = antes
        for _ in range(10):
            time.sleep(1)
            lsp.request("vesta/functions", {"uri": uri})
            despues = len(lsp.diagnostics(uri) or [])
            if despues == 0:
                break
        c.exigir(despues == 0,
                 "con el objetivo puesto, el modulo de esa maquina compila",
                 f"quedan {despues} diagnosticos")


def comprobar_varios_hilos(c: Comprobaciones, binario: str) -> None:
    """Que el servidor atienda varias consultas a la vez, y bien.

    Se comprueban tres cosas distintas:

      1. Que responde DESORDENADO.  Un servidor que atiende de uno en uno
         contesta en el mismo orden en que se le pregunta; que una respuesta
         adelante a otra solo puede pasar si hay varios atendiendo.
      2. Que lo que contesta es lo MISMO que contestaria de uno en uno.  Es lo
         unico que importa de verdad: ir mas rapido no vale nada si la
         respuesta cambia.
      3. Que ningun diagnostico sale sin texto.  El editor rechaza un
         diagnostico vacio y con el tira la tanda entera -- no se ve ninguno --,
         asi que un mensaje que falta no se nota como un mensaje que falta.
    """
    print("\n[varios hilos]")

    raiz = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))))
    ficheros = [os.path.join(raiz, "examples_codes_vx", n) for n in
                ("306_align_struct.vx", "198_inline_asm_symbols.vx")]
    ficheros = [f for f in ficheros if os.path.isfile(f)]
    if len(ficheros) < 2:
        print("  (faltan los ejemplos)")
        return

    peticiones = []
    for f in ficheros:
        for metodo, extra in (("vesta/functions", {}),
                              ("vesta/ir", {"phase": "post"}),
                              ("vesta/complexity", {}),
                              ("vesta/bytecode", {})):
            peticiones.append((metodo, extra, f))

    def corre(en_rafaga: bool) -> Dict[int, str]:
        firmas: Dict[int, str] = {}
        with VestaLspClient(binario, root_uri=os.path.dirname(ficheros[0])) as lsp:
            uris = {f: lsp.open(f, text=open(f, encoding="utf-8").read())
                    for f in ficheros}
            if en_rafaga:
                # Todas de golpe y se recogen despues: asi se solapan de verdad.
                ids = []
                for i, (metodo, extra, f) in enumerate(peticiones):
                    par = dict(extra)
                    par["uri"] = uris[f]
                    ids.append((i, metodo, lsp.send_request(metodo, par)))
                for i, metodo, rid in ids:
                    firmas[i] = json.dumps(lsp.await_response(rid, metodo),
                                           sort_keys=True)[:2000]
            else:
                for i, (metodo, extra, f) in enumerate(peticiones):
                    par = dict(extra)
                    par["uri"] = uris[f]
                    firmas[i] = json.dumps(lsp.request(metodo, par),
                                           sort_keys=True)[:2000]
        return firmas

    serie = corre(False)
    c.exigir(len(serie) == len(peticiones),
             "responde a todas las consultas de una en una",
             f"{len(serie)} de {len(peticiones)}")

    rafaga = corre(True)
    c.exigir(len(rafaga) == len(peticiones),
             "responde a todas las consultas lanzadas de golpe",
             f"{len(rafaga)} de {len(peticiones)}")
    distintas = [i for i in serie if i in rafaga and serie[i] != rafaga[i]]
    c.exigir(not distintas,
             "contesta lo mismo a la vez que de una en una",
             f"cambian {len(distintas)}: {distintas[:4]}")


def comprobar_diagnosticos_con_texto(c: Comprobaciones, binario: str) -> None:
    """Que ningun diagnostico se publique sin texto.

    Un diagnostico catalogado no lleva la frase escrita: lleva el codigo y los
    datos, y la frase se compone al mostrarla.  Publicando el campo crudo salia
    vacio, el editor lo rechazaba y con el TODA la tanda: no se veia ninguno, y
    en el registro solo aparecia "message must be set".
    """
    print("\n[diagnosticos con texto]")

    raiz = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))))
    fuente = os.path.join(raiz, "stdlib", "vx", "std", "syscall", "linux.vx")
    if not os.path.isfile(fuente):
        print(f"  (no esta {fuente})")
        return

    with VestaLspClient(binario, root_uri=os.path.dirname(fuente)) as lsp:
        uri = lsp.open(fuente, text=open(fuente, encoding="utf-8").read())
        diags = lsp.diagnostics(uri) or []
        c.exigir(len(diags) > 0,
                 "el fichero produce diagnosticos con los que probar")
        vacios = [d for d in diags if not str(d.get("message", "")).strip()]
        c.exigir(not vacios,
                 "ningun diagnostico se publica sin texto",
                 f"{len(vacios)} de {len(diags)} vienen vacios")


def comprobar_formas_de_respuesta(c: Comprobaciones, binario: str) -> None:
    """Que la forma de cada respuesta sea la que la extension lee.

    Aqui el fallo no se parece a un fallo.  El servidor contesta bien, la
    extension lee un campo que no existe y ensena lo que hay: nada.  Ni error
    ni aviso -- la vista sale vacia y parece que no habia nada que contar.
    Paso con dos: el diff del IR devuelve FILAS y se le pedia un texto, y la
    complejidad manda la confianza por su nombre donde antes iba un numero (que
    ademas reventaba al formatear la tabla).
    """
    print("\n[forma de las respuestas]")

    raiz = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))))
    fuente = os.path.join(raiz, "examples_codes_vx", "306_align_struct.vx")
    if not os.path.isfile(fuente):
        print(f"  (no esta {fuente})")
        return

    with VestaLspClient(binario, root_uri=os.path.dirname(fuente)) as lsp:
        uri = lsp.open(fuente, text=open(fuente, encoding="utf-8").read())

        diff = lsp.request("vesta/irDiff", {"uri": uri, "function": ""}) or {}
        filas = diff.get("rows")
        c.exigir(isinstance(filas, list) and len(filas) > 0,
                 "el diff del IR devuelve filas", campos(diff))
        if filas:
            primera = filas[0]
            c.exigir({"k", "l", "r"} <= set(primera.keys()),
                     "cada fila dice que paso y las dos versiones",
                     campos(primera))
            clases = {f.get("k") for f in filas}
            c.exigir(clases <= {"same", "del", "add", "chg"},
                     "las filas usan las cuatro clases conocidas",
                     str(clases))

        comp = lsp.request("vesta/complexity", {"uri": uri}) or {}
        funcs = comp.get("functions") or []
        c.exigir(len(funcs) > 0, "la complejidad devuelve funciones",
                 campos(comp))
        if funcs:
            f0 = funcs[0]
            # Toda celda que acaba en una tabla de texto tiene que ser texto o
            # numero; un objeto o una lista revientan al formatear.
            for clave in ("name", "partial", "total", "confidence",
                          "total_confidence", "declared"):
                if clave in f0:
                    c.exigir(isinstance(f0[clave], str),
                             f"'{clave}' llega como texto",
                             f"llega como {type(f0[clave]).__name__}")

        facts = lsp.request("vesta/asaFacts", {"uri": uri}) or {}
        hechos = facts.get("facts")
        c.exigir(isinstance(hechos, list), "el ASA devuelve hechos",
                 campos(facts))
        if hechos:
            h0 = hechos[0]
            for clave in ("line", "function", "subject", "label", "certainty",
                          "source", "domain"):
                c.exigir(clave in h0, f"cada hecho trae '{clave}'",
                         campos(h0))
        dominios = facts.get("domains")
        c.exigir(isinstance(dominios, list) and len(dominios) > 0,
                 "el ASA dice que analisis miraron", campos(facts))
        if dominios:
            d0 = dominios[0]
            c.exigir({"domain", "facts", "looked", "silent"} <= set(d0.keys()),
                     "cada analisis dice cuanto miro y cuanto callo",
                     campos(d0))
            # El nombre de un analisis no dice que mira; la frase, si.
            con_proposito = [d for d in dominios if d.get("purpose")]
            c.exigir(len(con_proposito) > 0,
                     "los analisis dicen que miran, en una frase",
                     "sin esto 'asa.rangos' no le dice nada a nadie")

        # Y de QUE habla cada hecho, y COMO se llego a el.  Sin lo primero, ocho
        # hechos de la misma linea son ocho filas identicas que dicen "valor";
        # sin lo segundo, son afirmaciones que hay que creerse.
        if hechos:
            for clave in ("subjectId", "subjectText", "sourceText", "rule",
                          "from", "producer", "restsOn"):
                c.exigir(any(clave in h for h in hechos),
                         f"los hechos traen '{clave}'", campos(hechos[0]))
            con_texto = [h for h in hechos
                         if h.get("subject") == "valor" and h.get("subjectText")]
            valores = [h for h in hechos if h.get("subject") == "valor"]
            if valores:
                c.exigir(len(con_texto) > 0,
                         "un hecho sobre un valor dice de QUE valor habla",
                         f"{len(con_texto)} de {len(valores)} lo dicen")
            # Lo que se ensena por defecto es el CODIGO: una operacion del
            # IR identifica sin lugar a dudas y no dice nada a quien no lo
            # tiene delante.
            con_codigo = [h for h in hechos if h.get("sourceText")]
            c.exigir(len(con_codigo) > 0,
                     "los hechos dicen de que LINEA DE CODIGO hablan",
                     f"{len(con_codigo)} de {len(hechos)}")

            derivados = [h for h in hechos if h.get("from")]
            c.exigir(len(derivados) > 0,
                     "hay hechos que dicen de cuales se siguen",
                     "es lo que permite recorrer la derivacion")
            # Y esa referencia tiene que apuntar a un hecho que exista.
            malas = [i for h in derivados for i in h["from"]
                     if not isinstance(i, int) or i < 0 or i >= len(hechos)]
            c.exigir(not malas,
                     "esas referencias apuntan a hechos que existen",
                     str(malas[:4]))


def comprobar_nombres_y_navegacion(c: Comprobaciones, binario: str) -> None:
    """Que se ensene el nombre ESCRITO, y que lleve a la funcion.

    `std__windows__GetCurrentFiber` es el nombre que el compilador construye al
    aplanar los namespaces.  Sirve para identificar -- es unico -- y no para
    mostrar: nadie escribio eso.  Se manda el interno Y el escrito, y ademas se
    puede buscar un simbolo por su nombre, que es lo que permite ir a una
    funcion desde un sitio donde solo se la nombra.
    """
    print("\n[nombres y navegacion]")

    raiz = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))))
    fuente = os.path.join(raiz, "stdlib", "vx", "std", "windows.vx")
    if not os.path.isfile(fuente):
        print(f"  (no esta {fuente})")
        return

    with VestaLspClient(binario, root_uri=raiz) as lsp:
        uri = lsp.open(fuente, text=open(fuente, encoding="utf-8").read())

        funcs = (lsp.request("vesta/functions", {"uri": uri}) or {}).get(
            "functions") or []
        c.exigir(len(funcs) > 0, "hay funciones con las que probar")
        if funcs:
            con_display = [f for f in funcs if f.get("display")]
            c.exigir(len(con_display) == len(funcs),
                     "cada funcion trae el nombre escrito",
                     f"{len(con_display)} de {len(funcs)}")
            manglados = [f for f in funcs if "__" in (f.get("display") or "")]
            c.exigir(not manglados,
                     "el nombre escrito no lleva separadores internos",
                     str([f.get("display") for f in manglados[:2]]))

        hechos = (lsp.request("vesta/asaFacts", {"uri": uri}) or {}).get(
            "facts") or []
        con_fn = [h for h in hechos if h.get("function")]
        if con_fn:
            c.exigir(all(h.get("functionDisplay") for h in con_fn),
                     "cada hecho trae el nombre escrito de su funcion")

        # Y el nombre tiene que poder llevar a la funcion.
        objetivo = next((f for f in funcs if f.get("display")), None)
        if objetivo:
            for consulta in (objetivo["display"], objetivo["name"]):
                res = lsp.request("workspace/symbol", {"query": consulta})
                c.exigir(isinstance(res, list) and len(res) > 0,
                         f"se encuentra el simbolo por '{consulta[:34]}'",
                         campos(res) if not isinstance(res, list) else "vacio")
                if isinstance(res, list) and res:
                    loc = res[0].get("location") or {}
                    c.exigir("uri" in loc and "range" in loc,
                             "el resultado dice donde esta", campos(loc))


def comprobar_informe_por_funcion(c: Comprobaciones, binario: str) -> None:
    """Que el informe por funcion venga en DATOS, no en un volcado.

    Lo que una funcion declara -- coste, reservas, pila, pureza -- y lo que el
    compilador mide de ella son dos listas que solo sirven puestas una al lado
    de la otra: un contrato existe para que se note cuando dejan de coincidir.
    Eso se ensenaba como una tabla de texto, que no se puede filtrar ni ordenar
    ni pulsar.
    """
    print("\n[informe por funcion]")

    raiz = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))))
    fuente = os.path.join(raiz, "examples_codes_vx", "198_inline_asm_symbols.vx")
    if not os.path.isfile(fuente):
        print(f"  (no esta {fuente})")
        return

    with VestaLspClient(binario, root_uri=os.path.dirname(fuente)) as lsp:
        uri = lsp.open(fuente, text=open(fuente, encoding="utf-8").read())
        r = lsp.request("vesta/functionReport", {"uri": uri}) or {}
        fns = r.get("functions") or []
        c.exigir(len(fns) > 0, "el informe trae funciones", campos(r))
        if not fns:
            return

        f0 = fns[0]
        for clave in ("name", "display", "line", "cost", "checks", "aot"):
            c.exigir(clave in f0, f"cada funcion trae '{clave}'", campos(f0))
        c.exigir("__" not in (f0.get("display") or ""),
                 "el nombre que se ensena no lleva separadores internos",
                 str(f0.get("display")))

        coste = f0.get("cost") or {}
        for clave in ("partial", "total", "confidence", "loops", "declared",
                      "mismatch"):
            c.exigir(clave in coste, f"el coste trae '{clave}'", campos(coste))

        # Lo MEDIDO es lo que hace util al contrato: sin ello solo se sabe lo
        # que la funcion prometio, que es la mitad que no comprueba nada.
        con_medida = [f for f in fns if f.get("measured")]
        c.exigir(len(con_medida) > 0,
                 "las funciones traen lo que el compilador mide de ellas")
        if con_medida:
            m = con_medida[0]["measured"]
            for clave in ("allocTotal", "stackBounded", "throws", "panics",
                          "pure", "effectsKnown"):
                c.exigir(clave in m, f"lo medido trae '{clave}'", campos(m))
            # `stackTotal` solo aparece cuando el marco se CONOCE.  Antes salia
            # siempre, y cuando no se sabia traia el centinela -- que en la
            # vista se leia como un marco de 18 trillones de bytes.  Ahora lo
            # que siempre esta es `stackBounded`, que dice si hay cifra.
            if m.get("stackBounded"):
                c.exigir("stackTotal" in m,
                         "si el marco esta acotado, viene su tamano", campos(m))
            else:
                c.exigir("stackTotal" not in m,
                         "y si no se conoce, NO se inventa una cifra",
                         campos(m))

        # Y si el modo nativo puede con cada una, que es la otra pregunta que
        # se hace mirando esto.
        c.exigir(all(isinstance((f.get("aot") or {}).get("ok"), bool)
                     for f in fns),
                 "cada funcion dice si compila a nativo")


def comprobar_bloque_asm(c: Comprobaciones, binario: str) -> None:
    """Que un bloque de asm venga con su FLUJO resuelto.

    Leer asm escrito a mano es ir saltando: se ve un salto y hay que buscar su
    etiqueta arriba o abajo.  El compilador ya construye el grafo de ese bloque
    para analizarlo, asi que sabe que instruccion salta a cual: con eso se
    dibujan las flechas en vez de seguirlas con el dedo.

    Lo que NO se resuelve -- un salto indirecto, una etiqueta ausente -- tiene
    que venir dicho: dibujar un flujo a medias sin avisar es peor que no
    dibujarlo.
    """
    print(chr(10) + "[bloque de asm]")

    raiz = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))))
    fuente = os.path.join(raiz, "examples_codes_vx", "asm_loop.vx")
    if not os.path.isfile(fuente):
        print("  (no esta " + fuente + ")")
        return

    texto = open(fuente, encoding="utf-8").read()
    lineas = texto.splitlines()
    dentro = next((i + 1 for i, l in enumerate(lineas)
                   if l.strip().startswith("add ")), 0)
    if dentro == 0:
        print("  (el ejemplo no tiene el bloque esperado)")
        return

    with VestaLspClient(binario, root_uri=os.path.dirname(fuente)) as lsp:
        uri = lsp.open(fuente, text=texto)
        r = lsp.request("vesta/asmBlock",
                        {"uri": uri, "line": dentro, "cpu": "intel-skylake",
                         "arch": "x86-64"}) or {}
        c.exigir(r.get("found") is True, "encuentra el bloque que contiene la linea",
                 campos(r))
        insns = r.get("instructions") or []
        c.exigir(len(insns) > 0, "el bloque trae sus instrucciones")
        if not insns:
            return

        for clave in ("index", "text", "line", "labels", "flow", "target",
                      "targetIndex", "known", "barrier"):
            c.exigir(clave in insns[0], "cada instruccion trae '" + clave + "'",
                     campos(insns[0]))

        # Cada instruccion tiene que apuntar a SU linea del fuente.
        bien = [i for i in insns
                if i["line"] > 0 and i["text"].split()[0] in
                lineas[i["line"] - 1]]
        c.exigir(len(bien) == len(insns),
                 "cada instruccion apunta a su linea del fuente",
                 str(len(bien)) + " de " + str(len(insns)))

        # Y el salto tiene que estar RESUELTO: sin eso no hay flecha que pintar.
        saltos = [i for i in insns if i["flow"] in ("salto", "rama")]
        c.exigir(len(saltos) > 0, "el bloque tiene algun salto con el que probar")
        if saltos:
            c.exigir(all(0 <= i["targetIndex"] < len(insns) for i in saltos),
                     "cada salto dice a que instruccion va",
                     str([i["targetIndex"] for i in saltos]))

        # Y lo que la base sabe de cada una.
        conocidas = [i for i in insns if i.get("known")]
        c.exigir(len(conocidas) == len(insns),
                 "la base conoce todas las instrucciones del bloque",
                 str(len(conocidas)) + " de " + str(len(insns)))
        con_coste = [i for i in insns if i.get("cost")]
        c.exigir(len(con_coste) > 0,
                 "las instrucciones traen lo que cuestan en esa microarquitectura")

        for clave in ("hasIndirect", "hasUnresolved", "unknownTerminators"):
            c.exigir(clave in r, "el bloque dice lo que NO pudo resolver ('" +
                     clave + "')", campos(r))

    # Y el flujo de TODOS los bloques, que es lo que se pinta sobre el codigo.
    with VestaLspClient(binario, root_uri=os.path.dirname(fuente)) as lsp:
        uri = lsp.open(fuente, text=texto)
        f = lsp.request("vesta/asmFlow", {"uri": uri, "arch": "x86-64"}) or {}
        bloques = f.get("blocks")
        c.exigir(isinstance(bloques, list) and len(bloques) > 0,
                 "el fichero entero devuelve sus bloques", campos(f))
        if bloques:
            b0 = bloques[0]
            for clave in ("firstLine", "lastLine", "jumps"):
                c.exigir(clave in b0, "cada bloque trae '" + clave + "'",
                         campos(b0))
            saltos = b0.get("jumps") or []
            c.exigir(len(saltos) > 0, "el bloque trae sus saltos")
            if saltos:
                # De linea a linea: es lo unico que hace falta para dibujar, y
                # tiene que caer DENTRO del bloque.
                dentro = [j for j in saltos
                          if b0["firstLine"] <= j["fromLine"] <= b0["lastLine"]
                          and b0["firstLine"] <= j["toLine"] <= b0["lastLine"]]
                c.exigir(len(dentro) == len(saltos),
                         "cada salto va de una linea del bloque a otra",
                         str([(j["fromLine"], j["toLine"]) for j in saltos]))


def main() -> int:
    """Punto de entrada del script."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lsp", help="ruta al ejecutable vesta_lsp")
    parser.add_argument("--file", help="fichero .vx con el que probar")
    args = parser.parse_args()

    binario = discover_lsp(args.lsp)
    if not binario:
        print("no se encontro vesta_lsp; pasa --lsp con la ruta")
        return 2

    fuente = args.file or ejemplo_por_defecto()
    if not fuente or not os.path.isfile(fuente):
        print("no se encontro ningun ejemplo .vx; pasa --file con la ruta")
        return 2

    print(f"servidor : {binario}")
    print(f"ejemplo  : {fuente}")

    c = Comprobaciones()
    with open(fuente, "r", encoding="utf-8") as fh:
        texto = fh.read()

    with VestaLspClient(binario, root_uri=os.path.dirname(fuente)) as lsp:
        comprobar_capacidades(lsp, c)
        comprobar_formato(lsp, c)
        uri = lsp.open(fuente, text=texto)

        print("\n[diagnosticos]")
        diagnosticos = lsp.diagnostics(uri)
        c.exigir(
            isinstance(diagnosticos, list),
            "el servidor publica diagnosticos del documento",
            campos(diagnosticos),
        )

        comprobar_navegacion(lsp, uri, texto, c)
        nombres = comprobar_vistas(lsp, uri, c) or []
        objetivo = "main" if "main" in nombres else (nombres[0] if nombres else "")
        comprobar_correlacion(lsp, uri, objetivo, c)
        comprobar_diagrama(lsp, uri, c)

    # Fuera del bloque anterior: usan su propia conexion sobre otro ejemplo.
    comprobar_navegacion_cross_module(c, binario)
    comprobar_instruccion(c, binario)
    comprobar_campos_y_opt(c, binario)
    comprobar_stdlib_analiza(c, binario)
    comprobar_objetivo_del_analisis(c, binario)
    comprobar_varios_hilos(c, binario)
    comprobar_diagnosticos_con_texto(c, binario)
    comprobar_formas_de_respuesta(c, binario)
    comprobar_nombres_y_navegacion(c, binario)
    comprobar_informe_por_funcion(c, binario)
    comprobar_bloque_asm(c, binario)

    print(f"\n{c.pasadas} comprobaciones pasadas, {len(c.fallos)} fallidas")
    for fallo in c.fallos:
        print(f"  - {fallo}")
    return 0 if not c.fallos else 1


if __name__ == "__main__":
    sys.exit(main())
