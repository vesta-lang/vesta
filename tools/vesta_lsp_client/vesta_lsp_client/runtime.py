# SPDX-License-Identifier: MIT
"""Ejecucion de programas Vesta compilados (``.velb``) via el binario ``vm``.

El servidor LSP COMPILA (embebe el compilador) pero no debe EJECUTAR programas
en su propio proceso: el ``print`` del programa escribiria en el stdout del LSP
y romperia el canal JSON-RPC.  Por eso la ejecucion se hace lanzando el binario
``vm`` como subproceso aislado, que ademas captura la salida del programa.

:class:`VestaRunner` envuelve esa invocacion y expone el codigo de salida, la
salida estandar/estandar de error y, opcionalmente, el valor de retorno del
programa (registro ``R00``).
"""

from __future__ import annotations

import os
import re
import subprocess
from typing import List, Optional

from .client import LspError, find_binary


def _vm_names() -> List[str]:
    """Nombres candidatos del binario principal de la VM segun el SO."""
    if os.name == "nt":
        return ["vm.exe", "vesta.exe", "vm", "vesta"]
    return ["vm", "vesta"]


def discover_vm(explicit: Optional[str] = None) -> Optional[str]:
    """Localiza el binario ``vm`` (VM/compilador) de forma portable.

    Mismo motor que :func:`vesta_lsp_client.discover_lsp` pero buscando el
    ejecutable principal: ``explicit`` -> variable de entorno ``VESTA_VM`` ->
    ``PATH`` -> raices de instalacion (``bin/``) -> builds del repo.

    :param explicit: ruta candidata prioritaria.
    :returns: ruta absoluta o ``None``.
    """
    return find_binary(_vm_names(), env_var="VESTA_VM",
                       subdirs=["bin", ""], explicit=explicit)


class RunResult:
    """Resultado de ejecutar un programa Vesta.

    :ivar exit_code: codigo de salida del PROCESO ``vm`` (0 si la VM termino
        bien; nota: NO es el valor de retorno del programa, ver :attr:`value`).
    :ivar stdout: salida estandar capturada del programa.
    :ivar stderr: salida de error capturada.
    :ivar value: valor de retorno del programa (registro ``R00`` truncado a
        64 bits), o ``None`` si no se pidio/parseo.
    """

    def __init__(self, exit_code: int, stdout: str, stderr: str,
                 value: Optional[int]):
        self.exit_code = exit_code
        self.stdout = stdout
        self.stderr = stderr
        self.value = value

    #: Marcadores donde empieza el volcado de ``--stats`` (dump de registros y
    #: schedulers).  Todo lo anterior es la salida real del programa.
    _STATS_MARKERS = ("=== RUN STATS ===", "=== JIT STATS ===",
                      "[Scheduler ", "[Process ", "PID LOCAL=",
                      "Estados de procesos")

    @property
    def program_output(self) -> str:
        """Salida del programa SIN el volcado de ``--stats`` (si se pidio valor).

        Si se ejecuto con ``want_value=True`` la VM anyade al stdout un dump de
        registros/schedulers; esta propiedad devuelve solo lo que imprimio el
        programa (todo hasta el primer marcador de stats).
        """
        lines = self.stdout.splitlines()
        cut = len(lines)
        for i, ln in enumerate(lines):
            s = ln.lstrip()
            if any(s.startswith(m) or m in ln for m in self._STATS_MARKERS):
                cut = i
                break
        return "\n".join(lines[:cut])

    def __repr__(self) -> str:
        return ("RunResult(exit_code=%d, value=%s, stdout=%d chars)"
                % (self.exit_code, self.value, len(self.stdout)))


_R00_RE = re.compile(r"R00=0x([0-9a-fA-F]+)")


class VestaRunner:
    """Ejecuta programas ``.velb`` lanzando el binario ``vm`` como subproceso.

    :param vm_path: ruta al binario ``vm``; ``None`` = auto-detectar con
        :func:`discover_vm`.
    :raises LspError: si no se encuentra el binario.
    """

    def __init__(self, vm_path: Optional[str] = None):
        resolved = discover_vm(vm_path)
        if not resolved:
            raise LspError(
                "no se encontro el binario 'vm'. Pasa la ruta, define VESTA_VM "
                "o anyadelo al PATH.")
        self._bin = resolved

    @property
    def binary(self) -> str:
        """Ruta absoluta del binario ``vm`` en uso."""
        return self._bin

    def run(self, program: str, *, args: Optional[List[str]] = None,
            mode: str = "vm", schedulers: int = 1, want_value: bool = True,
            timeout: Optional[float] = 60.0) -> RunResult:
        """Ejecuta un ``.velb`` y devuelve su resultado.

        :param program: ruta al ``.velb``.
        :param args: argumentos que recibe el programa (``args_get`` en Vesta).
        :param mode: ``"vm"`` (interprete) o ``"jit"`` (JIT en caliente).
        :param schedulers: numero de schedulers de la VM.
        :param want_value: si ``True`` pide ``--stats`` y parsea ``R00`` como
            valor de retorno (la salida incluira el volcado de estadisticas).
        :param timeout: segundos maximos; ``None`` = sin limite.
        :returns: un :class:`RunResult`.
        """
        cmd = [self._bin, "--run", program]
        if mode == "jit":
            cmd += ["-m", "jit"]
        if schedulers and schedulers != 1:
            cmd += ["--schedulers", str(schedulers)]
        if want_value:
            cmd.append("--stats")
        if args:
            cmd += list(args)
        try:
            p = subprocess.run(cmd, capture_output=True, timeout=timeout)
        except subprocess.TimeoutExpired as exc:
            raise LspError("la ejecucion excedio el timeout (%ss)"
                           % timeout) from exc
        out = p.stdout.decode("utf-8", "replace")
        err = p.stderr.decode("utf-8", "replace")
        value = None
        if want_value:
            m = _R00_RE.search(out) or _R00_RE.search(err)
            if m:
                value = int(m.group(1), 16)
        return RunResult(p.returncode, out, err, value)

    def compile_status(self, source: str, *, mode: str = "vm",
                       output: Optional[str] = None,
                       timeout: Optional[float] = 60.0) -> RunResult:
        """Compila un ``.vx`` con el binario ``vm`` (soporta TODO: AOT, formatos).

        Complemento de :meth:`vesta_lsp_client.VestaLspClient.compile` para los
        casos que el compilador embebido del LSP aun no cubre (p.ej. AOT nativo
        a ``.exe``): aqui se delega en el binario ``vm``, que soporta todos los
        flags.  Devuelve un :class:`RunResult` con la salida del compilador.

        :param source: ruta al fichero ``.vx``.
        :param mode: ``"vm"`` | ``"jit"`` | ``"aot"``.
        :param output: prefijo de salida (``-o``).
        """
        cmd = [self._bin, "--vesta", source, "-m", mode]
        if output:
            cmd += ["-o", output]
        try:
            p = subprocess.run(cmd, capture_output=True, timeout=timeout)
        except subprocess.TimeoutExpired as exc:
            raise LspError("la compilacion excedio el timeout") from exc
        return RunResult(p.returncode,
                         p.stdout.decode("utf-8", "replace"),
                         p.stderr.decode("utf-8", "replace"), None)
