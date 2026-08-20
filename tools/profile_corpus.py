#!/usr/bin/env python3
"""Compila el corpus entero bajo UNA sola recogida de VTune.

Por que una sola recogida y no una por ejemplo: finalizar un resultado de VTune
cuesta bastante mas que compilar un ejemplo de doscientas lineas, y son casi
quinientos.  VTune sigue a los procesos hijo, asi que perfilando este guion se
perfila todo el barrido de una vez.

Y por que se anota el PID: al pedir el informe agrupado por proceso, VTune da
una fila por PID.  Sin saber que PID compilo que fuente, esa fila no dice nada.
El mapa se escribe aqui, mientras se lanza cada compilacion.

Se compila EN SERIE a proposito.  En paralelo los ejemplos se pisan la CPU y el
perfil deja de decir cuanto cuesta cada cosa para decir cuanto se espero por un
nucleo libre.

Uso:
    python tools/profile_corpus.py cmake-build-profile/vm.exe --map salida.csv
"""
from __future__ import annotations

import argparse
import csv
import os
import subprocess
import sys
import time
from pathlib import Path


def compile_one(vm: Path, source: Path, out_dir: Path, timeout: float,
                env: dict) -> tuple:
    """Compila @p source y devuelve (pid, codigo, milisegundos).

    Se usa Popen y no run() porque hace falta el PID para poder atribuir
    despues el coste que VTune mide por proceso.  El codigo -1 marca que se
    paso del tiempo: un ejemplo colgado no debe parar el barrido entero.
    """
    out = out_dir / source.stem
    started = time.perf_counter()
    proc = subprocess.Popen(
        [str(vm), "--vesta", str(source), "-o", str(out)],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
    pid = proc.pid
    try:
        code = proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        code = -1
    return (pid, code, (time.perf_counter() - started) * 1000.0)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("vm", type=Path, help="binario del compilador a perfilar")
    ap.add_argument("--sources", type=Path, default=Path("examples_codes_vx"),
                    help="carpeta con los .vx del corpus")
    ap.add_argument("--out-dir", type=Path, default=Path("F:/vxtmp/prof"),
                    help="donde dejar los artefactos (se ignoran)")
    ap.add_argument("--map", type=Path, default=Path("F:/vxtmp/pid_map.csv"),
                    help="CSV con pid,ejemplo,codigo,ms")
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--limit", type=int, default=0,
                    help="solo los N primeros (para probar el montaje)")
    ap.add_argument("--parallel", action="store_true",
                    help="compilar con el reparto por hilos activado")
    args = ap.parse_args()

    sources = sorted(args.sources.glob("*.vx"))
    if args.limit:
        sources = sources[:args.limit]
    if not sources:
        print(f"sin fuentes en {args.sources}", file=sys.stderr)
        return 2
    args.out_dir.mkdir(parents=True, exist_ok=True)

    # El reparto por hilos es opt-in; el perfil base se toma SIN el, que es lo
    # que dice donde valdria la pena repartir.
    env = dict(os.environ)
    env["VESTA_PARALELO"] = "1" if args.parallel else "0"

    rows = []
    failed = 0
    started = time.perf_counter()
    for i, source in enumerate(sources, 1):
        pid, code, ms = compile_one(args.vm, source, args.out_dir,
                                    args.timeout, env)
        rows.append({"pid": pid, "example": source.name, "code": code,
                     "ms": round(ms, 1)})
        # Los negativos del corpus terminan con codigo 1 A PROPOSITO; solo se
        # cuentan aparte los que se pasan de tiempo o se caen.
        if code not in (0, 1):
            failed += 1
            print(f"  codigo={code}  {source.name}", file=sys.stderr)
        if i % 100 == 0:
            print(f"  {i}/{len(sources)}", file=sys.stderr)

    with args.map.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=["pid", "example", "code", "ms"])
        writer.writeheader()
        writer.writerows(rows)

    total = time.perf_counter() - started
    print(f"compilados={len(rows)}  anomalos={failed}  "
          f"total={total:.1f}s  mapa={args.map}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
