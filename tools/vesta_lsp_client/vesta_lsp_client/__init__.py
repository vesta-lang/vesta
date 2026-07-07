"""vesta_lsp_client: cliente Python sencillo para el LSP de Vesta.

Expone :class:`VestaLspClient`, una envoltura de alto nivel sobre el binario
``vesta_lsp`` que arranca el servidor, hace el handshake JSON-RPC y ofrece un
metodo por cada peticion soportada (hover, completado, diagnosticos, IR,
bytecode, asm JIT/AOT, diagramas, reporte de modos, etc.).

Uso minimo::

    from vesta_lsp_client import VestaLspClient

    with VestaLspClient("build/vesta_lsp.exe") as lsp:
        uri = lsp.open("programa.vx")            # abre y devuelve diagnosticos
        print(lsp.hover(uri, line=0, character=4))
        print(lsp.modes(uri))                     # interp / JIT / AOT

Ver el README para la referencia completa y mas ejemplos.
"""

from .client import (
    VestaLspClient,
    LspError,
    decode_semantic_tokens,
    discover_lsp,
)
from .highlight import iter_token_spans, render

__all__ = [
    "VestaLspClient",
    "LspError",
    "decode_semantic_tokens",
    "discover_lsp",
    "iter_token_spans",
    "render",
]
__version__ = "1.0.0"
