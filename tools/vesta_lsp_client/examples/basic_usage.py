#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Ejemplo basico: abrir un fuente Vesta y consultar el LSP.

Uso:
    python examples/basic_usage.py [--lsp <vesta_lsp>] [fichero.vx]

Si no se da fichero, usa un fuente de demostracion incrustado.  El binario del
LSP se auto-detecta (PATH + rutas de instalacion); usa --lsp para forzarlo.

Muestra diagnosticos, hover, info de simbolo, completado, lista de funciones y
complejidad Big-O.
"""
import argparse
import sys

from vesta_lsp_client import VestaLspClient

DEMO = """\
// Suma dos enteros.
i64 suma(i64 a, i64 b) { return a + b; }

// Bucle acumulador (complejidad lineal).
i64 acumular(i64 n) {
    i64 s = 0;
    i64 i = 0;
    while (i < n) { s = s + i; i = i + 1; }
    return s;
}

i64 main() { return suma(2, 3); }
"""


def main():
    ap = argparse.ArgumentParser(
        description="Consulta basica al LSP de Vesta (diagnosticos, hover, ...)")
    ap.add_argument("file", nargs="?", help="fuente .vx (opcional; hay demo)")
    ap.add_argument("--lsp", default=None,
                    help="ruta a vesta_lsp (por defecto: auto-detectar)")
    args = ap.parse_args()

    if args.file:
        with open(args.file, "r", encoding="utf-8") as fh:
            source = fh.read()
        name = args.file
    else:
        source, name = DEMO, "basic_demo.vx"

    try:
        with VestaLspClient(args.lsp) as lsp:
            uri = lsp.open(name, text=source)

            print("== Diagnosticos ==")
            diags = lsp.diagnostics(uri)
            if not diags:
                print("  (ninguno)")
            for d in diags:
                sev = {1: "error", 2: "aviso", 3: "info",
                       4: "pista"}.get(d.get("severity"), "?")
                rng = d.get("range", {}).get("start", {})
                print("  %s %s:%s  %s" % (
                    sev, rng.get("line"), rng.get("character"),
                    d.get("message")))

            # Info del primer simbolo declarado (hover + symbolInfo).
            print("\n== Funciones del modulo ==")
            fns = lsp.functions(uri).get("functions", [])
            for f in fns:
                print("  -", f.get("name"))

            print("\n== Complejidad Big-O ==")
            for f in lsp.complexity(uri).get("functions", []):
                print("  %-12s total=%s" % (f.get("name"), f.get("total")))

            # Completado en la primera linea (offere keywords/tipos/builtins).
            print("\n== Completado (inicio del documento) ==")
            comp = lsp.completion(uri, line=0, character=0)
            items = comp if isinstance(comp, list) else comp.get("items", [])
            print("   %d items; primeros:" % len(items),
                  [it["label"] for it in items[:8]])
    except Exception as exc:  # noqa: BLE001
        sys.stderr.write("basic_usage: %s\n" % exc)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
