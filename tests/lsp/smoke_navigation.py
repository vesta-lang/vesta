#!/usr/bin/env python3
"""Smoke test de la navegacion del servidor LSP de Vesta.

Monta un mini-workspace (lib_mat.vx define 'cuadrado', main_use.vx la usa dos
veces), fija el rootUri y verifica:
  - hover sobre 'cuadrado' -> funcion + complejidad Big-O.
  - definition -> apunta a lib_mat.vx (cross-file).
  - references (includeDeclaration) -> >=3 Locations en los dos ficheros.

    python tests/lsp/smoke_navigation.py <ruta-al-vesta_lsp[.exe]>
"""
import os
import shutil

from lsp_harness import (Report, check_bin, file_uri, m_did_open, m_exit,
                         m_initialize, m_initialized, m_pos, m_shutdown,
                         resp_for_id, run_lsp)

lsp = check_bin()

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ws = os.path.join(SCRIPT_DIR, ".tmp_nav")
shutil.rmtree(ws, ignore_errors=True)
os.makedirs(ws)

with open(os.path.join(ws, "lib_mat.vx"), "w", encoding="utf-8", newline="\n") as fh:
    fh.write("i64 cuadrado(i64 x) {\n    return x * x;\n}\n")
MAIN = ("i64 main() {\n"
        "    i64 a = cuadrado(3);\n"
        "    i64 b = cuadrado(4);\n"
        "    return a + b;\n"
        "}\n")
with open(os.path.join(ws, "main_use.vx"), "w", encoding="utf-8", newline="\n") as fh:
    fh.write(MAIN)

main_uri = file_uri(os.path.join(ws, "main_use.vx"))
root_uri = file_uri(ws)

# La llamada 'cuadrado' esta en la linea 1 (0-based), col 13.
out = run_lsp(lsp, [
    m_initialize(root_uri=root_uri),
    m_initialized(),
    m_did_open(main_uri, MAIN),
    m_pos("textDocument/hover", main_uri, 1, 13, 2),
    m_pos("textDocument/definition", main_uri, 1, 13, 3),
    m_pos("textDocument/references", main_uri, 1, 13, 4,
          context={"includeDeclaration": True}),
    m_shutdown(9),
    m_exit(),
])

r = Report("smoke_navigation")

hover = resp_for_id(out, 2)
r.check("cuadrado" in hover and "funcion" in hover.lower() and "complejidad" in hover.lower(),
        "hover devuelve funcion + Big-O",
        "hover no devolvio funcion/Big-O esperados")

defin = resp_for_id(out, 3)
r.check("lib_mat.vx" in defin,
        "definition cross-file (lib_mat.vx)",
        "definition no apunto a lib_mat.vx")

refs = resp_for_id(out, 4)
uri_count = refs.count('"uri"')
r.check(uri_count >= 3 and "main_use.vx" in refs and "lib_mat.vx" in refs,
        "references cross-file (>=3 locations: %d, ambos ficheros)" % uri_count,
        "references insuficientes o no cross-file (count=%d)" % uri_count)

shutil.rmtree(ws, ignore_errors=True)
r.finish()
