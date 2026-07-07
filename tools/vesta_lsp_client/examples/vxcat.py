#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""vxcat: un `cat` con resaltado de sintaxis para fuentes Vesta (.vx).

EJEMPLO consumidor de la libreria ``vesta_lsp_client``.  Aprovecha los
*semantic tokens* del LSP para colorear el codigo en la terminal.  A diferencia
de un resaltador por regex, el color viene del analisis real del compilador:
distingue funciones de metodos, clases de structs, conceptos, registros dentro
de bloques `asm`, secuencias de escape en strings, etc.

El ESQUEMA DE COLOR es de este ejemplo, no de la libreria: la libreria solo
entrega los *spans* tipados (:func:`vesta_lsp_client.render`) y cada programa
elige como pintarlos.

Uso:
    python examples/vxcat.py [--lsp <vesta_lsp>] [--no-color] <fichero.vx> ...

El binario del LSP se auto-detecta (PATH + rutas de instalacion); pasa --lsp
para forzar una ruta concreta.
"""
import argparse
import os
import sys

from vesta_lsp_client import VestaLspClient, decode_semantic_tokens, render

# Esquema de color de ESTE ejemplo: nombre de tipo de token -> color ANSI 256.
# Otro programa puede definir un mapa totalmente distinto (o salida HTML, etc.).
ANSI = {
    "keyword": "\033[38;5;204m",        # rosa
    "function": "\033[38;5;220m",       # amarillo
    "method": "\033[38;5;220m",
    "class": "\033[38;5;80m",           # cian
    "struct": "\033[38;5;80m",
    "enum": "\033[38;5;80m",
    "interface": "\033[38;5;114m",      # conceptos / interfaces
    "type": "\033[38;5;80m",
    "typeParameter": "\033[38;5;79m",
    "namespace": "\033[38;5;80m",
    "enumMember": "\033[38;5;117m",
    "string": "\033[38;5;114m",         # verde
    "number": "\033[38;5;215m",         # naranja
    "comment": "\033[38;5;244m",        # gris
    "macro": "\033[38;5;177m",          # violeta
    "operator": "\033[38;5;252m",
    "modifier": "\033[38;5;75m",        # azul
    "parameter": "\033[38;5;252m",
    "variable": "\033[38;5;252m",
    "property": "\033[38;5;153m",
    "register": "\033[38;5;203m",       # rojo (asm)
    "escapeSequence": "\033[38;5;120m",
    "interpolation": "\033[38;5;220m",
}
RESET = "\033[0m"


def ansi_style(name):
    """Funcion de estilo que consume `render`: tipo -> (prefijo, sufijo)."""
    color = ANSI.get(name)
    return (color, RESET) if color else ("", "")


def enable_windows_ansi():
    """Activa las secuencias ANSI en la consola de Windows."""
    if os.name != "nt":
        return
    try:
        import ctypes
        k = ctypes.windll.kernel32
        k.SetConsoleMode(k.GetStdHandle(-11), 7)  # PROCESSED | VT_PROCESSING
    except Exception:
        pass


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="vxcat",
        description="cat con resaltado de sintaxis Vesta via LSP")
    ap.add_argument("files", nargs="+", help="fuentes .vx a mostrar")
    ap.add_argument("--lsp", default=None,
                    help="ruta a vesta_lsp (por defecto: auto-detectar)")
    ap.add_argument("--no-color", action="store_true",
                    help="desactiva el color (salida plana)")
    ap.add_argument("--force-color", action="store_true",
                    help="fuerza el color aunque la salida no sea una terminal")
    args = ap.parse_args(argv)

    use_color = not args.no_color and (
        args.force_color
        or (sys.stdout.isatty() and os.environ.get("NO_COLOR") is None))
    if use_color:
        enable_windows_ansi()

    try:
        with VestaLspClient(args.lsp) as lsp:
            legend = lsp.semantic_token_legend()
            for path in args.files:
                with open(path, "r", encoding="utf-8") as fh:
                    text = fh.read()
                uri = lsp.open(path, text=text)
                data = (lsp.semantic_tokens(uri) or {}).get("data", [])
                tokens = decode_semantic_tokens(data)
                out = render(text, tokens, legend, ansi_style) \
                    if use_color else text
                sys.stdout.write(out)
                if not text.endswith("\n"):
                    sys.stdout.write("\n")
    except Exception as exc:  # noqa: BLE001 (CLI: mensaje claro)
        sys.stderr.write("vxcat: %s\n" % exc)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
