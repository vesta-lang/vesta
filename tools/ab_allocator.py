#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# VestaVM - Maquina Virtual Distribuida
# Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
# Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
#
"""A/B de los DOS asignadores del interprete con el MISMO binario.

Compila y ejecuta cada programa del corpus dos veces -- con la puerta cerrada
(ir::allocate_regs) y abierta (codegen::rbank) -- y compara el R0.

Que sea el mismo binario no es un detalle: es la unica forma de atribuir una
diferencia AL ASIGNADOR y no a cualquier otra cosa que cambie entre dos
compilaciones.  Por eso la puerta existe.

El `.vel` SI puede cambiar (dos asignaciones distintas pueden ser ambas
correctas); lo que no puede cambiar es lo que el programa CALCULA.

Uso:  python tools/ab_allocator.py <vm.exe> [-j N] [--gate VESTA_VM_RBANK]
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CORPUS = ROOT / "examples_codes_vx"
RE_R0 = re.compile(r"R00\s*=\s*(0x[0-9a-fA-F]+)")


def run_one(vm, src, out, env_extra, timeout):
    """Compila y ejecuta en interprete PURO.  Devuelve el R0 o None."""
    env = dict(os.environ)
    env.update(env_extra)
    try:
        c = subprocess.run([str(vm), "--vx", str(src), "-o", str(out)],
                           capture_output=True, timeout=timeout, env=env)
        if c.returncode != 0:
            return None  # no compila (negativo esperado)
        # `--run` auto-JITea: para medir el INTERPRETE hay que pedirlo explicito.
        r = subprocess.run([str(vm), "-m", "vm", "--run", str(out) + ".velb"],
                           capture_output=True, timeout=timeout, env=env)
    except subprocess.TimeoutExpired:
        return "TIMEOUT"
    txt = (r.stdout + r.stderr).decode("utf-8", "replace")
    m = RE_R0.search(txt)
    return m.group(1).lower() if m else ("rc=%d" % r.returncode)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("vm")
    ap.add_argument("--jobs", "-j", type=int, default=16)
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("--gate", default="VESTA_VM_RBANK")
    ap.add_argument("--filter", default="")
    args = ap.parse_args()

    vm = Path(args.vm).resolve()
    if not vm.exists():
        print("[error] no existe %s" % vm)
        return 2
    progs = sorted(p for p in CORPUS.rglob("*.vx") if args.filter in p.name)
    print("[a/b] %d programas | puerta %s | %d jobs"
          % (len(progs), args.gate, args.jobs))

    same = diff = skip = 0
    rows = []
    with tempfile.TemporaryDirectory() as td:
        def one(p):
            # Nombre unico por (programa, lado): hay muchos `main.vx` en
            # subdirectorios y con el stem a secas se pisarian entre hilos.
            base = str(abs(hash(str(p))))
            off = run_one(vm, p, Path(td) / ("off_" + base), {args.gate: "0"},
                          args.timeout)
            on = run_one(vm, p, Path(td) / ("on_" + base), {args.gate: "1"},
                         args.timeout)
            return p, off, on

        with ThreadPoolExecutor(max_workers=args.jobs) as ex:
            for i, (p, off, on) in enumerate(ex.map(one, progs)):
                if off is None or on is None:
                    skip += 1
                elif off == on:
                    same += 1
                else:
                    diff += 1
                    rows.append((str(p.relative_to(CORPUS)), off, on))
                if (i + 1) % 100 == 0:
                    print("  ... %d/%d" % (i + 1, len(progs)), flush=True)

    print("\n=== A/B del asignador del interprete ===")
    print("  MISMO resultado ..... %d" % same)
    print("  distinto ............ %d" % diff)
    print("  no compilan ......... %d  (negativos esperados)" % skip)
    if rows:
        print("\n  programa                          cerrada -> abierta")
        for name, a, b in rows[:25]:
            print("    %-32s %s -> %s" % (name, a, b))
        print("\n  VEREDICTO: el modelo NO calcula lo mismo.  Mirar esos primero.")
    else:
        print("\n  VEREDICTO: el modelo calcula EXACTAMENTE lo mismo en todo")
        print("  el corpus.  La asignacion puede diferir; el resultado no.")
    return 1 if diff else 0


if __name__ == "__main__":
    sys.exit(main())
