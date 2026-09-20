#!/usr/bin/env python3
"""Smoke test: the builtin documentation is multi-language.

The text the editor shows on hover comes from `catalog/builtin_docs.toml`,
which holds it in every language, and the active one is picked from the
environment -- the same criterion the diagnostics use.

Written as a test because the failure mode is silent: with the docs hard-coded
in the server there was a single language and nothing said so; the hover just
always answered in Spanish.  Here the SAME symbol is asked for twice and the
two answers must differ.

    python tests/lsp/smoke_builtin_docs_lang.py <path-to-vesta_lsp[.exe]>
"""
import os
import shutil
import subprocess

from lsp_harness import (Report, check_bin, file_uri, frame, m_did_open,
                         m_exit, m_initialize, m_initialized, m_request,
                         m_shutdown, resp_for_id)

lsp = check_bin()
rep = Report("LSP: builtin docs are multi-language")

WS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_ws_docs_lang")
shutil.rmtree(WS, ignore_errors=True)
os.makedirs(WS, exist_ok=True)

SRC = "i32 main() {\n\tu64 n = type.size<i64>();\n\treturn 0;\n}\n"
path = os.path.join(WS, "lang.vx")
with open(path, "w", encoding="utf-8", newline="\n") as fh:
    fh.write(SRC)
uri = file_uri(path)

# `type.size` sits on line 1 (0-based); column 11 falls inside the name.
MSGS = [
    m_initialize(root_uri=file_uri(WS)),
    m_initialized(),
    m_did_open(uri, SRC),
    m_request("vesta/symbolInfo", 7, {"uri": uri, "line": 1, "character": 11}),
    m_shutdown(9),
    m_exit(),
]
DATA = b"".join(frame(m) for m in MSGS)


def doc_in(lang):
    """The `doc` field the server answers with VESTA_LANG set to @p lang."""
    env = dict(os.environ)
    env["VESTA_LANG"] = lang
    proc = subprocess.run([lsp], input=DATA, capture_output=True, timeout=120,
                          env=env)
    line = resp_for_id(proc.stdout.decode("utf-8", "replace"), 7)
    at = line.find('"doc":"')
    if at < 0:
        return ""
    return line[at + 7:line.find('"', at + 7)]


en = doc_in("en")
es = doc_in("es")

rep.check(bool(en), "the hover answers with documentation in English",
          "with VESTA_LANG=en the hover brought no documentation")
rep.check(bool(es), "the hover answers with documentation in Spanish",
          "with VESTA_LANG=es the hover brought no documentation")
# The point of the catalogue: the same symbol reads differently per language.
rep.check(bool(en) and bool(es) and en != es,
          "the two languages give different text (en=%r)" % en[:40],
          "both languages gave the SAME text: the catalogue is not being used")
# And the Spanish one is the text the catalogue holds, not a fallback.
rep.check("Tamano" in es or "tipo" in es,
          "the Spanish text is the one from the catalogue",
          "the Spanish text does not look like the catalogue entry: %r" % es[:60])

shutil.rmtree(WS, ignore_errors=True)
rep.finish()
