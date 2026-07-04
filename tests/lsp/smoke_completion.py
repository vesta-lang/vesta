#!/usr/bin/env python3
"""Smoke test del autocompletado del servidor LSP de Vesta.

Monta un mini-workspace con comp.vx (clase Punto + funcion distancia + main con
puntos de insercion), fija el rootUri y verifica:
  - completion GENERAL (prefijo "dis") incluye el simbolo 'distancia' con kind.
  - completion de MIEMBRO tras "p." (p:Punto) devuelve un result valido (y
    'suma' si la heuristica del tipo del receptor resuelve; best-effort).

    python tests/lsp/smoke_completion.py <ruta-al-vesta_lsp[.exe]>
"""
import os
import shutil

from lsp_harness import (Report, check_bin, file_uri, m_completion, m_exit,
                         m_did_open, m_initialize, m_initialized, m_shutdown,
                         resp_for_id, run_lsp)

lsp = check_bin()

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ws = os.path.join(SCRIPT_DIR, ".tmp_comp")
shutil.rmtree(ws, ignore_errors=True)
os.makedirs(ws)

DOC = ("class Punto {\n"
       "    public i64 x;\n"
       "    public i64 y;\n"
       "    public i64 suma() { return this.x + this.y; }\n"
       "    public i64 escala(i64 k) { return this.x * k; }\n"
       "}\n"
       "\n"
       "i64 distancia(i64 a) {\n"
       "    return a * a;\n"
       "}\n"
       "\n"
       "i64 main() {\n"
       "    Punto p = new Punto();\n"
       "    i64 d = dis\n"
       "    return p.su\n"
       "}\n")
with open(os.path.join(ws, "comp.vx"), "w", encoding="utf-8", newline="\n") as fh:
    fh.write(DOC)

doc_uri = file_uri(os.path.join(ws, "comp.vx"))

# Linea 13 (0-based) = "    i64 d = dis" -> prefijo "dis" en col 15 (general).
# Linea 14 = "    return p.su" -> tras "p." con "su" en col 15 (miembro).
out = run_lsp(lsp, [
    m_initialize(root_uri=file_uri(ws)),
    m_initialized(),
    m_did_open(doc_uri, DOC),
    m_completion(doc_uri, 13, 15, 2),
    m_completion(doc_uri, 14, 15, 3),
    m_shutdown(9),
    m_exit(),
])

r = Report("smoke_completion")

gen = resp_for_id(out, 2)
r.check("distancia" in gen and '"kind"' in gen,
        "completion general filtra por prefijo (incluye 'distancia')",
        "completion general no incluyo el simbolo esperado")
r.check('"result"' in gen,
        "completion general devolvio un result valido",
        "completion general sin result")

mem = resp_for_id(out, 3)
if '"result"' in mem:
    print("OK  completion de miembro devolvio un result valido (sin crash)")
    if "suma" in mem:
        print("OK  completion de miembro resolvio el tipo del receptor ('suma')")
    else:
        print("NOTA  completion de miembro no resolvio el tipo (best-effort)")
else:
    print("FALLO  completion de miembro sin result")
    r.fail = 1

shutil.rmtree(ws, ignore_errors=True)
r.finish()
