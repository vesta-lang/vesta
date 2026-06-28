#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
bench_vectorize.py -- Mide la ganancia de la auto-vectorizacion del JIT.

Para cada kernel vectorizable (element-wise, reduccion, dot-product/FMA) compila
TRES variantes y las ejecuta con el JIT (-m jit), midiendo el wall time:

  * escalar : VESTA_NO_VECTORIZE=1  -> el bucle NO se vectoriza (linea base).
  * sse2    : VESTA_VEC_ISA=sse2    -> chunk de 128b (W=2 f64).
  * avx2    : VESTA_VEC_ISA=avx2    -> chunk de 256b (W=4 f64).

(AVX512 no se mide por ejecucion: esta CPU no tiene avx512f; su codegen se valida
por disassembly aparte.)

El chunk se hornea en COMPILE time (VESTA_VEC_ISA); el JIT emite al ancho del host
(AUTO).  Verifica ademas que el resultado (R00) coincide entre las tres variantes.

Uso:  python tools/bench_vectorize.py [--build-dir cmake-build-release]
                                      [--reps N] [--runs K] [--n ELEMS]
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
import time

# Kernels: nombre -> (dtype, BLOQUE interior).  El bloque contiene el bucle
# vectorizable + una sentencia que mantiene `s` vivo.  Las reducciones/dot usan
# un acumulador `acc` LOCAL (no `s`) para que el acumulador vectorial sea
# register-resident.  dtype determina el tamano de elemento y los valores init.
#   f64  -> element-wise/reduccion/dot/scalar-bcast (todas las ops)
#   i32  -> element-wise add/mul (PADDD/PMULLD) + scalar-bcast
#   i16  -> element-wise add/mul (PADDW/PMULLW)
KERNELS = [
    ("ew f64 (c=a+b)",       "f64",
        "for (i32 i=0;i<n;i++){ c[i]=a[i]+b[i]; } s=s+c[0]+a[0];"),
    ("reduccion f64",        "f64",
        "f64 acc=0.0; for (i32 i=0;i<n;i++){ acc=acc+a[i]; } s=s+acc;"),
    ("dot/FMA f64",          "f64",
        "f64 acc=0.0; for (i32 i=0;i<n;i++){ acc=acc+a[i]*b[i]; } s=s+acc;"),
    ("scalar-bcast f64 (a*k)", "f64",
        "for (i32 i=0;i<n;i++){ c[i]=a[i]*3.0; } s=s+c[0];"),
    ("ew i32 (c=a+b)",       "i32",
        "for (i32 i=0;i<n;i++){ c[i]=a[i]+b[i]; } s=s+(f64)c[0];"),
    ("ew i32 mul (c=a*b)",   "i32",
        "for (i32 i=0;i<n;i++){ c[i]=a[i]*b[i]; } s=s+(f64)c[0];"),
    ("ew i16 (c=a+b)",       "i16",
        "for (i32 i=0;i<n;i++){ c[i]=a[i]+b[i]; } s=s+(f64)c[0];"),
    ("scalar-bcast i32 (a*k)", "i32",
        "for (i32 i=0;i<n;i++){ c[i]=a[i]*3; } s=s+(f64)c[0];"),
]

# Init + sizeof por dtype.  Los enteros usan valores pequenos (sin overflow).
DTYPE_INFO = {
    "f64": {"sz": 8, "ia": "1.0", "ib": "2.0", "ic": "0.0"},
    "i32": {"sz": 4, "ia": "(i32)1", "ib": "(i32)2", "ic": "(i32)0"},
    "i16": {"sz": 2, "ia": "(i16)1", "ib": "(i16)2", "ic": "(i16)0"},
}

VEX_TEMPLATE = """\
// Generado por tools/bench_vectorize.py -- benchmark de auto-vectorizacion.
f64 kernel({dt}* a, {dt}* b, {dt}* c, i32 n, i32 reps) {{
    f64 s = 0.0;
    for (i32 r = 0; r < reps; r++) {{
        {inner}
    }}
    return s;
}}

i64 main() {{
    i32 n = {n};
    {dt}* a = ({dt}*) malloc(n * {sz});
    {dt}* b = ({dt}*) malloc(n * {sz});
    {dt}* c = ({dt}*) malloc(n * {sz});
    for (i32 i = 0; i < n; i++) {{ a[i] = {ia}; b[i] = {ib}; c[i] = {ic}; }}
    f64 r = kernel(a, b, c, n, {reps});
    free(a); free(b); free(c);
    return (i64) r;
}}
"""

MODES = [
    ("escalar", {"VESTA_NO_VECTORIZE": "1"}),
    ("sse2",    {"VESTA_VEC_ISA": "sse2"}),
    ("avx2",    {"VESTA_VEC_ISA": "avx2"}),
]


def run(cmd, env=None, timeout=600):
    """Ejecuta un comando capturando salida; devuelve (rc, stdout+stderr)."""
    full_env = dict(os.environ)
    if env:
        full_env.update(env)
    p = subprocess.run(cmd, env=full_env, capture_output=True, text=True,
                       timeout=timeout)
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def extract_r00(out):
    """Extrae el valor R00=0x... de la salida de --stats (o None)."""
    m = re.search(r"R00=0x([0-9a-fA-F]+)", out)
    return m.group(1).lstrip("0") or "0" if m else None


def compile_variant(vm, vex_path, out_base, env):
    """Compila vex_path -> out_base.velb con el env del modo.  Devuelve el .velb."""
    rc, out = run([vm, "--vex", vex_path, "-o", out_base], env=env)
    velb = out_base + ".velb"
    if rc != 0 or not os.path.exists(velb):
        raise RuntimeError("fallo al compilar (%s):\n%s" % (env, out[-800:]))
    return velb


def time_run(vm, velb, runs):
    """Ejecuta el .velb con -m jit `runs` veces; devuelve (min_segundos, r00)."""
    best = None
    r00 = None
    for _ in range(runs):
        t0 = time.perf_counter()
        rc, out = run([vm, "--run", velb, "-m", "jit", "--stats"])
        dt = time.perf_counter() - t0
        if rc != 0:
            raise RuntimeError("fallo en ejecucion:\n%s" % out[-800:])
        r00 = extract_r00(out)
        best = dt if best is None else min(best, dt)
    return best, r00


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", default="cmake-build-release")
    ap.add_argument("--n", type=int, default=200000, help="elementos del array")
    ap.add_argument("--reps", type=int, default=2000,
                    help="repeticiones del bucle externo")
    ap.add_argument("--runs", type=int, default=5,
                    help="ejecuciones por variante (se toma el minimo)")
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    vm = os.path.join(root, args.build_dir, "vm.exe")
    if not os.path.exists(vm):
        vm = os.path.join(root, args.build_dir, "vm")
    if not os.path.exists(vm):
        sys.exit("no se encuentra el binario vm en %s" % args.build_dir)

    total = args.n * args.reps
    print("== Benchmark auto-vectorizacion (JIT) ==")
    print("vm=%s" % vm)
    print("n=%d  reps=%d  ops_internas=%.1fM  runs=%d (min)\n"
          % (args.n, args.reps, total / 1e6, args.runs))

    hdr = "%-24s %10s %10s %10s   %8s %8s" % (
        "kernel", "escalar", "sse2", "avx2", "sse2x", "avx2x")
    print(hdr)
    print("-" * len(hdr))

    tmp = tempfile.mkdtemp(prefix="vecbench_")
    for name, dt, inner in KERNELS:
        di = DTYPE_INFO[dt]
        src = VEX_TEMPLATE.format(inner=inner, n=args.n, reps=args.reps,
                                  dt=dt, sz=di["sz"], ia=di["ia"], ib=di["ib"],
                                  ic=di["ic"])
        vex_path = os.path.join(tmp, "k.vex")
        with open(vex_path, "w") as f:
            f.write(src)

        times = {}
        r00s = {}
        for mode, env in MODES:
            out_base = os.path.join(tmp, "k_" + mode)
            velb = compile_variant(vm, vex_path, out_base, env)
            secs, r00 = time_run(vm, velb, args.runs)
            times[mode] = secs
            r00s[mode] = r00

        # correctness: las 3 variantes deben dar el MISMO R00.
        vals = set(v for v in r00s.values() if v is not None)
        ok = "" if len(vals) <= 1 else "  <!> DIVERGE %s" % r00s

        ts, ss, av = times["escalar"], times["sse2"], times["avx2"]
        print("%-24s %9.3fs %9.3fs %9.3fs   %7.2fx %7.2fx%s" % (
            name, ts, ss, av, ts / ss, ts / av, ok))

    print("\n(sse2x/avx2x = speedup vs escalar; minimo de %d ejecuciones)" % args.runs)


if __name__ == "__main__":
    main()
