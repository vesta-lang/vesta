#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# VestaVM - Maquina Virtual Distribuida
# Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
# Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
#
"""MODO SOMBRA del camino del interprete, sobre TODO el corpus.

Responde la pregunta que hay que responder ANTES de cambiar el asignador del
interprete: ¿el modelo (codegen::rbank) ve el mismo problema que
ir::allocate_regs y decide igual o mejor?

Se compara CALIDAD (cuantos valores, cuantos derrames), no la asignacion
registro a registro: dos asignaciones distintas pueden ser ambas correctas, y
exigir igualdad literal convertiria una mejora en un "fallo".  Lo que importa es
que el modelo no derrame MAS.

Solo compila (no ejecuta): la sombra vive en el emisor, asi que basta con
generar el .velb de cada programa.

Uso:  python tools/vm_shadow_corpus.py <vm.exe> [-j N] [--filter X]
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

RE_FN = re.compile(r"funciones \.+ (\d+)")
RE_VALS = re.compile(r"valores vivos \.+ (\d+)")
RE_SE = re.compile(r"derrames emisor \.+ (\d+)")
RE_SM = re.compile(r"derrames modelo \.+ (\d+)")
RE_CMP = re.compile(r"modelo MEJOR=(\d+)\s+igual=(\d+)\s+PEOR=(\d+)")
RE_WORST = re.compile(r"peor caso: (\S+) \(\+(\d+) derrames\)")


def compile_one(vm, path, out, timeout):
    """Compila con la sombra abierta y devuelve los contadores del resumen."""
    env = dict(os.environ, VESTA_VM_SHADOW="1")
    try:
        r = subprocess.run([str(vm), "--vx", str(path), "-o", str(out)],
                           capture_output=True, timeout=timeout, env=env)
    except subprocess.TimeoutExpired:
        return None
    txt = r.stderr.decode("utf-8", "replace")
    m = RE_FN.search(txt)
    if not m:
        return None  # no compila (negativo esperado) o sin funciones
    d = {"fns": int(m.group(1)), "vals": 0, "se": 0, "sm": 0,
         "better": 0, "equal": 0, "worse": 0, "worst": None, "worst_d": 0}
    for key, rx in (("vals", RE_VALS), ("se", RE_SE), ("sm", RE_SM)):
        mm = rx.search(txt)
        if mm:
            d[key] = int(mm.group(1))
    mc = RE_CMP.search(txt)
    if mc:
        d["better"], d["equal"], d["worse"] = (int(mc.group(i)) for i in (1, 2, 3))
    mw = RE_WORST.search(txt)
    if mw:
        d["worst"], d["worst_d"] = mw.group(1), int(mw.group(2))
    return d


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("vm")
    ap.add_argument("--timeout", type=float, default=90.0)
    ap.add_argument("--jobs", "-j", type=int, default=8)
    ap.add_argument("--filter", default="")
    args = ap.parse_args()

    vm = Path(args.vm).resolve()
    if not vm.exists():
        print(f"[error] no existe {vm}")
        return 1
    progs = sorted(p for p in CORPUS.rglob("*.vx") if args.filter in p.name)
    print(f"[vm-shadow] {len(progs)} programas, {args.jobs} jobs")

    rows = []
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        with ThreadPoolExecutor(max_workers=args.jobs) as ex:
            futs = {ex.submit(compile_one, vm, p, tmp / p.stem, args.timeout): p
                    for p in progs}
            for i, (f, p) in enumerate(futs.items()):
                d = f.result()
                if d:
                    rows.append((p.stem, d))
                if (i + 1) % 60 == 0:
                    print(f"  ... {i + 1}/{len(progs)}", flush=True)

    if not rows:
        print("[error] ninguna medicion valida")
        return 1

    agg = {k: 0 for k in ("fns", "vals", "se", "sm", "better", "equal", "worse")}
    worst = []
    for name, d in rows:
        for k in agg:
            agg[k] += d[k]
        if d["worse"]:
            worst.append((d["worst_d"], d["worst"] or "?", name))

    print("\n=== SOMBRA: emisor (ir::allocate_regs) vs modelo (rbank) ===")
    print(f"  programas compilados ... {len(rows)}")
    print(f"  funciones .............. {agg['fns']}")
    print(f"  valores vivos .......... {agg['vals']}")
    print(f"  derrames emisor ........ {agg['se']}")
    print(f"  derrames modelo ........ {agg['sm']}")
    delta = agg["sm"] - agg["se"]
    signo = "MENOS" if delta < 0 else ("MAS" if delta > 0 else "los MISMOS")
    print(f"  -> el modelo derrama {signo} ({delta:+d})")
    print(f"\n  por funcion: MEJOR={agg['better']}  igual={agg['equal']}"
          f"  PEOR={agg['worse']}")

    print("\n  VEREDICTO:")
    if agg["worse"] == 0:
        print("    el modelo NO derrama de mas en NINGUNA funcion del corpus.")
        print("    Condicion necesaria cumplida para cambiar el consumidor.")
    else:
        print(f"    {agg['worse']} funciones donde el modelo derrama MAS."
              "  NO cambiar el consumidor todavia.")
        worst.sort(reverse=True)
        print("    peores casos (delta, funcion, programa):")
        for d, fn, prog in worst[:10]:
            print(f"      +{d:<4} {fn:<34} {prog}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
