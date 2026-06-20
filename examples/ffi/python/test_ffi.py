#!/usr/bin/env python3
"""
test_ffi.py -- ejemplo de consumo de libvesta desde Python via ctypes.

Carga la libreria compartida (vesta.dll / libvesta.so), declara las firmas
de la API C, y ejecuta snippets Vex verificando su valor de retorno.

Uso:
    python test_ffi.py [ruta_a_la_libreria]

Si no se pasa ruta, intenta localizar la libreria en el directorio
cmake-build-release relativo a la raiz del repo.  En Windows, las DLL de
OpenSSL (libssl-3-x64.dll, libcrypto-3-x64.dll) deben estar junto a
libvesta.dll o en el PATH.
"""

import ctypes
import os
import sys


def find_library() -> str:
    """Localiza la libreria compartida de VestaVM."""
    if len(sys.argv) > 1:
        return sys.argv[1]
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.abspath(os.path.join(here, "..", "..", ".."))
    build = os.path.join(repo, "cmake-build-release")
    candidates = [
        os.path.join(build, "libvesta.dll"),  # Windows / MinGW
        os.path.join(build, "vesta.dll"),
        os.path.join(build, "libvesta.so"),  # POSIX
    ]
    for c in candidates:
        if os.path.exists(c):
            return c
    raise FileNotFoundError(
        "No se encontro libvesta; pasa la ruta como argumento.\n"
        "Probadas:\n  " + "\n  ".join(candidates)
    )


def load_api(path: str) -> ctypes.CDLL:
    """Carga el DLL/.so y declara las firmas de la API."""
    # En Windows, anyadir el directorio del DLL al search path para que
    # encuentre las dependencias (OpenSSL) en Python 3.8+.
    dll_dir = os.path.dirname(os.path.abspath(path))
    if hasattr(os, "add_dll_directory") and os.name == "nt":
        os.add_dll_directory(dll_dir)

    lib = ctypes.CDLL(path)

    lib.vesta_version.restype = ctypes.c_char_p
    lib.vesta_version.argtypes = []

    lib.vesta_eval.restype = ctypes.c_int
    lib.vesta_eval.argtypes = [
        ctypes.c_char_p,  # src
        ctypes.c_char_p,  # unit_name
        ctypes.POINTER(ctypes.c_int),  # out_exit
        ctypes.POINTER(ctypes.c_char_p),  # out_err
    ]

    lib.vesta_compile.restype = ctypes.c_int
    lib.vesta_compile.argtypes = [
        ctypes.c_char_p,  # src
        ctypes.c_char_p,  # unit_name
        ctypes.POINTER(ctypes.POINTER(ctypes.c_ubyte)),  # out_velb
        ctypes.POINTER(ctypes.c_size_t),  # out_len
        ctypes.POINTER(ctypes.c_char_p),  # out_err
    ]

    lib.vesta_run.restype = ctypes.c_int
    lib.vesta_run.argtypes = [
        ctypes.POINTER(ctypes.c_ubyte),  # velb
        ctypes.c_size_t,  # len
        ctypes.c_int,  # argc
        ctypes.POINTER(ctypes.c_char_p),  # argv
        ctypes.POINTER(ctypes.c_int),  # out_exit
        ctypes.POINTER(ctypes.c_char_p),  # out_err
    ]

    lib.vesta_free.restype = None
    lib.vesta_free.argtypes = [ctypes.c_void_p]

    return lib


def vesta_eval(lib: ctypes.CDLL, src: str, unit: str) -> int:
    """Compila + ejecuta una fuente Vex; devuelve el valor de retorno (R0)."""
    out_exit = ctypes.c_int(0)
    out_err = ctypes.c_char_p()
    rc = lib.vesta_eval(
        src.encode("utf-8"),
        unit.encode("utf-8"),
        ctypes.byref(out_exit),
        ctypes.byref(out_err),
    )
    if rc != 0:
        msg = out_err.value.decode("utf-8") if out_err.value else "(sin mensaje)"
        # Liberar el mensaje con la API (se asigno con malloc del DLL).
        lib.vesta_free(out_err)
        raise RuntimeError(f"vesta_eval fallo (rc={rc}): {msg}")
    return out_exit.value


def main() -> int:
    path = find_library()
    lib = load_api(path)
    print("libvesta version:", lib.vesta_version().decode("utf-8"))

    r = vesta_eval(lib, "i32 main() { return 42; }", "py_42")
    print(f"vesta_eval(return 42) -> {r}")
    assert r == 42, f"esperado 42, obtenido {r}"

    r = vesta_eval(lib, "i32 main() { return 6 * 7; }", "py_mul")
    print(f"vesta_eval(6 * 7) -> {r}")
    assert r == 42, f"esperado 42, obtenido {r}"

    print("\nTODOS LOS CASOS OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
