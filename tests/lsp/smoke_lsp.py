#!/usr/bin/env python3
"""Smoke test end-to-end del servidor LSP de Vesta.

Envia initialize + initialized + didOpen de un .vx con ERROR + didOpen de un .vx
valido + shutdown + exit, y verifica:
  - respuesta a initialize (capabilities).
  - publishDiagnostics con >=1 diagnostico para el fichero malo.
  - publishDiagnostics con 0 diagnosticos para el bueno.

    python tests/lsp/smoke_lsp.py <ruta-al-vesta_lsp[.exe]>
"""
from lsp_harness import (Report, check_bin, m_did_open, m_exit, m_initialize,
                         m_initialized, m_shutdown, run_lsp)

lsp = check_bin()

# Fuente con error (identificador no declarado) y fuente valida sin diagnosticos.
BAD = "i64 main() { return noexiste_xyz + 1; }"
GOOD = "i64 main() { return 0; }"

out = run_lsp(lsp, [
    m_initialize(),
    m_initialized(),
    m_did_open("file:///bad.vx", BAD),
    m_did_open("file:///good.vx", GOOD),
    m_shutdown(2),
    m_exit(),
])

r = Report("smoke_lsp")

# 1) initialize respondido con capabilities.
r.check('"capabilities"' in out,
        "initialize respondido (capabilities presentes)",
        "no se vio respuesta a initialize con capabilities")

# 2) bad.vx publica >=1 diagnostico (su notificacion lleva "message").
bad_lines = [ln for ln in out.splitlines() if "bad.vx" in ln]
r.check("bad.vx" in out and any('"message"' in ln for ln in bad_lines),
        "bad.vx publica >=1 diagnostico",
        "bad.vx no publico diagnosticos")

# 3) good.vx publica 0 diagnosticos ("diagnostics":[]).
good_lines = [ln for ln in out.splitlines() if "good.vx" in ln]
r.check(any('"diagnostics":[]' in ln for ln in good_lines),
        "good.vx publica 0 diagnosticos",
        "good.vx no publico una lista de diagnosticos vacia")

r.finish()
