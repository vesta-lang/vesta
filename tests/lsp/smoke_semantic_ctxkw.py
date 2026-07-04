#!/usr/bin/env python3
"""Smoke test del resaltado de keywords contextuales, builtins y parametros de
plantilla en el LSP de Vesta.  Decodifica el array plano "data" a posiciones
absolutas y comprueba el tokenType en posiciones conocidas:
  - `comptime`      -> Keyword       (idx 13)  en 0:0
  - `T` (en <T>)    -> TypeParameter (idx 6)   en 0:10
  - `static_assert` -> Function      (idx 11)  en 1:13
  - `normal`        -> Variable      (idx 8)   en 1:44

    python tests/lsp/smoke_semantic_ctxkw.py <ruta-al-vesta_lsp[.exe]>
"""
from lsp_harness import (Report, check_bin, decode_semantic_data,
                         extract_data_array, m_did_open, m_exit, m_initialize,
                         m_initialized, m_request, m_shutdown, run_lsp)

lsp = check_bin()

# L0: comptime <T> u32 vec_dim() { return 4; }
# L1: i32 main() { static_assert(true, "ok"); i32 normal = 1; return 0; }
SRC = ('comptime <T> u32 vec_dim() { return 4; }\n'
       'i32 main() { static_assert(true, "ok"); i32 normal = 1; return 0; }')

out = run_lsp(lsp, [
    m_initialize(),
    m_initialized(),
    m_did_open("file:///ctxkw.vx", SRC),
    m_request("textDocument/semanticTokens/full", 2,
              {"textDocument": {"uri": "file:///ctxkw.vx"}}),
    m_shutdown(3),
    m_exit(),
])

r = Report("smoke_semantic_ctxkw")

nums = extract_data_array(out)
if not nums:
    r.check(False, "", "no se encontro un array data no vacio en la respuesta")
    r.finish()

decoded = decode_semantic_data(nums)
pos_type = {(ln, cl): typ for (ln, cl, typ) in decoded}


def check_at(ln, cl, want, label):
    got = pos_type.get((ln, cl))
    r.check(got == want,
            "%s en %d:%d = tokenType %s" % (label, ln, cl, got),
            "%s en %d:%d: esperado %d, obtenido '%s'" % (label, ln, cl, want, got))


check_at(0, 0, 13, "comptime (Keyword)")
check_at(0, 10, 6, "T (TypeParameter)")
check_at(1, 13, 11, "static_assert (Function)")
check_at(1, 44, 8, "normal (Variable)")

r.finish()
