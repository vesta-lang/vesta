#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compila un PROYECTO Vesta multi-fichero (con ``import``) y lo ejecuta.

Si no se pasa un fichero raiz, crea un proyecto de demostracion de dos modulos
en un directorio temporal (un modulo de utilidades + el ``main`` que lo importa)
y lo compila con ``vesta/compileProject`` (compilador embebido del LSP).

Uso:
    python examples/project_build.py [--lsp <vesta_lsp>] [--vm <vm>] [raiz.vx]
"""
import argparse
import os
import shutil
import sys
import tempfile

import _console as con
from vesta_lsp_client import VestaLspClient, VestaRunner

MODULE_MATH = """\
// Modulo de utilidades matematicas.
public i64 cuadrado(i64 x) { return x * x; }
public i64 suma(i64 a, i64 b) { return a + b; }
"""

MODULE_MAIN = """\
import "mathutil" only suma, cuadrado;

i64 main() {
    print("proyecto multi-modulo\\n");
    return suma(cuadrado(5), cuadrado(4));   // 25 + 16 = 41
}
"""


def make_demo_project():
    """Crea un proyecto demo de 2 modulos; devuelve (dir, ruta_del_root)."""
    d = tempfile.mkdtemp(prefix="vxproj_")
    with open(os.path.join(d, "mathutil.vx"), "w", encoding="utf-8") as fh:
        fh.write(MODULE_MATH)
    root = os.path.join(d, "main.vx")
    with open(root, "w", encoding="utf-8") as fh:
        fh.write(MODULE_MAIN)
    return d, root


def main():
    ap = argparse.ArgumentParser(description="Compila un proyecto Vesta")
    ap.add_argument("root", nargs="?", help=".vx raiz (opcional; hay demo)")
    ap.add_argument("--lsp", default=None, help="ruta a vesta_lsp (auto)")
    ap.add_argument("--vm", default=None, help="ruta a vm (auto)")
    args = ap.parse_args()

    tmp_dir = None
    if args.root:
        root = args.root
    else:
        tmp_dir, root = make_demo_project()

    out_prefix = os.path.join(tempfile.gettempdir(),
                              "vxproj_out_" + str(os.getpid()))
    try:
        con.title("Proyecto")
        con.kv("raiz", root)
        if tmp_dir:
            con.kv("modulos", ", ".join(sorted(os.listdir(tmp_dir))))

        with VestaLspClient(args.lsp) as lsp:
            uri = lsp.open(root)
            con.title("Compilacion del proyecto (embebida)")
            res = lsp.compile_project(uri, output=out_prefix)
            if not res.get("ok"):
                con.err("fallo la compilacion del proyecto")
                con.print_diagnostics(res.get("diagnostics", []))
                if res.get("message"):
                    con.note(res["message"])
                return 1
            con.ok("compilado a " + os.path.basename(res["output"]))
            con.kv("project", res.get("project"))
            con.kv("frontend", "%d us" % res.get("frontend_us", 0))

        con.title("Ejecucion")
        runner = VestaRunner(args.vm)
        rr = runner.run(res["output"], want_value=True)
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
        if tmp_dir:
            shutil.rmtree(tmp_dir, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
