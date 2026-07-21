# SPDX-License-Identifier: MIT
"""Instalador de vesta_lsp_client (la libreria cliente del LSP de Vesta).

Instala UNICAMENTE la libreria; los scripts de ``examples/`` no se instalan
(son consumidores de la libreria, no forman parte de ella)::

    pip install .            # desde este directorio
    pip install -e .         # instalacion editable (desarrollo)

Tras instalar, la libreria se importa desde cualquier sitio::

    python -c "from vesta_lsp_client import VestaLspClient"

Los ejemplos se ejecutan directamente contra la libreria instalada::

    python examples/vxcat.py programa.vx
"""

import os

from setuptools import find_packages, setup

_here = os.path.abspath(os.path.dirname(__file__))
try:
    with open(os.path.join(_here, "README.md"), encoding="utf-8") as _fh:
        _long_desc = _fh.read()
except OSError:
    _long_desc = "Cliente del LSP de Vesta."

setup(
    name="vesta-lsp-client",
    version="1.0.0",
    description="Cliente portable para el LSP de Vesta "
                "(vesta_lsp).",
    long_description=_long_desc,
    long_description_content_type="text/markdown",
    author="David Lopez.T (DesmonHak)",
    license="MIT",
    url="https://github.com/vesta-lang/vesta",
    packages=find_packages(exclude=("examples", "examples.*")),
    python_requires=">=3.7",
    install_requires=[],  # solo libreria estandar de Python.
    classifiers=[
        "License :: OSI Approved :: MIT License",
        "Programming Language :: Python :: 3",
        "Operating System :: OS Independent",
        "Topic :: Software Development :: Compilers",
        "Topic :: Text Processing :: Linguistic",
    ],
    keywords="vesta lsp language-server jsonrpc compiler syntax-highlighting",
)
