# SPDX-License-Identifier: MIT
"""Utilidades de consola compartidas por los EJEMPLOS (no por la libreria).

Reune el color ANSI de la salida (cabeceras, etiquetas, ok/error) y el esquema
de resaltado de sintaxis, para que los ejemplos sean vistosos y su salida se
distinga de un vistazo.  Los colores viven aqui (en los ejemplos), no en la
libreria: la libreria solo entrega spans tipados y cada consumidor elige su
paleta.
"""
import os
import sys

from vesta_lsp_client import render

# --- Paleta de la salida de los ejemplos --------------------------------------
RESET = "\033[0m"
BOLD = "\033[1m"
DIM = "\033[2m"
_C = {
    "title": "\033[1;38;5;81m",   # cian brillante, negrita
    "key": "\033[38;5;250m",      # gris claro
    "val": "\033[38;5;223m",      # arena
    "ok": "\033[1;38;5;114m",     # verde
    "warn": "\033[1;38;5;215m",   # naranja
    "err": "\033[1;38;5;203m",    # rojo
    "note": "\033[38;5;244m",     # gris
    "accent": "\033[38;5;177m",   # violeta
}

# --- Esquema de resaltado de sintaxis del ejemplo -----------------------------
SYNTAX = {
    "keyword": "\033[38;5;204m", "function": "\033[38;5;220m",
    "method": "\033[38;5;220m", "class": "\033[38;5;80m",
    "struct": "\033[38;5;80m", "enum": "\033[38;5;80m",
    "interface": "\033[38;5;114m", "type": "\033[38;5;80m",
    "typeParameter": "\033[38;5;79m", "namespace": "\033[38;5;80m",
    "enumMember": "\033[38;5;117m", "string": "\033[38;5;114m",
    "number": "\033[38;5;215m", "comment": "\033[38;5;244m",
    "macro": "\033[38;5;177m", "operator": "\033[38;5;252m",
    "modifier": "\033[38;5;75m", "parameter": "\033[38;5;252m",
    "variable": "\033[38;5;252m", "property": "\033[38;5;153m",
    "register": "\033[38;5;203m", "escapeSequence": "\033[38;5;120m",
    "interpolation": "\033[38;5;220m",
}

_use_color = None


def color_enabled():
    """Decide (una vez) si usar color: TTY y sin ``NO_COLOR``.

    ``VESTA_FORCE_COLOR=1`` lo fuerza (util al canalizar la salida a un pager).
    """
    global _use_color
    if _use_color is None:
        _use_color = (os.environ.get("VESTA_FORCE_COLOR") == "1"
                      or (sys.stdout.isatty()
                          and os.environ.get("NO_COLOR") is None))
        if _use_color and os.name == "nt":
            try:
                import ctypes
                k = ctypes.windll.kernel32
                k.SetConsoleMode(k.GetStdHandle(-11), 7)
            except Exception:
                pass
    return _use_color


def _wrap(code, text):
    return (code + text + RESET) if color_enabled() else text


def title(text):
    """Imprime una cabecera de seccion resaltada."""
    print()
    print(_wrap(_C["title"], "== " + text + " =="))


def kv(key, value, status=None):
    """Imprime `clave: valor`; `status` in {ok,warn,err,note} colorea el valor."""
    k = _wrap(_C["key"], "  " + str(key) + ":")
    code = _C.get(status or "val", _C["val"])
    print("%s %s" % (k, _wrap(code, str(value))))


def ok(msg):
    print(_wrap(_C["ok"], "  [OK] ") + str(msg))


def warn(msg):
    print(_wrap(_C["warn"], "  [!] ") + str(msg))


def err(msg):
    print(_wrap(_C["err"], "  [x] ") + str(msg))


def note(msg):
    print(_wrap(_C["note"], "  " + str(msg)))


def ansi_style(name):
    """Funcion de estilo para :func:`vesta_lsp_client.render` (paleta SYNTAX)."""
    code = SYNTAX.get(name)
    return (code, RESET) if (code and color_enabled()) else ("", "")


def highlight_source(text, tokens, legend, gutter=True):
    """Devuelve `text` con resaltado de sintaxis (y numeros de linea opcionales)."""
    body = render(text, tokens, legend, ansi_style) if color_enabled() else text
    if not gutter:
        return body
    out = []
    for i, line in enumerate(body.split("\n"), 1):
        num = _wrap(_C["note"], "%4d " % i)
        out.append(num + "| " + line)
    return "\n".join(out)


def print_diagnostics(diags):
    """Imprime una lista de diagnosticos del LSP/compilador con color."""
    if not diags:
        ok("sin diagnosticos")
        return
    for d in diags:
        lvl = d.get("level") or (
            {1: "error", 2: "warning", 3: "info", 4: "hint"}
            .get(d.get("severity"), "info"))
        rng = d.get("range", {}).get("start", {})
        line = d.get("line", rng.get("line", "?"))
        col = d.get("column", rng.get("character", "?"))
        loc = "%s:%s" % (line, col)
        msg = d.get("message", "")
        code = {"error": _C["err"], "warning": _C["warn"]}.get(lvl, _C["note"])
        print("  %s %s %s" % (_wrap(code, lvl.upper()),
                              _wrap(_C["key"], loc), msg))
