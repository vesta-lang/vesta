#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# VestaVM - Maquina Virtual Distribuida
# Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
# Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
#
"""KPI del SPLITTING (Fragmentation Recovery) sobre todo el corpus.

Aplica la metodologia del backend -- Potential -> Recovered -> Remaining -- al area
de lane que el allocator desperdicia:

    Potential   techo MEDIDO antes de implementar (area libre de los spills `partially`).
    Recovered   lo que la transformacion consigue de verdad.
    Remaining   margen que queda para una transformacion mas potente.

Compila cada .vx del corpus y lo ejecuta en JIT con el instrumento activo
(VESTA_REMAT_MEASURE=1) y el splitting encendido (VESTA_SPLITTING=1), agregando las
lineas (g) y (h) del resumen del allocator.

Uso:  python tools/corpus_splitting.py <vm.exe> [-j N] [--filter X]
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

# (g) SPLITTING techo: splitting_potential=N de wasted_lane_area=M (P% ...)
RE_G = re.compile(r"splitting_potential=(\d+) de wasted_lane_area=(\d+)")
# (h) SPLITTING real: valores=A tramos=B usos=C | AREA Potential=D Recovered=E (F%)
#     Remaining=G | descartados forma=H coste=I
RE_H = re.compile(
    r"SPLITTING real: valores=(\d+) tramos=(\d+) usos=(\d+) \| AREA Potential=(\d+)"
    r" Recovered=(\d+) \([\d.]+%\) Remaining=(\d+) \| descartados forma=(\d+)"
    r" coste=(\d+)")
# (i) SPLITTING perfil: ACEPTADO len=A usos=B ganancia=C | RECHAZADO len=D usos=E ganancia=F
RE_I = re.compile(
    r"SPLITTING perfil: ACEPTADO len=([\d.-]+) usos=([\d.-]+) ganancia=([\d.-]+)"
    r" \| RECHAZADO len=([\d.-]+) usos=([\d.-]+) ganancia=([\d.-]+)")

KEYS = ("values", "intervals", "uses", "potential", "recovered", "remaining",
        "rej_shape", "rej_cost", "wasted")
# Medias por programa; se re-ponderan por numero de tramos al agregar.
FKEYS = ("acc_len", "acc_uses", "acc_gain", "rej_len", "rej_uses", "rej_gain")


def compile_velb(vm, path, out, timeout):
    try:
        subprocess.run([str(vm), "--vx", str(path), "-o", str(out)],
                       capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None
    velb = Path(str(out) + ".velb")
    return velb if velb.exists() else None


def run_measure(vm, velb, timeout):
    """Ejecuta con el instrumento + splitting y devuelve los contadores de (g)/(h)."""
    env = dict(os.environ, VESTA_REMAT_MEASURE="1", VESTA_SPLITTING="1")
    try:
        r = subprocess.run(
            [str(vm), "--run", str(velb), "-m", "jit", "--jit-threshold", "1"],
            capture_output=True, timeout=timeout, env=env)
    except subprocess.TimeoutExpired:
        return None
    txt = r.stderr.decode("utf-8", "replace")
    d = dict.fromkeys(KEYS, 0)
    d.update(dict.fromkeys(FKEYS, 0.0))
    mg = RE_G.search(txt)
    if mg:
        d["wasted"] = int(mg.group(2))
    mh = RE_H.search(txt)
    if mh:
        (d["values"], d["intervals"], d["uses"], d["potential"], d["recovered"],
         d["remaining"], d["rej_shape"], d["rej_cost"]) = (
            int(mh.group(i)) for i in range(1, 9))
    mi = RE_I.search(txt)
    if mi:
        for j, k in enumerate(FKEYS):
            d[k] = float(mi.group(j + 1))
    return d


def process_one(vm, name, path, tmp, ct, rt):
    velb = compile_velb(vm, path, tmp / name, ct)
    if not velb:
        return None  # no compila (negativo esperado) o timeout.
    d = run_measure(vm, velb, rt)
    return None if d is None else (name, d)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("vm")
    ap.add_argument("--run-timeout", type=float, default=15.0)
    ap.add_argument("--compile-timeout", type=float, default=60.0)
    ap.add_argument("--jobs", "-j", type=int, default=4)
    ap.add_argument("--filter", default="")
    ap.add_argument("--top", type=int, default=12,
                    help="cuantos programas listar por area recuperada")
    args = ap.parse_args()

    vm = Path(args.vm).resolve()
    if not vm.exists():
        print(f"[error] no existe {vm}")
        return 1

    progs = sorted(p for p in CORPUS.rglob("*.vx") if args.filter in p.name)
    if not progs:
        print("[error] corpus vacio")
        return 1
    print(f"[splitting] {len(progs)} programas, {args.jobs} jobs")

    rows = []
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        with ThreadPoolExecutor(max_workers=args.jobs) as ex:
            futs = [ex.submit(process_one, vm, p.stem, p, tmp,
                              args.compile_timeout, args.run_timeout)
                    for p in progs]
            for i, f in enumerate(futs):
                r = f.result()
                if r:
                    rows.append(r)
                if (i + 1) % 40 == 0:
                    print(f"  ... {i + 1}/{len(progs)}", flush=True)

    agg = dict.fromkeys(KEYS, 0)
    wacc = dict.fromkeys(("len", "uses", "gain"), 0.0)  # ponderado por tramos.
    wrej = dict.fromkeys(("len", "uses", "gain"), 0.0)  # ponderado por rechazos.
    with_split = 0
    for _, d in rows:
        for k in KEYS:
            agg[k] += d[k]
        if d["intervals"]:
            with_split += 1
        for k, src in (("len", "acc_len"), ("uses", "acc_uses"), ("gain", "acc_gain")):
            wacc[k] += d[src] * d["intervals"]
        for k, src in (("len", "rej_len"), ("uses", "rej_uses"), ("gain", "rej_gain")):
            wrej[k] += d[src] * d["rej_cost"]

    print("\n=== SPLITTING (Fragmentation Recovery) sobre el corpus ===")
    print(f"  programas medidos ........ {len(rows)}")
    print(f"  con algun tramo recuperado {with_split}"
          f" ({100.0 * with_split / max(1, len(rows)):.1f}%)")
    print(f"  valores partidos ......... {agg['values']}")
    print(f"  tramos del plan .......... {agg['intervals']}")
    print(f"  usos devueltos a registro  {agg['uses']}")
    print("\n  --- Potential -> Recovered -> Remaining (area de lane) ---")
    pot, rec = agg["potential"], agg["recovered"]
    print(f"  Potential (techo medido) . {pot}")
    print(f"  Recovered (real) ......... {rec}"
          f"  ({100.0 * rec / max(1, pot):.1f}% del techo)")
    print(f"  Remaining (margen) ....... {agg['remaining']}")
    if agg["wasted"]:
        print(f"  [contexto] wasted_lane_area del corpus = {agg['wasted']}"
              f" -> el techo era el {100.0 * pot / agg['wasted']:.1f}% y se recupera"
              f" el {100.0 * rec / agg['wasted']:.1f}%")
    print("\n  --- Por que NO se recupera el resto ---")
    print(f"  descartados por FORMA .... {agg['rej_shape']}"
          "   (no cabe: cruza bloque / borde de la vida)")
    print(f"  descartados por COSTE .... {agg['rej_cost']}"
          "   (cabe, pero no compensa segun el cost model)")

    ni, nr = max(1, agg["intervals"]), max(1, agg["rej_cost"])
    print("\n  --- Perfil de la decision (guia el tuning del cost model) ---")
    print(f"  ACEPTADO  len={wacc['len'] / ni:7.1f}  usos={wacc['uses'] / ni:6.2f}"
          f"  ganancia={wacc['gain'] / ni:6.2f}")
    print(f"  RECHAZADO len={wrej['len'] / nr:7.1f}  usos={wrej['uses'] / nr:6.2f}"
          f"  ganancia={wrej['gain'] / nr:6.2f}")

    # PARETO: ¿esta el resultado dominado por unos pocos programas?  Sin esto, ajustar
    # parametros contra el agregado seria tuning sobre el sesgo de un solo benchmark.
    rows.sort(key=lambda r: -r[1]["recovered"])
    tot = sum(d["recovered"] for _, d in rows)
    if tot:
        print("\n  --- Pareto (concentracion del resultado) ---")
        acc = 0
        marks = [50.0, 80.0, 90.0]
        for i, (_, d) in enumerate(rows, 1):
            acc += d["recovered"]
            while marks and 100.0 * acc / tot >= marks[0]:
                print(f"  el {marks[0]:.0f}% del area recuperada viene de {i} programa(s)"
                      f" ({100.0 * i / len(rows):.1f}% del corpus medido)")
                marks.pop(0)
        top1 = 100.0 * rows[0][1]["recovered"] / tot
        if top1 >= 50.0:
            print(f"  [aviso] un solo programa ({rows[0][0]}) aporta el {top1:.1f}%:"
                  " el agregado NO es representativo -- calibrar con la mediana")
        med = rows[len(rows) // 2][1]["recovered"]
        print(f"  mediana de area recuperada por programa: {med}")

    print(f"\n  --- Top {args.top} por area recuperada ---")
    for name, d in rows[:args.top]:
        if not d["recovered"]:
            break
        print(f"    {name:<44} area={d['recovered']:<7} tramos={d['intervals']:<4}"
              f" usos={d['uses']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
