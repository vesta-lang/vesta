#!/usr/bin/env python3
"""format_code.py -- pasa el formateador del proyecto a TODO el codigo propio.

El estilo del proyecto vive en el `.clang-format` de la raiz, y es el mismo que
usa clang-tidy (`FormatStyle: file` en `.clang-tidy`).  Un solo estilo, lo
aplique quien lo aplique: la herramienta, el analizador estatico o el IDE.

Por que existe este script y no un `clang-format -i` a pelo: hay tres clases de
fichero que NO se deben tocar y que un comodin se lleva por delante --

  - `libs/`   codigo VENDORIZADO (keystone, capstone, ...).  Formatearlo
              destruiria el diff contra el upstream del que viene.
  - `src/vx/gen/`  ficheros GENERADOS ("NO editar a mano").  Formatearlos hace
              que el siguiente pase del generador los deshaga, asi que el
              formateo seria ruido que reaparece en cada regeneracion.
  - los directorios de compilacion (`cmake-build-*`, `wsl-asan`, ...), que
              contienen copias y fuentes de terceros.

Uso:
    python tools/format_code.py            # formatea (modifica los ficheros)
    python tools/format_code.py --check    # NO modifica; sale != 0 si falta algo
    python tools/format_code.py --submodule  # incluye tambien preprocessor/

`--check` es lo que hace exigible el "siempre": vale para un gancho de commit o
para la CI, porque responde con codigo de salida en vez de con un listado que
haya que leer.

`preprocessor/` es un submodulo -- otro repositorio --, asi que queda fuera por
defecto: formatearlo produce cambios que hay que commitear ALLI y que mueven el
puntero del submodulo, y eso no debe pasar de rebote al formatear el padre.
"""

import argparse
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

# Raiz del repositorio: este script vive en tools/.
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Donde hay codigo NUESTRO.  El submodulo se anade aparte con --submodule.
SOURCE_DIRS = ["src", "include", "tests", "tools", "stdlib"]
SOURCE_FILES = ["main.cpp"]
SUBMODULE_DIRS = ["preprocessor"]

# Prefijos (relativos a la raiz) que se excluyen SIEMPRE.  Ver la cabecera.
EXCLUDED_PREFIXES = ("libs/", "src/vx/gen/", "cmake-build-", "wsl-asan/", "tmp/")

EXTENSIONS = (".c", ".h", ".cpp", ".hpp", ".cc", ".hh")


def is_excluded(rel_path):
    """@brief Decide si un fichero queda fuera del formateo.

    @param rel_path Ruta relativa a la raiz, con separadores `/`.
    @return true si NO se debe formatear.
    """
    return rel_path.startswith(EXCLUDED_PREFIXES)


def collect(dirs, files):
    """@brief Reune los ficheros de codigo propios, ya filtrados.

    @param dirs  Directorios a recorrer, relativos a la raiz.
    @param files Ficheros sueltos a incluir, relativos a la raiz.
    @return Lista de rutas relativas ordenada, sin los excluidos.
    """
    found = []
    for d in dirs:
        base = os.path.join(ROOT, d)
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            # Podar la rama entera en vez de filtrar hoja a hoja: recorrer
            # `libs/` para descartar cada fichero cuesta mas que no entrar.
            dirnames[:] = [
                n
                for n in dirnames
                if not is_excluded(
                    os.path.relpath(os.path.join(dirpath, n), ROOT).replace("\\", "/")
                    + "/"
                )
            ]
            for name in filenames:
                if not name.endswith(EXTENSIONS):
                    continue
                rel = os.path.relpath(os.path.join(dirpath, name), ROOT)
                rel = rel.replace("\\", "/")
                if not is_excluded(rel):
                    found.append(rel)
    for f in files:
        if os.path.isfile(os.path.join(ROOT, f)):
            found.append(f)
    return sorted(found)


def clang_format_binary():
    """@brief Localiza el clang-format a usar.

    @return Ruta al ejecutable, o None si no hay ninguno.
    """
    from shutil import which

    found = which("clang-format")
    if found:
        return found
    # Sitio habitual en Windows cuando LLVM se instalo sin tocar el PATH.
    fallback = r"C:\Program Files\LLVM\bin\clang-format.exe"
    return fallback if os.path.isfile(fallback) else None


def run_one(binary, rel, check):
    """@brief Formatea (o comprueba) un fichero.

    @param binary Ejecutable de clang-format.
    @param rel    Ruta relativa del fichero.
    @param check  Si true, no modifica nada y solo informa.
    @return La ruta si el fichero NO estaba formateado, o None si ya lo estaba.
    """
    style = "--style=file:" + os.path.join(ROOT, ".clang-format")
    if check:
        # --dry-run -Werror no escribe y devuelve != 0 cuando algo cambiaria,
        # que es justo la pregunta que hace --check.
        res = subprocess.run(
            [binary, style, "--dry-run", "-Werror", rel],
            cwd=ROOT,
            capture_output=True,
        )
        return rel if res.returncode != 0 else None
    # Sin --check hay que saber si CAMBIO algo para poder informar del recuento,
    # y el propio -i no lo dice: se pregunta antes.
    before = subprocess.run(
        [binary, style, "--dry-run", "-Werror", rel], cwd=ROOT, capture_output=True
    )
    subprocess.run([binary, style, "-i", rel], cwd=ROOT, capture_output=True)
    return rel if before.returncode != 0 else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="no modifica nada; sale != 0 si algun fichero no esta formateado",
    )
    parser.add_argument(
        "--submodule",
        action="store_true",
        help="incluye tambien el submodulo preprocessor/ (otro repositorio)",
    )
    parser.add_argument(
        "-j", type=int, default=os.cpu_count() or 4, help="procesos en paralelo"
    )
    args = parser.parse_args()

    binary = clang_format_binary()
    if binary is None:
        print("error: no se encuentra clang-format (ni en PATH ni en LLVM/bin)")
        return 2

    dirs = list(SOURCE_DIRS) + (SUBMODULE_DIRS if args.submodule else [])
    files = collect(dirs, SOURCE_FILES)
    if not files:
        print("error: no se encontro ningun fichero de codigo")
        return 2

    with ThreadPoolExecutor(max_workers=args.j) as pool:
        results = list(pool.map(lambda f: run_one(binary, f, args.check), files))
    touched = [r for r in results if r]

    if args.check:
        if touched:
            print("sin formatear ({} de {}):".format(len(touched), len(files)))
            for f in touched[:40]:
                print("  " + f)
            if len(touched) > 40:
                print("  ... y {} mas".format(len(touched) - 40))
            print("\narreglalo con:  python tools/format_code.py")
            return 1
        print("formato OK: {} ficheros".format(len(files)))
        return 0

    print("formateados {} de {} ficheros".format(len(touched), len(files)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
