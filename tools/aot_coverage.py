#!/usr/bin/env python3
"""aot_coverage.py -- mide la cobertura AOT sobre examples_codes_vex/.

Compila cada .vex con `-m aot --emit obj` y clasifica:
  - OK: se genero el .o.
  - FAIL-ANALYZE: aot_analyze rechaza una op RUNTIME_DEPENDENT (reporta cual).
  - FAIL-CODEGEN: el selector vreg no soporta una op.
  - FAIL-OTHER: error de compilacion del frontend, timeout, etc.

Agrupa los fallos por la op/categoria culpable para priorizar.

Uso:
  python tools/aot_coverage.py [vm_path] [--dir examples_codes_vex] [--show N]
"""
from __future__ import annotations
import argparse
import re
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path


def find_vm(explicit: str | None) -> Path:
    if explicit:
        return Path(explicit)
    for c in ("cmake-build-release/vm.exe", "cmake-build-release/vm",
              "build/vm.exe", "build/vm"):
        p = Path(c)
        if p.is_file():
            return p
    raise SystemExit("vm no encontrado; pasa la ruta como argumento")


# Regex para extraer el motivo de un fallo aot_analyze:
#   "[aot] op <NAME> no compilable ..." / "funcion 'X' op 'Y' (RUNTIME_DEPENDENT)"
RE_ANALYZE_OP = re.compile(r"\bop[:\s]+'?([A-Z_][A-Z0-9_]*)'?", re.I)
RE_SELECTOR = re.compile(r"selector.*no soporta.*'?([a-zA-Z_][a-zA-Z0-9_]*)'?")
RE_INCOMPAT = re.compile(r"incompatible|RUNTIME_DEPENDENT|no compilable|"
                         r"no soportad", re.I)


RE_OP_QUOTED = re.compile(r"op '([a-zA-Z_][a-zA-Z0-9_]*)'")


def classify(out: str, ok: bool) -> tuple[str, str]:
    """Devuelve (categoria, motivo)."""
    if ok:
        return ("OK", "")
    low = out.lower()
    # selector vreg (codegen): "el selector vreg no soporta la funcion ..."
    if "selector vreg no soporta" in low or "op fuera del subset" in low:
        return ("FAIL-CODEGEN", "vreg-unsupported")
    # aot_analyze: "fn 'X' linea N: op 'getproc' requiere ..."
    m = RE_OP_QUOTED.search(out)
    if m:
        return ("FAIL-ANALYZE", m.group(1))
    if RE_INCOMPAT.search(out):
        return ("FAIL-ANALYZE", "?")
    # compile-must-fail tests (borrow checker, static_assert, etc.)
    if "error:" in low:
        return ("FAIL-COMPILE", (re.findall(r"error: ?(.{0,50})", low)
                                 or ["?"])[0].strip())
    return ("FAIL-OTHER", (out.strip().splitlines() or ["?"])[-1][:80])


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("vm_path", nargs="?")
    ap.add_argument("--dir", default="examples_codes_vex")
    ap.add_argument("--recurse", action="store_true",
                    help="incluir subdirectorios (benchmark/, aot/, etc.)")
    ap.add_argument("--show", type=int, default=20,
                    help="cuantos ejemplos por categoria listar")
    ap.add_argument("--out", default="/tmp/aot_cov.o")
    args = ap.parse_args()

    vm = find_vm(args.vm_path)
    base = Path(args.dir)
    pat = "**/*.vex" if args.recurse else "*.vex"
    files = sorted(base.glob(pat))
    if not files:
        raise SystemExit(f"sin .vex en {base}")

    cat_count: Counter[str] = Counter()
    reason_count: Counter[str] = Counter()
    by_cat: dict[str, list[tuple[str, str]]] = defaultdict(list)

    for f in files:
        try:
            r = subprocess.run(
                [str(vm), "--vex", str(f), "-m", "aot", "--format", "elf",
                 "--emit", "obj", "-o", args.out],
                capture_output=True, text=True, timeout=60)
            out = (r.stdout + "\n" + r.stderr)
            ok = (r.returncode == 0 and Path(args.out).is_file())
        except subprocess.TimeoutExpired:
            out, ok = "TIMEOUT", False
        except Exception as e:  # noqa: BLE001
            out, ok = str(e), False
        # limpiar el .o para la siguiente iteracion
        try:
            Path(args.out).unlink()
        except OSError:
            pass
        cat, reason = classify(out, ok)
        cat_count[cat] += 1
        if cat != "OK":
            reason_count[f"{cat}:{reason}"] += 1
            by_cat[reason].append((f.name, cat))

    total = len(files)
    ok = cat_count.get("OK", 0)
    print(f"\n=== Cobertura AOT: {ok}/{total} "
          f"({100.0 * ok / total:.0f}%) compilan a .o ===\n")
    for c in ("FAIL-ANALYZE", "FAIL-CODEGEN", "FAIL-OTHER"):
        print(f"  {c}: {cat_count.get(c, 0)}")
    print("\n--- Motivos de fallo (mas frecuentes) ---")
    for reason, n in reason_count.most_common(args.show):
        print(f"  {n:3d}  {reason}")
    print("\n--- Ejemplos por motivo (top) ---")
    for reason, n in sorted(((r, len(v)) for r, v in by_cat.items()),
                            key=lambda x: -x[1])[:args.show]:
        names = ", ".join(nm for nm, _ in by_cat[reason][:6])
        more = "" if n <= 6 else f" (+{n - 6})"
        print(f"  [{reason}] x{n}: {names}{more}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
