#!/usr/bin/env python3
"""Smoke test del resaltado semantico (semantic tokens) del LSP de Vesta.

Verifica que initialize anuncia semanticTokensProvider con legend, y que la
respuesta a semanticTokens/full trae un array "data" no vacio, multiplo de 5,
sin negativos, con los tokenType esperados (keyword/type/string/number/comment/
class/function).

    python tests/lsp/smoke_semantic_tokens.py <ruta-al-vesta_lsp[.exe]>
"""
from lsp_harness import (Report, check_bin, decode_semantic_data,
                         extract_data_array, m_did_open, m_exit, m_initialize,
                         m_initialized, m_request, m_shutdown, run_lsp)

lsp = check_bin()

SRC = ("// comentario de linea\n"
       "class Punto { i32 x; i32 y; }\n"
       "/* bloque\n multilinea */\n"
       "i32 area(Punto p) { return 42; }\n"
       'string saludo() { return "hola"; }')

out = run_lsp(lsp, [
    m_initialize(),
    m_initialized(),
    m_did_open("file:///sem.vx", SRC),
    m_request("textDocument/semanticTokens/full", 2,
              {"textDocument": {"uri": "file:///sem.vx"}}),
    m_shutdown(3),
    m_exit(),
])

r = Report("smoke_semantic_tokens")

r.check('"semanticTokensProvider"' in out and '"tokenTypes"' in out,
        "initialize anuncia semanticTokensProvider con legend",
        "initialize NO anuncio semanticTokensProvider/legend")

nums = extract_data_array(out)
if not nums:
    r.check(False, "", "no se encontro un array data no vacio en la respuesta")
    r.finish()

r.check(len(nums) > 0 and len(nums) % 5 == 0,
        "data tiene %d enteros (multiplo de 5)" % len(nums),
        "data no es multiplo de 5 (count=%d)" % len(nums))
r.check(all(x >= 0 for x in nums),
        "data sin valores negativos",
        "data contiene valores negativos")

# tokenType = 4o elemento de cada quinteto (idx 3).
types = set(nums[i + 3] for i in range(0, len(nums) - 4, 5))
for idx, name in [(13, "keyword"), (1, "type"), (16, "string"), (17, "number"),
                  (15, "comment"), (2, "class"), (11, "function")]:
    r.check(idx in types,
            "presente tokenType %s (idx=%d)" % (name, idx),
            "ausente tokenType %s (idx=%d)" % (name, idx))

r.finish()
