#!/usr/bin/env python3
"""aot_heap_audit.py -- audita el uso de HEAP (malloc/calloc/free/realloc) en
los .o que el AOT genera para examples_codes_vex/.

Para cada .vex que compila a .o, hace `nm` y reporta si quedan simbolos libc de
heap sin resolver.  Agrupa los que SI usan heap para priorizar el barrido
stack-first (convertir malloc local no-escapante -> ALLOCA de pila).

Uso:
  python tools/aot_heap_audit.py [vm_path] [--dir examples_codes_vex] [--recurse]
"""
from __future__ import annotations
import argparse
import re
import subprocess
import sys
from pathlib import Path

HEAP_RE = re.compile(r"^\s+U\s+(malloc|calloc|free|realloc)\b", re.M)


def find_vm(explicit: str | None) -> str:
    if explicit:
        return str(Path(explicit).resolve())
    for c in ("cmake-build-release/vm.exe", "cmake-build-release/vm",
              "build/vm.exe", "build/vm"):
        if Path(c).is_file():
            return str(Path(c).resolve())
    raise SystemExit("vm no encontrado")


def nm_tool() -> str:
    # nm de WSL/MinGW; en Windows el del toolchain.
    for c in ("nm", "F:/msys/ucrt64/bin/nm.exe"):
        try:
            subprocess.run([c, "--version"], capture_output=True, timeout=5)
            return c
        except Exception:  # noqa: BLE001
            continue
    return "nm"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("vm_path", nargs="?")
    ap.add_argument("--dir", default="examples_codes_vex")
    ap.add_argument("--recurse", action="store_true")
    ap.add_argument("--out", default="cmake-build-release/_heap_audit.o")
    args = ap.parse_args()

    vm = find_vm(args.vm_path)
    nm = nm_tool()
    args.out = str(Path(args.out).resolve())
    base = Path(args.dir)
    pat = "**/*.vex" if args.recurse else "*.vex"
    files = sorted(base.glob(pat))

    compiled = 0
    heap_users: list[tuple[str, set[str]]] = []
    clean: list[str] = []

    for f in files:
        out = Path(args.out)
        try:
            out.unlink()
        except OSError:
            pass
        try:
            r = subprocess.run(
                [vm, "--vex", str(f), "-m", "aot", "--format", "elf",
                 "--emit", "obj", "-o", str(out)],
                capture_output=True, text=True, timeout=60)
        except Exception:  # noqa: BLE001
            continue
        if r.returncode != 0 or not out.is_file():
            continue
        compiled += 1
        nmr = subprocess.run([nm, str(out)], capture_output=True, text=True)
        syms = set(HEAP_RE.findall(nmr.stdout))
        if syms:
            heap_users.append((f.name, syms))
        else:
            clean.append(f.name)
    try:
        Path(args.out).unlink()
    except OSError:
        pass

    print(f"\n=== Heap audit: {compiled} ejemplos compilan a .o ===")
    print(f"  sin heap (puro stack/.rodata): {len(clean)}")
    print(f"  con heap (malloc/free/...):    {len(heap_users)}\n")
    print("--- Ejemplos que AUN usan heap ---")
    for name, syms in sorted(heap_users):
        print(f"  {name:42s} {', '.join(sorted(syms))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
