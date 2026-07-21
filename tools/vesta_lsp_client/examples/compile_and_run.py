#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compila un fuente Vesta y lo ejecuta, con salida vistosa.

Flujo:
  1. Abre el fichero en el LSP y muestra el FUENTE con resaltado de sintaxis.
  2. Lista sus diagnosticos (coloreados por severidad).
  3. Compila a ``.velb`` con el compilador EMBEBIDO del LSP (``vesta/compile``).
  4. Ejecuta el ``.velb`` con el binario ``vm`` (aislado) y muestra su salida y
     su valor de retorno (registro R00).

Uso:
    python examples/compile_and_run.py [--lsp <vesta_lsp>] [--vm <vm>]
                                       [--jit] [fichero.vx]
"""
import argparse
import os
import sys
import tempfile

import _console as con
from vesta_lsp_client import (VestaLspClient, VestaRunner,
                              decode_semantic_tokens)

DEMO = """\
// Factorial iterativo con un pequeno bucle.
i64 factorial(i64 n) {
    i64 acc = 1;
    i64 i = 2;
    while (i <= n) { acc = acc * i; i = i + 1; }
    return acc;
}

i64 main() {
    print("calculando factorial(5)...\\n");
    return factorial(5);
}
"""


def main():
    ap = argparse.ArgumentParser(description="Compila y ejecuta un fuente Vesta")
    ap.add_argument("file", nargs="?", help="fuente .vx (opcional; hay demo)")
    ap.add_argument("--lsp", default=None, help="ruta a vesta_lsp (auto)")
    ap.add_argument("--vm", default=None, help="ruta a vm (auto)")
    ap.add_argument("--jit", action="store_true", help="ejecutar con JIT")
    args = ap.parse_args()

    if args.file:
        with open(args.file, "r", encoding="utf-8") as fh:
            source = fh.read()
        name = args.file
    else:
        source, name = DEMO, "factorial_demo.vx"

    out_prefix = os.path.join(tempfile.gettempdir(),
                              "vxrun_" + str(os.getpid()))
    try:
        with VestaLspClient(args.lsp) as lsp:
            uri = lsp.open(name, text=source)

            con.title("Fuente (%s)" % os.path.basename(name))
            legend = lsp.semantic_token_legend()
            data = (lsp.semantic_tokens(uri) or {}).get("data", [])
            tokens = decode_semantic_tokens(data)
            print(con.highlight_source(source, tokens, legend))

            con.title("Diagnosticos")
            con.print_diagnostics(lsp.diagnostics(uri))

            con.title("Compilacion (compilador embebido del LSP)")
            mode = "jit" if args.jit else "vm"
            res = lsp.compile(uri, output=out_prefix, mode=mode)
            if not res.get("ok"):
                con.err("fallo la compilacion")
                con.print_diagnostics(res.get("diagnostics", []))
                if res.get("message"):
                    con.note(res["message"])
                return 1
            con.ok("compilado a " + os.path.basename(res["output"]))
            con.kv("modo", mode)
            con.kv("frontend", "%d us" % res.get("frontend_us", 0))

        con.title("Ejecucion (binario vm, aislado)")
        runner = VestaRunner(args.vm)
        rr = runner.run(res["output"], mode=mode, want_value=True)
        for line in rr.program_output.splitlines():
            if line.strip():
                con.note(line)
        con.kv("valor de retorno (R00)", rr.value, status="ok")
    except Exception as exc:  # noqa: BLE001
        con.err(str(exc))
        return 1
    finally:
        for ext in (".velb", ".vel", ".velb-map"):
            try:
                os.remove(out_prefix + ext)
            except OSError:
                pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
