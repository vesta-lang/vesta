#!/usr/bin/env python3
"""diff_harness.py -- red de seguridad diferencial interp vs JIT para VestaVM.

Sprint JIT-hardening (Fase 0).  Corre cada @c .vx del corpus en los 3 modos
de ejecucion y clasifica cada programa comparando contra el INTERPRETE como
oraculo.  Convierte bugs latentes del JIT (corrupcion silenciosa, divergencia
interp!=jit, crashes) en un backlog conocido ANTES de tocar el codegen.

Modos de ejecucion:
  - interp     : -m vm  (VESTA_JIT_THRESHOLD=UINT32_MAX), 100% interprete (oraculo).
  - jit-vreg   : -m jit (threshold=1), path por defecto (regalloc vreg).
  - jit-slots  : VESTA_JIT_VREGS=0 -m jit, path selector (slots).

Clasificacion por programa (el interp es el oraculo de correccion):
  OK         los 3 modos exit 0 y mismo R0.
  DIVERGE    interp da R0 pero algun JIT da R0 distinto  -> BUG del JIT.
  CRASH      interp da R0 (exit 0) pero algun JIT exit != 0 -> BUG del JIT.
  TIMEOUT    algun modo excede el timeout.
  NOCOMPILA  el .velb no se genero (puede ser test negativo esperado).
  NORUN      ningun modo produce R0 (no-ejecutable: modulo sin main, falta input...).
  NODET      en lista de no-deterministas conocidos (se reporta aparte, no es bug).

Salida: tabla clasificada en consola + diff_baseline.json con el detalle.

Uso:
  python tools/diff_harness.py [vm.exe] [--filter X] [--timeout S]
                               [--no-benchmarks] [--out diff_baseline.json]
"""
from __future__ import annotations
import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

R00_RE = re.compile(r"R00=0x([0-9a-fA-F]+)")

# Programas con R0 legitimamente no-determinista (punteros host, timestamps,
# direcciones GC) -> interp!=jit es esperado, NO un bug.  Se reportan aparte.
NODET = {
    "bug3_struct",            # puntero host en R0
    "160_macro_walk_pchase",  # macro: ERR en interp por diseno
}

# Programas que necesitan setup externo (loadmodule de un .velb concreto,
# argv, stdin) y no corren standalone en este harness.  No son bugs del JIT.
SKIP = {
    "49_loadmodule_caller",   # requiere _test_plugin.velb en el FS
}


class C:
    _on = sys.stdout.isatty() and not os.environ.get("NO_COLOR")
    R = "\033[0m" if _on else ""
    BOLD = "\033[1m" if _on else ""
    RED = "\033[31m" if _on else ""
    GRN = "\033[32m" if _on else ""
    YEL = "\033[33m" if _on else ""
    CYN = "\033[36m" if _on else ""
    DIM = "\033[2m" if _on else ""


def find_vm(arg: str | None, root: Path) -> Path | None:
    if arg and Path(arg).is_file():
        return Path(arg)
    for c in ("cmake-build-release/vm.exe", "cmake-build-release/vm",
              "build/vm.exe", "build/vm", "vm.exe", "vm"):
        p = root / c
        if p.is_file():
            return p
    return None


def discover(root: Path, want_bench: bool):
    """Devuelve [(name, path)] del corpus."""
    items = []
    for p in sorted((root / "examples_codes_vex").glob("*.vx")):
        items.append((p.stem, p))
    if want_bench:
        for d in sorted((root / "examples_codes_vex" / "benchmark").iterdir()):
            mv = d / "main.vx"
            if mv.is_file():
                items.append((d.name, mv))
    return items


def run_mode(vm: Path, velb: Path, mode: str, timeout: float, tmp: Path):
    """Corre el .velb en un modo.  Devuelve (status, r0) donde status es
    'ok'|'crash'|'timeout'|'norun' y r0 es la cadena hex (o None)."""
    env = dict(os.environ)
    cmd = [str(vm), "--run", str(velb), "--stats", "--schedulers", "1"]
    if mode == "interp":
        cmd += ["-m", "vm"]
    elif mode == "jit-vreg":
        cmd += ["-m", "jit"]
    elif mode == "jit-slots":
        cmd += ["-m", "jit"]
        env["VESTA_JIT_VREGS"] = "0"
    try:
        r = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=timeout, env=env, cwd=str(tmp), check=False)
    except subprocess.TimeoutExpired:
        return "timeout", None
    out = (r.stdout or "") + (r.stderr or "")
    m = R00_RE.search(out)
    r0 = m.group(1).lstrip("0") or "0" if m else None
    if r.returncode != 0:
        # exit != 0: crash si ademas no hay R0 limpio; igual lo marcamos crash.
        return "crash", r0
    if r0 is None:
        return "norun", None
    return "ok", r0


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("vm_path", nargs="?")
    ap.add_argument("--filter", default="")
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--no-benchmarks", action="store_true")
    ap.add_argument("--out", default="diff_baseline.json")
    args = ap.parse_args()

    vm = find_vm(args.vm_path, root)
    if not vm:
        print(f"{C.RED}[error]{C.R} no se encontro vm.exe (pasa la ruta)")
        return 1
    print(f"{C.CYN}[info]{C.R} vm: {vm}")

    tmp = root / "tmp" / "diff_harness"
    tmp.mkdir(parents=True, exist_ok=True)

    corpus = discover(root, not args.no_benchmarks)
    if args.filter:
        corpus = [(n, p) for (n, p) in corpus if args.filter in n]
    print(f"{C.CYN}[info]{C.R} corpus: {len(corpus)} programas, 3 modos, "
          f"timeout {args.timeout:.0f}s\n")

    cats = {k: [] for k in
            ("OK", "VREG_HANG", "DIVERGE", "CRASH", "SLOTS_BUG",
             "NO_ORACLE", "NOCOMPILA", "NODET", "SKIP")}
    detail = []
    t0 = time.time()

    for i, (name, path) in enumerate(corpus, 1):
        sys.stdout.write(f"\r{C.DIM}[{i}/{len(corpus)}] {name[:40]:40s}{C.R}")
        sys.stdout.flush()
        if name in SKIP:
            cats["SKIP"].append(name); continue

        # Compilar una vez.
        velb = tmp / (name + ".velb")
        try:
            cr = subprocess.run([str(vm), "--vex", str(path), "-o", str(tmp / name)],
                                capture_output=True, text=True, timeout=120, check=False)
        except subprocess.TimeoutExpired:
            cats["NOCOMPILA"].append(name); detail.append({"name": name, "cat": "NOCOMPILA",
                "note": "compile timeout"}); continue
        if cr.returncode != 0 or not velb.is_file():
            cats["NOCOMPILA"].append(name)
            detail.append({"name": name, "cat": "NOCOMPILA"})
            continue

        # 3 modos.
        res = {}
        for mode in ("interp", "jit-vreg", "jit-slots"):
            res[mode] = run_mode(vm, velb, mode, args.timeout, tmp)

        st = {m: res[m][0] for m in res}
        r0 = {m: res[m][1] for m in res}
        rec = {"name": name, "interp": res["interp"], "jit-vreg": res["jit-vreg"],
               "jit-slots": res["jit-slots"]}

        # Clasificar (interp = oraculo).  ORDEN IMPORTANTE: separar un HANG del
        # path de PRODUCCION (jit-vreg cuelga mientras el interp SI termina) de
        # un programa lento de por si.  Un jit-vreg timeout con interp OK ES un
        # BUG DE JIT (miscompilacion que rompe la terminacion; p.ej. el
        # coalescing que corrompe el contador del loop de state_machine).  Esta
        # clase escapaba al veredicto viejo porque TIMEOUT era categoria benigna.
        if st["interp"] != "ok":
            # El ORACULO no da un R0 valido (crash/timeout/norun) -> no hay con
            # que comparar el JIT -> NO es bug del JIT (p.ej. un programa que
            # crashea en los 3 modos por diseno, o que es lento de por si).
            cat = "NO_ORACLE"
        elif st["jit-vreg"] == "timeout":
            cat = "VREG_HANG"   # interp termina, vreg cuelga -> BUG DE PRODUCCION
        elif st["jit-vreg"] == "crash":
            cat = "CRASH"       # crash del path de produccion -> BUG
        elif r0["jit-vreg"] != r0["interp"]:
            cat = "DIVERGE"     # resultado vreg != oraculo -> BUG
        elif st["jit-slots"] in ("timeout", "crash") or \
                r0["jit-slots"] != r0["interp"]:
            cat = "SLOTS_BUG"   # ruta legacy (backlog conocido: no falla el gate)
        else:
            cat = "OK"
        # NODET: R0 legitimamente no-determinista (punteros host) -> no es bug.
        if name in NODET and cat in ("DIVERGE", "CRASH", "VREG_HANG"):
            cat = "NODET"
        rec["cat"] = cat
        cats[cat].append(name)
        if cat not in ("OK",):
            detail.append(rec)

    sys.stdout.write("\r" + " " * 60 + "\r")
    elapsed = time.time() - t0

    # Resumen.
    print(f"{C.BOLD}=== diff_harness: baseline ({elapsed:.0f}s) ==={C.R}")
    order = [("OK", C.GRN), ("VREG_HANG", C.RED), ("DIVERGE", C.RED),
             ("CRASH", C.RED), ("SLOTS_BUG", C.YEL), ("NO_ORACLE", C.DIM),
             ("NOCOMPILA", C.DIM), ("NODET", C.DIM), ("SKIP", C.DIM)]
    for k, col in order:
        print(f"  {col}{k:10s}{C.R} {len(cats[k]):3d}")

    # Los bugs reales del path de PRODUCCION (vreg): VREG_HANG + DIVERGE + CRASH.
    # SLOTS_BUG es la ruta legacy en jubilacion (backlog conocido) -> no falla.
    bug_cats = ("VREG_HANG", "DIVERGE", "CRASH")
    bugs = sum((cats[c] for c in bug_cats), [])
    if bugs:
        print(f"\n{C.RED}{C.BOLD}BUGS DEL JIT (path vreg de produccion):{C.R}")
        for rec in detail:
            if rec["cat"] in bug_cats:
                iv = rec.get("interp"); vg = rec.get("jit-vreg"); sl = rec.get("jit-slots")
                print(f"  {C.RED}{rec['cat']:9s}{C.R} {rec['name']:32s} "
                      f"interp={iv} vreg={vg} slots={sl}")
    else:
        print(f"\n{C.GRN}{C.BOLD}Sin bugs del path vreg (VREG_HANG/DIVERGE/CRASH).{C.R}")
    if cats["SLOTS_BUG"]:
        print(f"{C.YEL}[nota]{C.R} SLOTS_BUG (legacy en jubilacion): "
              f"{', '.join(cats['SLOTS_BUG'])}")

    out = root / args.out
    out.write_text(json.dumps({"vm": str(vm), "elapsed_s": elapsed,
        "summary": {k: len(v) for k, v in cats.items()},
        "categories": cats, "detail": detail}, indent=2), encoding="utf-8")
    print(f"\n{C.CYN}[ok]{C.R} baseline: {out}")
    # Exit code = numero de bugs del path de produccion (0 = limpio) -> usable
    # como gate de CI / bisect.  Antes SIEMPRE retornaba 0 (nunca fallaba).
    return 1 if bugs else 0


if __name__ == "__main__":
    sys.exit(main())
