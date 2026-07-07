#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Ejemplo avanzado: inspeccion multi-modo, asm nativo y diagramas.

Uso:
    python examples/inspect_modes.py [--lsp <vesta_lsp>] [fichero.vx]

Si no se da fichero, usa un fuente de demostracion incrustado.  El binario del
LSP se auto-detecta (PATH + rutas de instalacion); usa --lsp para forzarlo.

Demuestra:
  - vesta/modes         (reporte en interprete / JIT / AOT).
  - vesta/jitAsm        (desensamblado del codigo JIT de una funcion).
  - vesta/aotAsm por OS/arch (Windows x86-64 vs Linux x86-32).
  - vesta/diagram kind=asm (CFG del codigo nativo).
  - vesta/diagram kind=types (classDiagram de la POO).
"""
import argparse
import sys

import _console as con
from vesta_lsp_client import VestaLspClient

DEMO = """\
i64 clasifica(i64 n) {
    i64 r = 0;
    if (n < 0) { r = 0 - n; } else { r = n; }
    i64 i = 0;
    while (i < r) { i = i + 1; }
    return i;
}

class Punto { public i64 x; public i64 getX() { return this.x; } }

i64 main() { return clasifica(-5); }
"""


def pick_demo_function(lsp, uri, modes_report):
    """Elige una funcion 'de usuario' para las vistas asm/CFG.

    Prefiere una que el JIT sepa compilar (para que el desensamblado y el CFG
    no salgan vacios); si no hay, cae a la primera funcion no sintetica.
    """
    jit = next((m for m in modes_report.get("modes", [])
                if m.get("mode") == "jit"), {})
    for name in jit.get("compilable_functions", []):
        if name and not name.startswith("__"):
            return name
    for fn in lsp.functions(uri).get("functions", []):
        name = fn.get("name", "")
        if name and not name.startswith("__"):
            return name
    return ""


def main():
    ap = argparse.ArgumentParser(
        description="Inspeccion multi-modo (interp/JIT/AOT) + asm + diagramas")
    ap.add_argument("file", nargs="?", help="fuente .vx (opcional; hay demo)")
    ap.add_argument("--lsp", default=None,
                    help="ruta a vesta_lsp (por defecto: auto-detectar)")
    args = ap.parse_args()

    if args.file:
        with open(args.file, "r", encoding="utf-8") as fh:
            source = fh.read()
        name = args.file
    else:
        source, name = DEMO, "modes_demo.vx"

    try:
        with VestaLspClient(args.lsp) as lsp:
            uri = lsp.open(name, text=source)
            report = lsp.modes(uri)
            fn = pick_demo_function(lsp, uri, report)

            con.title("Modos (interp / JIT / AOT)")
            for m in report.get("modes", []):
                con.kv(m["mode"], m.get("note", ""), status="accent")
                if m["mode"] == "jit":
                    con.note("compilables: %s" % m.get("compilable_functions"))
                    con.note("fallback   : %s" % m.get("fallback_functions"))
                if m["mode"] == "aot":
                    con.note("compatible : %s | ok: %s"
                             % (m.get("compatible"), m.get("ok_functions")))

            if fn:
                con.title("asm AOT de '%s' por OS/arch" % fn)
                for os_, arch in (("windows", "x86-64"), ("linux", "x86-32")):
                    r = lsp.aot_asm(uri, function=fn, os_=os_, arch=arch)
                    body = (r.get("text", "") or r.get("reason", "")
                            or r.get("error", ""))
                    head = body.splitlines()[:1]
                    con.kv("%s %s" % (os_, arch),
                           head[0] if head else "(vacio)")

                con.title("CFG del codigo nativo de '%s' (mermaid)" % fn)
                cfg = lsp.diagram(uri, kind="asm", fmt="mermaid", function=fn)
                for ln in cfg.get("text", "").splitlines()[:6]:
                    con.note(ln)

            con.title("Diagrama de tipos (classDiagram)")
            ty = lsp.diagram(uri, kind="types", fmt="mermaid")
            for ln in ty.get("text", "").splitlines()[:10]:
                con.note(ln)
    except Exception as exc:  # noqa: BLE001
        sys.stderr.write("inspect_modes: %s\n" % exc)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
