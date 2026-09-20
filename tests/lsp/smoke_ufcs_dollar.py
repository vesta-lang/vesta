#!/usr/bin/env python3
"""Smoke test: what the LSP offers for the UFCS spellings.

Three completions that the tree-shaped builtin names and the qualified-call
syntax made necessary:

  - after `$` (`x.f$`), ONLY namespaces may follow, so that is what must be
    offered -- not types, not variables, not the receiver's members.
  - after a builtin family root (`type.`), the next segment of the tree:
    `size`, `align`, `name`...  A flat list cannot do this, and it is the
    reason the names are a tree.
  - after an intermediate branch (`scoped.`), the branch itself (`method`),
    which is not callable on its own.

    python tests/lsp/smoke_ufcs_dollar.py <path-to-vesta_lsp[.exe]>
"""
import os
import shutil

from lsp_harness import (Report, check_bin, file_uri, m_completion, m_exit,
                         m_did_open, m_initialize, m_initialized, m_shutdown,
                         resp_for_id, run_lsp)

lsp = check_bin()
rep = Report("LSP: completion for the UFCS spellings")

WS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_ws_dollar")
shutil.rmtree(WS, ignore_errors=True)
os.makedirs(WS, exist_ok=True)

# `geo.metrico` is a real namespace of the document, so it must show up after
# the `$`.
SRC = """namespace geo.metrico;

public i64 doble(i64 v) => v * 2;

i32 main() {
\ti64 a = 3;
\ti64 b = a.doble$
\tu64 c = type.
\tu32 d = scoped.
\treturn 0;
}
"""
path = os.path.join(WS, "dollar.vx")
with open(path, "w", encoding="utf-8") as fh:
    fh.write(SRC)
uri = file_uri(path)

# Lines are 0-based.  6 = `i64 b = a.doble$`, 7 = `u64 c = type.`,
# 8 = `u32 d = scoped.` -- the cursor goes at the end of each one.
lines = SRC.split("\n")
msgs = [
    m_initialize(1, file_uri(WS)),
    m_initialized(),
    m_did_open(uri, SRC),
    m_completion(uri, 6, len(lines[6]), 10),  # after `$`
    m_completion(uri, 7, len(lines[7]), 11),  # after `type.`
    m_completion(uri, 8, len(lines[8]), 12),  # after `scoped.`
    m_shutdown(),
    m_exit(),
]
out = run_lsp(lsp, msgs)


def has_label(resp_id, label):
    """True if the answer with that id offers exactly that label."""
    return ('"label":"%s"' % label) in resp_for_id(out, resp_id)


# 1. After `$`: namespaces, and ONLY namespaces.
rep.check(has_label(10, "geo.metrico"),
          "after `$` the namespace `geo.metrico` is offered",
          "after `$` the namespace `geo.metrico` is missing")
# `doble` is a function of that namespace, not a namespace: it must not be
# here -- what follows a `$` can only be a namespace.
rep.check(not has_label(10, "doble"),
          "after `$` only namespaces: the function `doble` is not offered",
          "after `$` a function leaked into the list")

# 2. After `type.`: the leaves of that family.
rep.check(has_label(11, "size") and has_label(11, "align"),
          "after `type.` the leaves `size` and `align` are offered",
          "after `type.` the leaves `size`/`align` are missing")
# The full name must NOT be offered: the editor replaces the prefix, not the
# receiver, so `type.size` there would end up written `type.type.size`.
rep.check(not has_label(11, "type.size"),
          "after `type.` the segment is offered, not the whole name",
          "after `type.` the whole name was offered: it duplicates the root")

# 3. After `scoped.`: the intermediate branch.
rep.check(has_label(12, "method"),
          "after `scoped.` the branch `method` is offered",
          "after `scoped.` the branch `method` is missing")

shutil.rmtree(WS, ignore_errors=True)
rep.finish()
