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
    for capacidad in ("hoverProvider", "definitionProvider", "referencesProvider"):
        c.exigir(bool(capacidades.get(capacidad)), f"anuncia {capacidad}")
    c.exigir(
        "completionProvider" in capacidades,
        "anuncia completionProvider",
    )
    anunciados = capacidades.get("experimental", {}).get("vestaMethods", [])
    for metodo in METODOS_ESPERADOS:
        c.exigir(metodo in anunciados, f"anuncia el metodo propio {metodo}")


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

    # Fuera del bloque anterior: usa su propia conexion sobre otro ejemplo.
    comprobar_navegacion_cross_module(c, binario)

    print(f"\n{c.pasadas} comprobaciones pasadas, {len(c.fallos)} fallidas")
    for fallo in c.fallos:
        print(f"  - {fallo}")
    return 0 if not c.fallos else 1


if __name__ == "__main__":
    sys.exit(main())
