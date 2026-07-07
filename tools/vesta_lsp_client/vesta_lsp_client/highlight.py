# SPDX-License-Identifier: MIT
"""Utilidades para partir un fuente en *spans* tipados a partir de semantic
tokens del LSP.

La libreria **no** impone ningun esquema de color ni formato de salida: cada
programa que la use decide como resaltar (ANSI en la terminal, ``<span>`` en
HTML, tags de un editor, ...).  Aqui solo se ofrece la maquinaria generica que
mapea los tokens a fragmentos de texto etiquetados con el NOMBRE de su tipo:

  - :func:`iter_token_spans` -- itera ``(fragmento, tipo)`` cubriendo todo el
    texto en orden (``tipo`` vacio para los huecos sin token).
  - :func:`render` -- reconstruye el texto envolviendo cada fragmento con el
    prefijo/sufijo que devuelva la funcion de estilo del llamante.

El nombre de tipo procede de la leyenda del servidor
(:meth:`vesta_lsp_client.VestaLspClient.semantic_token_legend`): p.ej.
``keyword``, ``function``, ``class``, ``string``, ``comment``, ``register``...
"""

from __future__ import annotations

from typing import Callable, Iterator, List, Sequence, Tuple

# Un token decodificado: (linea, columna, longitud, tipo, modificadores).
Token = Tuple[int, int, int, int, int]
# Un span de salida: (fragmento de texto, nombre del tipo o "" si sin token).
Span = Tuple[str, str]
# Funcion de estilo del consumidor: nombre de tipo -> (prefijo, sufijo).
StyleFn = Callable[[str], Tuple[str, str]]


def _u16_to_index(line: str, u16_col: int) -> int:
    """Convierte una columna LSP (unidades UTF-16) a indice de caracter Python.

    Las posiciones LSP se miden en unidades de codigo UTF-16; los caracteres
    fuera del BMP (emojis) ocupan 2.  Para fuentes ASCII coincide 1:1.
    """
    units = 0
    for i, ch in enumerate(line):
        if units >= u16_col:
            return i
        units += 2 if ord(ch) > 0xFFFF else 1
    return len(line)


def iter_token_spans(
    text: str,
    tokens: Sequence[Token],
    legend: Sequence[str],
) -> Iterator[Span]:
    """Itera el texto como ``(fragmento, nombre_de_tipo)`` en orden.

    Cubre todo el fuente: los tramos sin token (espacios, puntuacion no
    tokenizada, saltos de linea) se emiten con tipo ``""``.  El consumidor
    decide como pintar cada tipo; la libreria no elige colores.

    :param text: fuente original.
    :param tokens: lista ``(linea, col, longitud, tipo, mods)`` absoluta, tal
        como la devuelve :func:`vesta_lsp_client.decode_semantic_tokens`.
    :param legend: ``tokenTypes`` del servidor (indice -> nombre).
    :yields: tuplas ``(fragmento, nombre_de_tipo)``.
    """
    lines = text.split("\n")
    by_line: dict = {}
    for (ln, col, length, ttype, _mods) in tokens:
        by_line.setdefault(ln, []).append((col, length, ttype))
    for li, src in enumerate(lines):
        if li > 0:
            yield ("\n", "")
        spans = sorted(by_line.get(li, []))
        idx = 0
        for (col, length, ttype) in spans:
            start = _u16_to_index(src, col)
            end = _u16_to_index(src, col + length)
            if start < idx:      # solape defensivo: saltar.
                continue
            if start > idx:
                yield (src[idx:start], "")
            name = legend[ttype] if 0 <= ttype < len(legend) else ""
            yield (src[start:end], name)
            idx = end
        if idx < len(src):
            yield (src[idx:], "")


def render(
    text: str,
    tokens: Sequence[Token],
    legend: Sequence[str],
    style: StyleFn,
) -> str:
    """Reconstruye @p text aplicando el estilo del llamante a cada span.

    No hay colores por defecto: @p style es una funcion que, dado el nombre de
    un tipo de token, devuelve el par ``(prefijo, sufijo)`` con el que envolver
    ese fragmento.  Ejemplos::

        # ANSI en terminal
        ANSI = {"keyword": "\\033[35m", "string": "\\033[32m"}
        render(text, toks, legend,
               lambda n: (ANSI.get(n, ""), "\\033[0m" if n in ANSI else ""))

        # HTML
        render(text, toks, legend,
               lambda n: ("<span class='%s'>" % n, "</span>") if n else ("", ""))

    :param style: ``nombre_de_tipo -> (prefijo, sufijo)``.  Para los huecos sin
        token el nombre es ``""``.
    :returns: el texto con los prefijos/sufijos insertados.
    """
    out: List[str] = []
    for frag, name in iter_token_spans(text, tokens, legend):
        prefix, suffix = style(name)
        out.append(prefix)
        out.append(frag)
        out.append(suffix)
    return "".join(out)
