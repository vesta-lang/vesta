#!/usr/bin/env python3
"""build_and_test.py -- construye y valida, sin que el resultado pueda mentir.

Compilar y filtrar la salida a mano tiene una trampa que ya costo una tanda de
trabajo: si el filtro mira solo un fichero (`grep ir_emitter.cpp`), los errores
de una CABECERA no aparecen.  El build falla, el `vm.exe` anterior sigue en su
sitio, y la suite corre contra el binario viejo dando todo por verde.

Esto lo cierra por construccion:

  1. usa el CODIGO DE SALIDA del compilador, no lo que se lea de su salida;
  2. comprueba que el binario es MAS NUEVO que las fuentes -- si el enlace no
     llego a escribirlo (fichero bloqueado, por ejemplo), se ve aqui;
  3. solo entonces corre la suite.

Uso:
    python tools/build_and_test.py [build_dir] [-j N] [--solo-build]
"""

import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def mtime_maxima(directorios, extensiones):
    """@brief La fecha del fuente modificado mas recientemente.

    @param directorios Rutas relativas a la raiz.
    @param extensiones Sufijos que cuentan como fuente.
    @return Marca de tiempo, o 0 si no se encontro ninguno.
    """
    ultima = 0.0
    for d in directorios:
        base = os.path.join(ROOT, d)
        for dirpath, _dirs, ficheros in os.walk(base):
            for f in ficheros:
                if f.endswith(extensiones):
                    try:
                        ultima = max(ultima, os.path.getmtime(os.path.join(dirpath, f)))
                    except OSError:
                        pass
    return ultima


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("build_dir", nargs="?", default="cmake-build-release")
    ap.add_argument("-j", type=int, default=6)
    ap.add_argument("--solo-build", action="store_true")
    ap.add_argument("--target", default="vm")
    args = ap.parse_args()

    build = args.build_dir
    if not os.path.isabs(build):
        build = os.path.join(ROOT, build)
    binario = os.path.join(build, "vm.exe")
    if not os.path.exists(binario):
        binario = os.path.join(build, "vm")

    # -- 1. compilar; manda el codigo de salida ----------------------------
    res = subprocess.run(
        ["cmake", "--build", build, "--target", args.target, "-j", str(args.j)],
        cwd=ROOT, capture_output=True,
    )
    salida = (res.stdout or b"").decode("utf-8", "replace") + \
             (res.stderr or b"").decode("utf-8", "replace")
    errores = [l for l in salida.splitlines() if ": error:" in l or l.startswith("FAILED")]
    if res.returncode != 0:
        print("BUILD FALLIDO ({} lineas de error):".format(len(errores)))
        for l in errores[:20]:
            print("  " + l[:160])
        return 1

    # -- 2. el binario tiene que ser mas nuevo que las fuentes -------------
    if not os.path.exists(binario):
        print("BUILD sin errores pero NO hay binario en " + binario)
        return 1
    fuentes = mtime_maxima(["src", "include", "main.cpp"], (".cpp", ".h", ".c"))
    if os.path.getmtime(binario) < fuentes:
        print("BUILD sin errores pero el binario es MAS VIEJO que las fuentes.")
        print("  binario: " + binario)
        print("  (enlace no escrito: fichero bloqueado, o build en otro directorio)")
        return 1
    print("build OK: " + binario)
    if args.solo_build:
        return 0

    # -- 3. la suite, que es la que decide ---------------------------------
    suite = subprocess.run(
        [sys.executable, os.path.join("tests", "vx", "e2e_test.py"),
         args.build_dir, "-j", "8"],
        cwd=ROOT,
    )
    return suite.returncode


if __name__ == "__main__":
    sys.exit(main())
