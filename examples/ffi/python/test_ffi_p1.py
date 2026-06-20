#!/usr/bin/env python3
"""
test_ffi_p1.py -- ejemplo de las 4 funciones P1 de libvesta via ctypes.

Cubre:
    1. vesta_ir_to_velb : texto IR SSA -> bytecode .velb -> ejecucion.
    2. vesta_json_validate : valida JSON correcto e incorrecto.
    3. vesta_json_format : minifica y embellece JSON.
    4. vesta_sqlite_exec : ejecuta SQL sobre ":memory:" y obtiene JSON.

Uso:
    python test_ffi_p1.py [ruta_a_la_libreria]

Si no se pasa ruta, intenta localizar la libreria en cmake-build-release
relativo a la raiz del repo.  En Windows, las DLL de OpenSSL deben estar
junto a libvesta.dll o en el PATH.
"""

import ctypes
import os
import sys


def find_library() -> str:
    if len(sys.argv) > 1:
        return sys.argv[1]
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.abspath(os.path.join(here, "..", "..", ".."))
    build = os.path.join(repo, "cmake-build-release")
    candidates = [
        os.path.join(build, "libvesta.dll"),
        os.path.join(build, "vesta.dll"),
        os.path.join(build, "libvesta.so"),
    ]
    for c in candidates:
        if os.path.exists(c):
            return c
    raise FileNotFoundError(
        "No se encontro libvesta; pasa la ruta como argumento.\n"
        "Probadas:\n  " + "\n  ".join(candidates)
    )


def load_api(path: str) -> ctypes.CDLL:
    dll_dir = os.path.dirname(os.path.abspath(path))
    if hasattr(os, "add_dll_directory") and os.name == "nt":
        os.add_dll_directory(dll_dir)

    lib = ctypes.CDLL(path)

    lib.vesta_version.restype = ctypes.c_char_p
    lib.vesta_version.argtypes = []

    lib.vesta_compile_to_ir.restype = ctypes.c_int
    lib.vesta_compile_to_ir.argtypes = [
        ctypes.c_char_p,  # src
        ctypes.c_char_p,  # unit_name
        ctypes.POINTER(ctypes.c_char_p),  # out_ir
        ctypes.POINTER(ctypes.c_char_p),  # out_err
    ]

    lib.vesta_ir_to_velb.restype = ctypes.c_int
    lib.vesta_ir_to_velb.argtypes = [
        ctypes.c_char_p,  # ir_text
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

    lib.vesta_json_validate.restype = ctypes.c_int
    lib.vesta_json_validate.argtypes = [
        ctypes.c_char_p,  # json_text
        ctypes.POINTER(ctypes.c_char_p),  # out_err
    ]

    lib.vesta_json_format.restype = ctypes.c_int
    lib.vesta_json_format.argtypes = [
        ctypes.c_char_p,  # json_text
        ctypes.c_int,  # indent
        ctypes.POINTER(ctypes.c_char_p),  # out_text
        ctypes.POINTER(ctypes.c_char_p),  # out_err
    ]

    lib.vesta_sqlite_exec.restype = ctypes.c_int
    lib.vesta_sqlite_exec.argtypes = [
        ctypes.c_char_p,  # db_path
        ctypes.c_char_p,  # sql
        ctypes.POINTER(ctypes.c_char_p),  # out_json
        ctypes.POINTER(ctypes.c_char_p),  # out_err
    ]

    lib.vesta_free.restype = None
    lib.vesta_free.argtypes = [ctypes.c_void_p]

    return lib


def compile_to_ir(lib, src, unit) -> str:
    out_ir = ctypes.c_char_p()
    out_err = ctypes.c_char_p()
    rc = lib.vesta_compile_to_ir(
        src.encode(), unit.encode(), ctypes.byref(out_ir), ctypes.byref(out_err)
    )
    if rc != 0:
        msg = out_err.value.decode() if out_err.value else "(sin mensaje)"
        lib.vesta_free(out_err)
        raise RuntimeError(f"vesta_compile_to_ir fallo (rc={rc}): {msg}")
    ir = out_ir.value.decode() if out_ir.value else ""
    lib.vesta_free(out_ir)
    return ir


def ir_to_velb(lib, ir_text: str) -> bytes:
    out_velb = ctypes.POINTER(ctypes.c_ubyte)()
    out_len = ctypes.c_size_t(0)
    out_err = ctypes.c_char_p()
    rc = lib.vesta_ir_to_velb(
        ir_text.encode(),
        ctypes.byref(out_velb),
        ctypes.byref(out_len),
        ctypes.byref(out_err),
    )
    if rc != 0:
        msg = out_err.value.decode() if out_err.value else "(sin mensaje)"
        lib.vesta_free(out_err)
        raise RuntimeError(f"vesta_ir_to_velb fallo (rc={rc}): {msg}")
    data = bytes(out_velb[i] for i in range(out_len.value))
    lib.vesta_free(out_velb)
    return data


def run_velb(lib, velb: bytes) -> int:
    buf = (ctypes.c_ubyte * len(velb)).from_buffer_copy(velb)
    out_exit = ctypes.c_int(0)
    out_err = ctypes.c_char_p()
    rc = lib.vesta_run(
        buf,
        ctypes.c_size_t(len(velb)),
        0,
        None,
        ctypes.byref(out_exit),
        ctypes.byref(out_err),
    )
    if rc != 0:
        msg = out_err.value.decode() if out_err.value else "(sin mensaje)"
        lib.vesta_free(out_err)
        raise RuntimeError(f"vesta_run fallo (rc={rc}): {msg}")
    lib.vesta_free(out_err)
    return out_exit.value


def json_validate(lib, json_text: str) -> bool:
    out_err = ctypes.c_char_p()
    rc = lib.vesta_json_validate(json_text.encode(), ctypes.byref(out_err))
    msg = out_err.value.decode() if out_err.value else ""
    lib.vesta_free(out_err)
    return rc == 0, msg


def json_format(lib, json_text: str, indent: int) -> str:
    out_text = ctypes.c_char_p()
    out_err = ctypes.c_char_p()
    rc = lib.vesta_json_format(
        json_text.encode(), indent, ctypes.byref(out_text), ctypes.byref(out_err)
    )
    if rc != 0:
        msg = out_err.value.decode() if out_err.value else "(sin mensaje)"
        lib.vesta_free(out_err)
        raise RuntimeError(f"vesta_json_format fallo (rc={rc}): {msg}")
    text = out_text.value.decode() if out_text.value else ""
    lib.vesta_free(out_text)
    return text


def sqlite_exec(lib, db_path: str, sql: str) -> str:
    out_json = ctypes.c_char_p()
    out_err = ctypes.c_char_p()
    rc = lib.vesta_sqlite_exec(
        db_path.encode(), sql.encode(), ctypes.byref(out_json), ctypes.byref(out_err)
    )
    if rc != 0:
        msg = out_err.value.decode() if out_err.value else "(sin mensaje)"
        lib.vesta_free(out_err)
        raise RuntimeError(f"vesta_sqlite_exec fallo (rc={rc}): {msg}")
    text = out_json.value.decode() if out_json.value else ""
    lib.vesta_free(out_json)
    return text


def main() -> int:
    path = find_library()
    lib = load_api(path)
    print("libvesta version:", lib.vesta_version().decode())

    # --- Caso 1: IR -> velb -> run ---
    ir = compile_to_ir(lib, "i32 main() { return 7; }", "py_p1_ir")
    velb = ir_to_velb(lib, ir)
    code = run_velb(lib, velb)
    print(f"\n--- Caso 1: IR -> velb -> run = {code} (esperado 7)")
    assert code == 7, f"esperado 7, obtenido {code}"

    # --- Caso 2: json_validate ---
    ok, _ = json_validate(lib, '{"a": 1, "b": [2, 3], "c": null}')
    assert ok, "JSON valido rechazado"
    bad, msg = json_validate(lib, '{"a": 1, }')
    assert not bad, "JSON invalido aceptado"
    print(f"--- Caso 2: valido OK, invalido rechazado OK ({msg[:40]}...)")

    # --- Caso 3: json_format ---
    mini = json_format(lib, '{ "x" :  1 ,  "y" : [ 2 , 3 ] }', -1)
    assert mini == '{"x":1,"y":[2,3]}', f"minify inesperado: {mini}"
    pretty = json_format(lib, '{"x":1}', 2)
    print(f"--- Caso 3: minify = {mini}")
    print(f"--- Caso 3: pretty:\n{pretty}")

    # --- Caso 4: sqlite_exec ---
    sql = (
        "CREATE TABLE t(id INTEGER, name TEXT);"
        "INSERT INTO t VALUES(1,'a'),(2,'b');"
        "SELECT * FROM t;"
    )
    rows = sqlite_exec(lib, ":memory:", sql)
    expected = '[{"id":1,"name":"a"},{"id":2,"name":"b"}]'
    assert rows == expected, f"sqlite inesperado:\n  {rows}\n  esperado {expected}"
    print(f"--- Caso 4: sqlite_exec = {rows}")

    print("\nTODOS LOS CASOS P1 OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
