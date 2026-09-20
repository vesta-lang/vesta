#!/usr/bin/env python3
"""Smoke test: the dot offers what can actually be CALLED.

With uniform call syntax `q.area()` and `area(q)` are the same call, so a free
function whose first parameter is the receiver's type can be called through the
dot.  The editor has to offer it, or it shows half of what is callable and the
programmer has no way of knowing the rest exists -- the compiler accepts it,
the dot just never mentioned it.

The answer comes from the semantic index, which now records the head of each
symbol's first parameter, so this costs one map lookup rather than a walk over
every symbol in the module.

    python tests/lsp/smoke_ufcs_completion.py <path-to-vesta_lsp[.exe]>
"""
import os
import re
import shutil

from lsp_harness import (Report, check_bin, file_uri, m_completion, m_exit,
                         m_did_open, m_initialize, m_initialized, m_shutdown,
                         resp_for_id, run_lsp)

lsp = check_bin()
rep = Report("LSP: the dot offers what is callable (UFCS)")

WS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_ws_ufcs_comp")
shutil.rmtree(WS, ignore_errors=True)
os.makedirs(WS, exist_ok=True)

SRC = """struct Punto {
\ti64 x;
\ti64 y;

\ti64 doble() { return this.x * 2; }
}

i64 area(Punto p) { return p.x * p.y; }
i64 perimetro(Punto p) { return (p.x + p.y) * 2; }
i64 nada(i64 v) { return v; }

i32 main() {
\tPunto q;
\ti64 s = q.
\treturn 0;
}
"""
path = os.path.join(WS, "ufcs.vx")
with open(path, "w", encoding="utf-8", newline="\n") as fh:
    fh.write(SRC)
uri = file_uri(path)

lines = SRC.split("\n")
out = run_lsp(lsp, [
    m_initialize(root_uri=file_uri(WS)),
    m_initialized(),
    m_did_open(uri, SRC),
    m_completion(uri, 13, len(lines[13]), 5),  # right after `q.`
    m_shutdown(9),
    m_exit(),
])
answer = resp_for_id(out, 5)
labels = re.findall(r'"label":"([^"]*)"', answer)

rep.check("doble" in labels, "the type's own method is offered",
          "the method `doble` is missing: %s" % labels[:8])
rep.check("x" in labels and "y" in labels, "its fields are offered",
          "the fields are missing: %s" % labels[:8])
rep.check("area" in labels and "perimetro" in labels,
          "the reachable free functions are offered too",
          "`area`/`perimetro` are missing -- the dot shows half of what is "
          "callable: %s" % labels[:8])
# A free function that does NOT take a Punto first must not show up: the
# receiver is what decides, not the name being in scope.
rep.check("nada" not in labels,
          "a free function of another type is NOT offered",
          "`nada(i64)` leaked in: the receiver is not deciding")

shutil.rmtree(WS, ignore_errors=True)
rep.finish()
