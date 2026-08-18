#!/usr/bin/env python3
"""run_unit_tests.py -- ejecuta los binarios de test unitario y resume.

Cada `.cpp` de `tests/` produce un ejecutable propio; el proyecto no usa ctest,
asi que sin esto no hay forma de correrlos todos de una vez.  Y sin forma de
correrlos, no se corren: asi es como un test dejo de compilar al cambiar la
firma de una funcion y siguio roto sin que nadie se enterara.

Un test se considera PASS si termina con codigo 0.  Los que se cuelgan se matan
al llegar al tiempo limite y cuentan como TIMEOUT, no como fallo silencioso.

Uso:
    python tools/run_unit_tests.py [build_dir] [-k patron] [--timeout N] [-j N]
"""

import argparse
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def say(text):
    """@brief Imprime sin morir por lo que no quepa en la consola.

    La salida de un test puede traer color ANSI y caracteres que la consola de
    Windows no sabe representar.  Sin esto, el resumen entero se pierde por un
    byte de un test: el informe reventaba justo donde iba a decir que fallo.
    """
    enc = getattr(sys.stdout, "encoding", None) or "utf-8"
    sys.stdout.write(text.encode(enc, "replace").decode(enc, "replace") + "\n")


def collect(build_dir, pattern):
    """@brief Reune los ejecutables de test del directorio de build.

    @param build_dir Directorio de compilacion.
    @param pattern   Subcadena que debe contener el nombre, o None.
    @return Lista de rutas, ordenada.
    """
    found = []
    for dirpath, _dirnames, filenames in os.walk(build_dir):
        for name in filenames:
            if not name.startswith("test_"):
                continue
            if not name.endswith(".exe") and "." in name:
                continue
            if pattern and pattern not in name:
                continue
            found.append(os.path.join(dirpath, name))
    return sorted(found)


def run_one(path, timeout):
    """@brief Ejecuta un test.
    @return (nombre, estado, salida) con estado en PASS/FAIL/TIMEOUT.
    """
    name = os.path.basename(path)
    try:
        res = subprocess.run(
            [path], capture_output=True, timeout=timeout, cwd=ROOT
        )
    except subprocess.TimeoutExpired:
        return (name, "TIMEOUT", "")
    except OSError as e:
        return (name, "FAIL", str(e))
    out = (res.stdout or b"").decode("utf-8", "replace")
    err = (res.stderr or b"").decode("utf-8", "replace")
    return (name, "PASS" if res.returncode == 0 else "FAIL", out + err)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_dir", nargs="?", default="cmake-build-release")
    parser.add_argument("-k", dest="pattern", help="filtra por nombre")
    parser.add_argument("--timeout", type=int, default=120, help="segundos por test")
    parser.add_argument("-j", type=int, default=max(1, (os.cpu_count() or 4) // 2))
    args = parser.parse_args()

    build_dir = args.build_dir
    if not os.path.isabs(build_dir):
        build_dir = os.path.join(ROOT, build_dir)
    tests = collect(build_dir, args.pattern)
    if not tests:
        print("no se encontro ningun ejecutable de test en " + build_dir)
        return 2

    with ThreadPoolExecutor(max_workers=args.j) as pool:
        results = list(pool.map(lambda t: run_one(t, args.timeout), tests))

    bad = [r for r in results if r[1] != "PASS"]
    for name, state, out in bad:
        say("== {} [{}]".format(name, state))
        # Las ultimas lineas son donde el test dice que fallo; el resto es ruido.
        tail = [l for l in out.splitlines() if l.strip()][-12:]
        for line in tail:
            say("   " + line[:160])

    print(
        "\n=== tests unitarios: {} OK, {} fallidos de {}".format(
            len(results) - len(bad), len(bad), len(results)
        )
    )
    if bad:
        print("   " + ", ".join(n for n, _s, _o in bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
