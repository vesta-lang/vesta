#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""vxcat: un `cat` con resaltado de sintaxis para fuentes Vesta (.vx).

EJEMPLO consumidor de la libreria ``vesta_lsp_client``.  Aprovecha los
*semantic tokens* del LSP para colorear el codigo en la terminal.  A diferencia
de un resaltador por regex, el color viene del analisis real del compilador:
distingue funciones de metodos, clases de structs, conceptos, registros dentro
de bloques `asm`, secuencias de escape en strings, etc.

El ESQUEMA DE COLOR vive en ``examples/_console.py`` (de los ejemplos), no en la
libreria: la libreria solo entrega los *spans* tipados y cada programa elige su
paleta.

Uso:
    python examples/vxcat.py [--lsp <vesta_lsp>] [--no-numbers] <fichero.vx> ...
"""
import argparse
import sys

import _console as con
from vesta_lsp_client import VestaLspClient, decode_semantic_tokens


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="vxcat",
        description="cat con resaltado de sintaxis Vesta via LSP")
    ap.add_argument("files", nargs="+", help="fuentes .vx a mostrar")
    ap.add_argument("--lsp", default=None,
                    help="ruta a vesta_lsp (por defecto: auto-detectar)")
    ap.add_argument("--no-numbers", action="store_true",
                    help="no mostrar numeros de linea")
    args = ap.parse_args(argv)

    try:
        with VestaLspClient(args.lsp) as lsp:
            legend = lsp.semantic_token_legend()
            for path in args.files:
                with open(path, "r", encoding="utf-8") as fh:
                    text = fh.read()
                uri = lsp.open(path, text=text)
                data = (lsp.semantic_tokens(uri) or {}).get("data", [])
                tokens = decode_semantic_tokens(data)
                sys.stdout.write(
                    con.highlight_source(text, tokens, legend,
                                         gutter=not args.no_numbers))
                if not text.endswith("\n"):
                    sys.stdout.write("\n")
    except Exception as exc:  # noqa: BLE001 (CLI: mensaje claro)
        sys.stderr.write("vxcat: %s\n" % exc)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
