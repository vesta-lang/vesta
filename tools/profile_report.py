#!/usr/bin/env python3
"""Cruza el informe de VTune con el mapa de PID y saca el coste por ejemplo.

VTune agrupa por proceso, y como cada compilacion es un proceso distinto, esa
fila ES el coste de un ejemplo -- pero identificada solo por PID.  El mapa que
escribe `profile_corpus.py` dice que PID compilo que fuente; aqui se juntan.

Ademas se pide el desglose por modulo y por funcion.  El de modulo importa mas
de lo que parece: separa lo que cuesta ARRANCAR el proceso (CRT, cargador del
sistema) de lo que cuesta COMPILAR, y en fuentes pequenos lo primero se come a
lo segundo.

Uso:
    python tools/profile_report.py F:/vxtmp/vt_corpus --map F:/vxtmp/corpus_map.csv
"""
from __future__ import annotations

import argparse
import csv
import io
import subprocess
import sys
from pathlib import Path

VTUNE = r"C:/Program Files (x86)/Intel/oneAPI/vtune/2025.3/bin64/vtune"


def vtune_report(result: Path, group_by: str, extra: list = None) -> list:
    """Devuelve el informe de VTune como lista de diccionarios.

    Sale en TSV, no en CSV con comas, pese a que la opcion se llame `csv`.
    """
    cmd = [VTUNE, "-report", "hotspots", "-r", str(result),
           "-group-by", group_by, "-format", "csv", "-csv-delimiter", "tab"]
    if extra:
        cmd += extra
    out = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    if not out.stdout.strip():
        print(out.stderr[:400], file=sys.stderr)
        return []
    return list(csv.DictReader(io.StringIO(out.stdout), delimiter="\t"))


def cpu_of(row: dict) -> float:
    """CPU de una fila, tolerando el nombre exacto de la columna."""
    for key in row:
        if key and key.strip() == "CPU Time":
            try:
                return float(row[key])
            except (TypeError, ValueError):
                return 0.0
    return 0.0


def pid_of(row: dict) -> str:
    """PID de una fila agrupada por proceso.

    El nombre de la columna cambia entre versiones, asi que se busca por
    contenido y se cae a la ultima columna, que es donde va.
    """
    for key in row:
        if key and ("PID" in key or "Process ID" in key):
            return str(row[key]).strip()
    values = [v for v in row.values() if v is not None]
    return str(values[-1]).strip() if values else ""


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("result", type=Path, help="carpeta -r de la recogida")
    ap.add_argument("--map", type=Path, help="CSV pid,example,code,ms")
    ap.add_argument("--top", type=int, default=20)
    args = ap.parse_args()

    # --- Por ejemplo (cruzando PID) -----------------------------------------
    if args.map and args.map.exists():
        by_pid = {}
        with args.map.open(encoding="utf-8") as fh:
            for row in csv.DictReader(fh):
                by_pid[str(row["pid"]).strip()] = row
        rows = vtune_report(args.result, "process")
        examples = []
        unmatched = 0.0
        for row in rows:
            cpu = cpu_of(row)
            entry = by_pid.get(pid_of(row))
            if entry is None:
                unmatched += cpu
                continue
            examples.append((cpu, entry["example"], entry["ms"]))
        examples.sort(reverse=True)
        total = sum(c for c, _, _ in examples)
        print(f"=== CPU por ejemplo  (n={len(examples)}, total={total:.2f}s, "
              f"sin cruzar={unmatched:.2f}s) ===")
        print(f"{'CPU s':>8}  {'wall ms':>8}  ejemplo")
        for cpu, name, ms in examples[:args.top]:
            print(f"{cpu:8.3f}  {ms:>8}  {name}")
        if examples:
            mid = examples[len(examples) // 2]
            print(f"  mediana: {mid[0]:.3f}s ({mid[1]})")

    # --- Por modulo: arranque frente a compilar -----------------------------
    print(f"\n=== CPU por modulo (top {args.top}) ===")
    for row in vtune_report(args.result, "module")[:args.top]:
        name = list(row.values())[0]
        print(f"{cpu_of(row):8.3f}  {name}")

    # --- Por funcion: donde esta el cuello ----------------------------------
    print(f"\n=== CPU por funcion (top {args.top}) ===")
    for row in vtune_report(args.result, "function")[:args.top]:
        name = str(list(row.values())[0])[:88]
        print(f"{cpu_of(row):8.3f}  {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
