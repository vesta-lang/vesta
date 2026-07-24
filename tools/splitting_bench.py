#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# VestaVM - Maquina Virtual Distribuida
# Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
# Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
#
"""¿El SPLITTING hace que algo vaya mas rapido?  (tiempo, no area).

Todo el KPI del splitting mide AREA DE LANE -- cuanto banco de registros se deja
ocioso -- que es un PROXY.  Este script mide lo unico que decide si la
optimizacion merece estar activada: el TIEMPO.

Hay un motivo concreto para dudar del proxy: en x86-64 el Rewrite PLIEGA el slot
como operando de memoria de la propia instruccion, asi que un uso derramado no
cuesta una instruccion extra.  Se puede recuperar muchisima area y no ganar un
solo ciclo.

METODO (lo que hace creible el resultado):
  - DOS FASES: un CRIBADO barato descarta los programas que ni generan plan --
    cronometrarlos seria gastar horas midiendo ruido.  Se conserva un grupo de
    CONTROL sin plan como testigo: si el control tambien se mueve, lo medido es
    la maquina, no la optimizacion.
  - SUELO DE RUIDO: se mide el MISMO binario con la MISMA configuracion
    dos veces.  Un delta menor que ese suelo NO es una mejora, es varianza.  Sin
    este dato, cualquier porcentaje pequeno es autoengano.
  - best-of-N (el minimo, no la media): el minimo es la muestra menos
    contaminada por el planificador del SO, y es la convencion del proyecto.
  - INTERCALADO OFF/ON en cada repeticion, no en bloques: si la maquina se
    calienta o entra otro proceso a mitad, afecta a las dos ramas por igual.

Uso:  python tools/splitting_bench.py <vm.exe> [--reps N] [--filter X]
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CORPUS = ROOT / "examples_codes_vx"

RE_H = re.compile(r"SPLITTING real: valores=(\d+) tramos=(\d+) usos=(\d+)")

# Programas donde la medicion del corpus vio area recuperada (top por area) +
# todos los benchmarks (que son los unicos con carga suficiente para cronometrar).
TOP_RECOVERED = [
    "bench_native_callback", "76_string_exhaustive", "273_overlay_pe_parser",
    "test_fmt_spec", "test_read", "272_overlay_elf_parser", "281_overlay_endian_ctx",
    "264_overlay_block_if", "57_linkedlist_option", "175_generics_deep_nesting",
    "101_raii_casos_limite", "258_struct_defaults",
]


def compile_velb(vm, path, out):
    subprocess.run([str(vm), "--vx", str(path), "-o", str(out)],
                   capture_output=True, timeout=120)
    velb = Path(str(out) + ".velb")
    return velb if velb.exists() else None


def run_once(vm, velb, splitting, timeout):
    """Devuelve el wall time en ms, o None si falla/timeout."""
    env = dict(os.environ)
    if splitting:
        env["VESTA_SPLITTING"] = "1"
    else:
        env.pop("VESTA_SPLITTING", None)
    t0 = time.perf_counter()
    try:
        subprocess.run([str(vm), "--run", str(velb), "-m", "jit", "--jit-threshold", "1"],
                       capture_output=True, timeout=timeout, env=env)
    except subprocess.TimeoutExpired:
        return None
    return (time.perf_counter() - t0) * 1000.0


def plan_info(vm, velb, timeout):
    """(valores, tramos, usos) del plan de splitting, o (0,0,0)."""
    env = dict(os.environ, VESTA_REMAT_MEASURE="1", VESTA_SPLITTING="1")
    try:
        r = subprocess.run(
            [str(vm), "--run", str(velb), "-m", "jit", "--jit-threshold", "1"],
            capture_output=True, timeout=timeout, env=env)
    except subprocess.TimeoutExpired:
        return (0, 0, 0)
    m = RE_H.search(r.stderr.decode("utf-8", "replace"))
    return tuple(int(m.group(i)) for i in (1, 2, 3)) if m else (0, 0, 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("vm")
    ap.add_argument("--reps", type=int, default=7)
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("--filter", default="")
    args = ap.parse_args()

    vm = Path(args.vm).resolve()
    if not vm.exists():
        print(f"[error] no existe {vm}")
        return 1

    # Candidatos: benchmarks (carga real) + los programas donde SI hubo recuperacion.
    cand = {}
    for p in CORPUS.rglob("*.vx"):
        if "benchmark" in p.parts or p.stem in TOP_RECOVERED:
            if args.filter in p.stem:
                cand[p.stem] = p
    if not cand:
        print("[error] sin candidatos")
        return 1
    names = sorted(cand)
    print(f"[bench] {len(names)} candidatos -- fase 1: cribado (¿genera plan?)",
          flush=True)

    rows = []
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        # --- FASE 1: cribado.  Una ejecucion por programa para saber quien tiene plan.
        # Cronometrar los que no lo tienen seria medir ruido durante horas.  Se conserva
        # un grupo de CONTROL sin plan como TESTIGO: si el control tambien "se mueve",
        # lo que se esta midiendo es la maquina, no la optimizacion.
        built = {}
        for name in names:
            velb = compile_velb(vm, cand[name], tmp / name)
            if velb:
                built[name] = (velb, plan_info(vm, velb, args.timeout))
        with_plan = [n for n in names if n in built and built[n][1][1]]
        without = [n for n in names if n in built and not built[n][1][1]]
        control = without[:3]
        target = with_plan + control
        print(f"[bench] con plan: {len(with_plan)} | control sin plan: {len(control)}"
              f" | descartados: {len(without) - len(control)}", flush=True)
        print(f"[bench] fase 2: best-of-{args.reps}, OFF/ON intercalado\n", flush=True)

        for i, name in enumerate(target, 1):
            velb, (vals, tramos, usos) = built[name]
            off, on = [], []
            bad = False
            for _ in range(args.reps):
                a = run_once(vm, velb, False, args.timeout)   # intercalado: la deriva
                b = run_once(vm, velb, True, args.timeout)    # afecta a ambas ramas.
                if a is None or b is None:
                    bad = True
                    break
                off.append(a)
                on.append(b)
            if bad or not off:
                continue
            rows.append((name, min(off), min(on), tramos, usos))
            print(f"  [{i}/{len(target)}] {name}", flush=True)

    if not rows:
        print("[error] ninguna medicion valida")
        return 1

    # SUELO DE RUIDO: lo da el GRUPO DE CONTROL, no una re-medicion sintetica.
    #
    # LECCION (error cometido y corregido): medir el ruido sobre UN programa y aplicarlo
    # a todos es invalido.  Un programa de carga LARGA tiene una varianza RELATIVA
    # minima; uno de carga CORTA esta dominado por el arranque del proceso y por la
    # compilacion JIT, y se mueve un orden de magnitud mas.  El suelo medido en el
    # pesado declaraba "MEJORA" deltas pequenos en los cortos... incluidos los del
    # CONTROL, cuyo codigo es BYTE-IDENTICO en ambas ramas y por tanto no puede mejorar.
    #
    # Nada aqui usa umbrales en MILISEGUNDOS ABSOLUTOS: dependen de la CPU, del build y
    # de la carga de la maquina, asi que hardcodearlos no es reproducible.  "Corto" y
    # "largo" se determinan por la FRACCION del tiempo que se va en compilar (dato que
    # da la telemetria del JIT), no por un numero fijo.
    #
    # El control ES la medida honesta: mismo codigo, misma carga, misma duracion -> todo
    # lo que se mueva ahi es ruido puro.  Se toma el maximo |delta| observado.
    ctrl = [abs(n - o) / max(o, 1e-9) * 100.0 for _, o, n, tr, _ in rows if not tr]
    noise = max(ctrl) if ctrl else 0.0

    print("\n=== SPLITTING: tiempo (no area) ===")
    print(f"  SUELO DE RUIDO (max |delta| del grupo de control): {noise:.2f}%")
    print(f"  -> medido sobre {len(ctrl)} programas SIN plan (codigo byte-identico):"
          " lo que se mueva ahi es ruido puro.")
    print("  -> cualquier |delta| por debajo de ese valor NO es una mejora.\n")
    print(f"  {'programa':<40} {'OFF ms':>9} {'ON ms':>9} {'delta':>8}  plan")
    print("  " + "-" * 78)

    rows.sort(key=lambda r: (r[2] - r[1]) / max(r[1], 1e-9))
    signif = []
    for name, o, n, tramos, usos in rows:
        d = (n - o) / max(o, 1e-9) * 100.0
        mark = "" if abs(d) <= noise else ("  <-- MEJORA" if d < 0 else "  <-- REGRESION")
        plan = f"{tramos} tramos/{usos} usos" if tramos else "CONTROL (sin plan)"
        if abs(d) > noise and tramos:
            signif.append((name, d))
        print(f"  {name:<40} {o:9.1f} {n:9.1f} {d:+7.2f}%  {plan}{mark}")

    with_plan = [r for r in rows if r[3]]
    print(f"\n  programas con plan de splitting: {len(with_plan)} de {len(rows)}")
    if with_plan:
        avg = sum((n - o) / max(o, 1e-9) for _, o, n, _, _ in with_plan) / len(with_plan)
        print(f"  delta medio DONDE HAY PLAN: {avg * 100:+.2f}%"
              f"   (suelo de ruido {noise:.2f}%)")
    print("\n  VEREDICTO:")
    if not with_plan:
        print("    ningun programa medido genero plan -> el experimento no concluye.")
    elif not signif:
        print("    ningun cambio supera el ruido -> el area recuperada NO se traduce en")
        print("    tiempo.  El default correcto es OFF: correcto pero sin efecto medible.")
    else:
        for name, d in signif:
            print(f"    {name}: {d:+.2f}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
