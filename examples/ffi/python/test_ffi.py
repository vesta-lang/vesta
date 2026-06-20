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

    lib.vesta_compile_to_vel.restype = ctypes.c_int
    lib.vesta_compile_to_vel.argtypes = [
        ctypes.c_char_p,  # src
        ctypes.c_char_p,  # unit_name
        ctypes.POINTER(ctypes.c_char_p),  # out_vel
        ctypes.POINTER(ctypes.c_char_p),  # out_err
    ]

    lib.vesta_compile_to_ir.restype = ctypes.c_int
    lib.vesta_compile_to_ir.argtypes = [
        ctypes.c_char_p,  # src
        ctypes.c_char_p,  # unit_name
        ctypes.POINTER(ctypes.c_char_p),  # out_ir
        ctypes.POINTER(ctypes.c_char_p),  # out_err
    ]

    lib.vesta_disasm.restype = ctypes.c_int
    lib.vesta_disasm.argtypes = [
        ctypes.POINTER(ctypes.c_ubyte),  # bytes
        ctypes.c_size_t,  # len
        ctypes.c_char_p,  # arch
        ctypes.POINTER(ctypes.c_char_p),  # out_text
        ctypes.POINTER(ctypes.c_char_p),  # out_err
    ]

    lib.vesta_diagram.restype = ctypes.c_int
    lib.vesta_diagram.argtypes = [
        ctypes.c_char_p,  # src
        ctypes.c_char_p,  # unit_name
        ctypes.c_char_p,  # kind
        ctypes.c_char_p,  # format
        ctypes.POINTER(ctypes.c_char_p),  # out_text
        ctypes.POINTER(ctypes.c_char_p),  # out_err
    ]

    lib.vesta_vsh_eval.restype = ctypes.c_int
    lib.vesta_vsh_eval.argtypes = [
        ctypes.c_char_p,  # script
        ctypes.POINTER(ctypes.c_int),  # out_rc
        ctypes.POINTER(ctypes.c_char_p),  # out_err
    ]

    lib.vesta_free.restype = None
    lib.vesta_free.argtypes = [ctypes.c_void_p]

    return lib


def _call_text(lib, fn, *args):
    """Invoca una fn (...) -> 0/err que devuelve UNA cadena por out_text.

    args = los argumentos previos a (out_text, out_err).  Devuelve la
    cadena resultante (str) liberando el buffer con vesta_free.
    """
    out_text = ctypes.c_char_p()
    out_err = ctypes.c_char_p()
    rc = fn(*args, ctypes.byref(out_text), ctypes.byref(out_err))
    if rc != 0:
        msg = out_err.value.decode("utf-8") if out_err.value else "(sin mensaje)"
        lib.vesta_free(out_err)
        raise RuntimeError(f"{fn.__name__} fallo (rc={rc}): {msg}")
    text = out_text.value.decode("utf-8") if out_text.value else ""
    lib.vesta_free(out_text)
    return text


def compile_to_ir(lib, src, unit):
    """Vex -> texto del IR SSA."""
    return _call_text(
        lib, lib.vesta_compile_to_ir, src.encode("utf-8"), unit.encode("utf-8")
    )


def diagram(lib, src, unit, kind, fmt):
    """Vex -> diagrama del pipeline (mermaid|graphviz|html)."""
    return _call_text(
        lib,
        lib.vesta_diagram,
        src.encode("utf-8"),
        unit.encode("utf-8"),
        kind.encode("utf-8"),
        fmt.encode("utf-8"),
    )


def disasm(lib, code: bytes, arch: str):
    """Desensambla un buffer de bytes nativos."""
    buf = (ctypes.c_ubyte * len(code)).from_buffer_copy(code)
    return _call_text(
        lib, lib.vesta_disasm, buf, ctypes.c_size_t(len(code)), arch.encode("utf-8")
    )


def vsh_eval(lib, script: str) -> int:
    """Ejecuta un script VestaShellScript; devuelve out_rc."""
    out_rc = ctypes.c_int(0)
    out_err = ctypes.c_char_p()
    rc = lib.vesta_vsh_eval(
        script.encode("utf-8"), ctypes.byref(out_rc), ctypes.byref(out_err)
    )
    if rc != 0:
        msg = out_err.value.decode("utf-8") if out_err.value else "(sin mensaje)"
        lib.vesta_free(out_err)
        raise RuntimeError(f"vesta_vsh_eval fallo (rc={rc}): {msg}")
    lib.vesta_free(out_err)
    return out_rc.value


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

    # --- compile_to_ir ---
    ir = compile_to_ir(lib, "i32 main() { return 6 * 7; }", "py_ir")
    print("\n--- IR (head) ---")
    print(ir[:200] + (" [...]" if len(ir) > 200 else ""))
    assert "@module" in ir, "IR sin cabecera @module"

    # --- diagram (Mermaid del IR post-opt) ---
    mmd = diagram(lib, "i32 main() { return 6 * 7; }", "py_diag", "ir-post", "mermaid")
    print("\n--- diagrama Mermaid ir-post (head) ---")
    print(mmd[:160] + (" [...]" if len(mmd) > 160 else ""))
    assert "mermaid" in mmd.lower(), "diagrama Mermaid invalido"

    # --- disasm (mov eax,42 ; ret) ---
    text = disasm(lib, b"\xB8\x2A\x00\x00\x00\xC3", "X86-64")
    print("\n--- desensamblado X86-64 ---")
    print(text, end="")
    assert "mov" in text and "ret" in text, "desensamblado inesperado"

    # --- vsh_eval (script VestaShellScript) ---
    rc = vsh_eval(lib, 'print("hola desde VSH via FFI")\n')
    print(f"\nvesta_vsh_eval -> rc={rc}")
    assert rc == 0, f"VSH rc inesperado: {rc}"

    print("\nTODOS LOS CASOS OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
