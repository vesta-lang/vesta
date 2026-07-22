#!/usr/bin/env python3
# VestaVM - medicion agregada de la Recovery Pass + taxonomia sobre el corpus.
#
# Compila cada .vx del corpus a .velb y lo ejecuta DOS veces con el instrumento
# del allocator (VESTA_REMAT_MEASURE=1): una con VESTA_RECOVERY=0 (greedy sin
# recuperacion) y otra con =1 (Recovery ON).  Parsea:
#   (e) GREEDY:   spills + AREA_ociosa (wasted_lane_area).
#   (f) TAXONOMIA: structural/fully(grafo)/partially + RECUPERACION greedy/optimo.
# Agrega:
#   - spills OFF vs ON            -> recuperados por la Recovery real (nº y %).
#   - wasted_lane_area OFF vs ON  -> reduccion de area (nº y %).
#   - taxonomia (ON): fully/partially/structural totales del corpus.
#   - RECUPERACION de fully: greedy vs optimo (matching bipartito) -> GAP agregado.
#
# El GAP greedy->optimo decide el SIGUIENTE sprint:
#   gap ~ 0  -> el greedy ya es optimo; ir a Fragmentation Recovery (splitting).
#   gap grande -> merece un matching real (Hopcroft-Karp) antes que el splitting.

import argparse
import os
import re
import subprocess
import tempfile
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

# (e) "... spills=15 ... AREA_ociosa=715 lane-pos ...".
RE_E = re.compile(r"spills=(\d+).*?AREA_ociosa=(\d+)", re.S)
# (f) "... structural(inevitable)=0  fully(grafo)=6  partially(Splitting)=9
#       | RECUPERACION fully: greedy=1 optimo=1 gap=0".
RE_F = re.compile(
    r"structural\(inevitable\)=(\d+)\s+fully\(grafo\)=(\d+)\s+"
    r"partially\(Splitting\)=(\d+).*?Recovered=(\d+)", re.S)
# (g) "... splitting_potential=629 de wasted_lane_area=715 ...".
RE_G = re.compile(r"splitting_potential=(\d+)\s+de\s+wasted_lane_area=(\d+)", re.S)


def discover(root, benchmarks):
    corpus = []
    for p in sorted((root / "examples_codes_vx").glob("*.vx")):
        corpus.append((p.stem, p))
    if benchmarks:
        bd = root / "examples_codes_vx" / "benchmark"
        if bd.exists():
            for d in sorted(bd.iterdir()):
                mv = d / "main.vx"
                if mv.exists():
                    corpus.append((d.name, mv))
    return corpus


def compile_velb(vm, path, out, timeout):
    try:
        subprocess.run([str(vm), "--vx", str(path), "-o", str(out)],
                       capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None
    velb = Path(str(out) + ".velb")
    return velb if velb.exists() else None


def run_measure(vm, velb, recovery, timeout):
    # Devuelve dict con spills/area/(taxonomia) o None si timeout.
    env = dict(os.environ, VESTA_REMAT_MEASURE="1",
               VESTA_RECOVERY=("1" if recovery else "0"))
    try:
        r = subprocess.run(
            [str(vm), "--run", str(velb), "-m", "jit", "--jit-threshold", "1"],
            capture_output=True, timeout=timeout, env=env)
    except subprocess.TimeoutExpired:
        return None
    txt = r.stderr.decode("utf-8", "replace")
    d = {"spills": 0, "area": 0, "structural": 0, "fully": 0,
         "partially": 0, "greedy": 0, "split_pot": 0}
    me = RE_E.search(txt)
    if me:
        d["spills"], d["area"] = int(me.group(1)), int(me.group(2))
    mf = RE_F.search(txt)
    if mf:
        (d["structural"], d["fully"], d["partially"],
         d["greedy"]) = (int(mf.group(i)) for i in range(1, 5))
    mg = RE_G.search(txt)
    if mg:
        d["split_pot"] = int(mg.group(1))
    return d


def process_one(vm, name, path, tmp, ct, rt):
    velb = compile_velb(vm, path, tmp / name, ct)
    if not velb:
        return None  # no compila (negativo esperado) o timeout de compilacion.
    off = run_measure(vm, velb, False, rt)
    on = run_measure(vm, velb, True, rt)
    if off is None or on is None:
        return None  # timeout de ejecucion.
    return (name, off, on)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("vm")
    ap.add_argument("--run-timeout", type=float, default=15.0)
    ap.add_argument("--compile-timeout", type=float, default=60.0)
    ap.add_argument("--jobs", "-j", type=int, default=4)
    ap.add_argument("--no-benchmarks", action="store_true")
    ap.add_argument("--filter", default="")
    args = ap.parse_args()

    root = Path(__file__).resolve().parent.parent
    vm = Path(args.vm).resolve()
    corpus = discover(root, not args.no_benchmarks)
    if args.filter:
        corpus = [(n, p) for (n, p) in corpus if args.filter in n]
    tmp = Path(tempfile.mkdtemp(prefix="corpus_rec_"))
    print(f"[info] corpus: {len(corpus)} programas, jobs {args.jobs}, "
          f"run-timeout {args.run_timeout}s")

    results = []
    done = 0
    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(process_one, vm, n, p, tmp, args.compile_timeout,
                          args.run_timeout): n for (n, p) in corpus}
        for fut in as_completed(futs):
            done += 1
            r = fut.result()
            if r:
                results.append(r)
            if done % 25 == 0:
                print(f"  [{done}/{len(corpus)}]", flush=True)

    # Solo programas con spills en algun modo (los demas no ejercitan el allocator).
    ws = [(n, off, on) for (n, off, on) in results if off["spills"] > 0 or on["spills"] > 0]
    sum_off_s = sum(off["spills"] for _, off, _ in ws)
    sum_on_s = sum(on["spills"] for _, _, on in ws)
    sum_off_a = sum(off["area"] for _, off, _ in ws)
    sum_on_a = sum(on["area"] for _, _, on in ws)
    acted = [(n, off, on) for (n, off, on) in ws
             if on["spills"] < off["spills"] or on["area"] < off["area"]]
    rec = [off["spills"] - on["spills"] for (_, off, on) in ws if on["spills"] < off["spills"]]

    # Taxonomia + recuperacion: del modo ON (donde greedy es real; la taxonomia es
    # PRE-recovery, identica en ambos, pero rec_greedy solo tiene sentido con ON).
    t_full = sum(on["fully"] for _, _, on in ws)
    t_part = sum(on["partially"] for _, _, on in ws)
    t_struct = sum(on["structural"] for _, _, on in ws)
    t_greedy = sum(on["greedy"] for _, _, on in ws)

    print("\n=== CORPUS: Recovery Pass OFF vs ON ===")
    print(f"  compilados: {len(results)} | con spills (ejercitan allocator): {len(ws)}")
    if sum_off_s:
        print(f"  SPILLS totales:  OFF={sum_off_s}  ON={sum_on_s}  "
              f"recuperados={sum_off_s - sum_on_s} "
              f"({100.0 * (sum_off_s - sum_on_s) / sum_off_s:.1f}%)")
    if sum_off_a:
        print(f"  AREA ociosa tot: OFF={sum_off_a}  ON={sum_on_a}  "
              f"reduccion={sum_off_a - sum_on_a} "
              f"({100.0 * (sum_off_a - sum_on_a) / sum_off_a:.1f}%)")
    if ws:
        print(f"  programas donde Recovery ACTUO: {len(acted)}/{len(ws)} "
              f"({100.0 * len(acted) / len(ws):.1f}%)")
    if rec:
        print(f"  distribucion d-spills/programa: {dict(sorted(Counter(rec).items()))}")

    print("\n=== TAXONOMIA (per-spill, PRE-recovery) ===")
    tot_tax = t_full + t_part + t_struct
    if tot_tax:
        print(f"  fully(grafo)={t_full} ({100.0*t_full/tot_tax:.1f}%)  "
              f"partially(Splitting)={t_part} ({100.0*t_part/tot_tax:.1f}%)  "
              f"structural(inevitable)={t_struct} ({100.0*t_struct/tot_tax:.1f}%)")
    # KPI del allocator: Fully(limite superior) / Recovered(greedy) / Potential.  El
    # "optimo real" de recuperar fully es INTERVAL SCHEDULING (varios no-solapantes por
    # lane), NO matching bipartito (que subestima: medido, da < greedy).  Potential es
    # el limite superior del margen (no todo recuperable a la vez).
    print("\n=== RECUPERACION de fully: KPI del allocator ===")
    pot = t_full - t_greedy
    print(f"  Fully(grafo)={t_full}  Recovered(greedy)={t_greedy}  Potential={pot}")
    if t_full:
        print(f"  greedy recupera {100.0*t_greedy/t_full:.1f}% del fully "
              f"(Potential = fully sin recuperar; cota superior, comparten lanes)")
    print("  -> El grueso NO esta en fully: partially domina.  Siguiente sprint =")
    print("     Fragmentation Recovery (splitting) -> minimizar wasted_lane_area.")

    # TECHO del splitting: cuanto del wasted_lane_area es recuperable metiendo los
    # partially en registro en sus huecos (splitting perfecto).  Decide si implementarlo.
    t_split = sum(on["split_pot"] for _, _, on in ws)
    print("\n=== TECHO del SPLITTING (Fragmentation Recovery) ===")
    print(f"  splitting_potential={t_split}  de wasted_lane_area(ON)={sum_on_a}")
    if sum_on_a:
        print(f"  -> {100.0*t_split/sum_on_a:.1f}% del area desperdiciada es recuperable "
              f"por una Fragmentation Recovery ideal.")
        print(f"     (patron futuro: potential -> recovered_area -> remaining_area)")


if __name__ == "__main__":
    main()
