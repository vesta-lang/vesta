#!/usr/bin/env python3
"""Valgrind de los .o AOT del retorno de string por valor (SRET de 24 bytes).
Enlaza con gcc -no-pie y verifica 0 leaks / 0 errores.  Se corre bajo WSL.
Los .o + su exit-code esperado se pasan como pares "ruta:exit".

    wsl python3 tests/aot/vg_string_sret.py <a.o:42> <b.o:19> ...
"""
import os
import shutil
import subprocess
import sys

work = os.path.expanduser("~/vgwork_strsret")
os.makedirs(work, exist_ok=True)
os.chdir(work)
rc = 0

for spec in sys.argv[1:]:
    o, _, expect = spec.rpartition(":")
    base = os.path.splitext(os.path.basename(o))[0]
    print("=== %s (esperado exit=%s) ===" % (base, expect))
    try:
        shutil.copy(o, os.path.join(work, base + ".o"))
    except OSError:
        print("cp fallo"); rc = 1; continue
    if subprocess.run(["gcc", "-no-pie", "-o", base + ".elf", base + ".o"],
                      stderr=subprocess.DEVNULL).returncode != 0:
        print("gcc fallo"); rc = 1; continue
    got = subprocess.run(["./" + base + ".elf"], stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL).returncode
    print("  run exit=%d" % got)
    if str(got) != expect:
        print("  EXIT MISMATCH"); rc = 1
    vg = subprocess.run(
        ["valgrind", "--error-exitcode=99", "--leak-check=full",
         "--errors-for-leak-kinds=all", "-q", "./" + base + ".elf"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode
    if vg == 99:
        print("  VALGRIND FALLO (errores/leaks detectados)"); rc = 1
    else:
        print("  VALGRIND OK (0 leaks, 0 errores; prog exit=%d)" % vg)

sys.exit(rc)
