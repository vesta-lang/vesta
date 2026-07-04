#!/usr/bin/env python3
"""Smoke test de la metaprogramacion en el LSP de Vesta.

Abre un .vx con una constante comptime entera (ANSWER=42), una derivada
(DOBLE = ANSWER*2) y un @Macro que genera codigo Vesta, y verifica las dos
peticiones a medida:
  - vesta/macroExpand    -> expansions + skipped, con "doblar(21)" generado.
  - vesta/comptimeValues -> values con ANSWER=42 y DOBLE=84.

    python tests/lsp/smoke_metaprog.py <ruta-al-vesta_lsp[.exe]>
"""
from lsp_harness import (Report, check_bin, m_did_open, m_exit, m_initialize,
                         m_initialized, m_request, m_shutdown, resp_for_id,
                         run_lsp)

lsp = check_bin()

URI = "file:///metaprog_demo.vx"
SRC = ("comptime i64 ANSWER = 42;\n"
       "comptime i64 DOBLE = ANSWER * 2;\n"
       "i32 doblar(i32 x) { return x * 2; }\n"
       "@Macro\n"
       'comptime string macro_double(i64 n) { return "doblar(" + to_str(n) + ")"; }\n'
       "i32 main() { i32 r1 = macro_double(21); if (r1 != 42) return 91; return 42; }")

out = run_lsp(lsp, [
    m_initialize(),
    m_initialized(),
    m_did_open(URI, SRC),
    m_request("vesta/macroExpand", 20, {"uri": URI}),
    m_request("vesta/comptimeValues", 21, {"uri": URI}),
    m_shutdown(99),
    m_exit(),
])

r = Report("smoke_metaprog")


def check_id(id_, pattern, desc):
    r.check(pattern in resp_for_id(out, id_), desc, desc)


# 0) initialize anuncia los metodos nuevos.
r.check("vesta/macroExpand" in out and "vesta/comptimeValues" in out,
        "initialize anuncia macroExpand + comptimeValues",
        "initialize no anuncio los metodos nuevos")

# 1) macroExpand.
check_id(20, '"expansions"', "vesta/macroExpand devuelve expansions")
check_id(20, '"skipped"', "vesta/macroExpand devuelve skipped")
check_id(20, "doblar(21)", "vesta/macroExpand incluye el codigo generado")

# 2) comptimeValues.
check_id(21, '"values"', "vesta/comptimeValues devuelve values")
check_id(21, "ANSWER", "vesta/comptimeValues incluye la constante ANSWER")
check_id(21, '"42"', "vesta/comptimeValues muestra el valor 42")
check_id(21, '"84"', "vesta/comptimeValues computa DOBLE = 84")

r.finish()
