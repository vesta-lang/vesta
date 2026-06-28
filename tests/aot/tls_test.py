#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
TLS local-exec (thread_local) en el enlazado nativo ELF.

El enlazador propio (vm --link) maqueta el bloque TLS de las secciones .tdata
(inicializadas) + .tbss (a cero) de los objetos, calcula el offset de cada
variable desde el thread pointer (TPOFF, modelo local-exec) y emite un segmento
PT_TLS; el cargador dinamico monta el TLS por-hilo antes del entry.  Sin
ld/gcc para el enlace.

Como el binario es ELF, se compila/ejecuta via WSL.  Dos escenarios:
  A) una variable __thread inicializada (.tdata) -> get_counter() = 5.
  B) .tdata + .tbss + varios tamanos -> compute() = 5 + 7 + 100 = 112.

Uso:  python tests/aot/tls_test.py <build_dir>
"""
import os
import subprocess
import sys


def wsl(cmd):
    return subprocess.run(["wsl", "bash", "-c", cmd], capture_output=True,
                          text=True)


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def main():
    if len(sys.argv) < 2:
        print("uso: tls_test.py <build_dir>")
        return 2
    build = os.path.abspath(sys.argv[1])
    vm = os.path.join(build, "vm.exe")
    if not os.path.exists(vm):
        vm = os.path.join(build, "vm")
    repo = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

    if wsl("echo ok").stdout.strip().splitlines()[-1:] != ["ok"]:
        print("TLS: WSL no disponible, omitido")
        return 0
    # WSL debe poder compilar (gcc) y ejecutar ELF64.
    if wsl("which gcc >/dev/null 2>&1 && echo yes").stdout.strip()[-3:] != "yes":
        print("TLS: gcc no disponible en WSL, omitido")
        return 0

    work = os.path.join(repo, "_tls_test")
    wm = "/mnt/" + repo[0].lower() + repo[1:].replace("\\", "/").replace(
        ":", "") + "/_tls_test"
    os.makedirs(work, exist_ok=True)
    rc = 0
    try:
        wsl(f"cp $(ls /usr/lib/x86_64-linux-gnu/libc.so.6 "
            f"/lib/x86_64-linux-gnu/libc.so.6 2>/dev/null | head -1) "
            f"{wm}/libc.so.6")
        libc = os.path.join(work, "libc.so.6")
        if not os.path.exists(libc):
            print("TLS: libc.so.6 no encontrada, omitido")
            return 0

        # --- A) .tdata simple ---
        with open(os.path.join(work, "tl.c"), "w") as f:
            f.write("__thread int counter = 5;\n"
                    "long long get_counter(void){ return counter; }\n")
        wsl(f"cd {wm} && gcc -O2 -c tl.c -o tl.o")
        with open(os.path.join(work, "ma.vex"), "w") as f:
            f.write('extern "tl" { fn get_counter() -> i64; }\n'
                    'i64 main(){ return get_counter(); }\n')
        run([vm, "--vex", os.path.join(work, "ma.vex"), "-m", "aot", "--emit",
             "obj", "--format", "elf", "-o", os.path.join(work, "ma.o")])
        run([vm, "--link", os.path.join(work, "ma.o"),
             os.path.join(work, "tl.o"), libc, "-o",
             os.path.join(work, "pa"), "--format", "elf"])
        ra = wsl(f"cd {wm} && chmod +x pa && ./pa").returncode
        if ra == 5:
            print("TLS-A (.tdata, __thread): exit=5 OK")
        else:
            print(f"TLS-A: EXIT MISMATCH got={ra} exp=5")
            rc = 1

        # --- B) .tdata + .tbss + varios tamanos ---
        with open(os.path.join(work, "tl2.c"), "w") as f:
            f.write("__thread int a = 5;\n"
                    "__thread int b;\n"
                    "__thread long long c = 100;\n"
                    "long long compute(void){ b = 7; return a + b + c; }\n")
        wsl(f"cd {wm} && gcc -O2 -c tl2.c -o tl2.o")
        with open(os.path.join(work, "mb.vex"), "w") as f:
            f.write('extern "tl2" { fn compute() -> i64; }\n'
                    'i64 main(){ return compute(); }\n')
        run([vm, "--vex", os.path.join(work, "mb.vex"), "-m", "aot", "--emit",
             "obj", "--format", "elf", "-o", os.path.join(work, "mb.o")])
        run([vm, "--link", os.path.join(work, "mb.o"),
             os.path.join(work, "tl2.o"), libc, "-o",
             os.path.join(work, "pb"), "--format", "elf"])
        rb = wsl(f"cd {wm} && chmod +x pb && ./pb").returncode
        if rb == 112:
            print("TLS-B (.tdata + .tbss + tamanos mixtos): exit=112 OK")
        else:
            print(f"TLS-B: EXIT MISMATCH got={rb} exp=112")
            rc = 1
        return rc
    finally:
        import shutil
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
