#!/usr/bin/env python3
"""Valgrind de los .o AOT (rep movsb): enlaza con gcc -no-pie y verifica
0 leaks / 0 errores.  Se corre bajo Linux/WSL.  Los .o se pasan como args.

    wsl python3 tests/aot/vg_repmovsb.py <a.o> <b.o> ...
"""
import os
import shutil
import subprocess
import sys

work = os.path.expanduser("~/vgwork_repmovsb")
os.makedirs(work, exist_ok=True)
os.chdir(work)
rc = 0

for o in sys.argv[1:]:
    base = os.path.splitext(os.path.basename(o))[0]
    print("=== %s ===" % base)
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
    # --error-exitcode=99: SOLO retorna 99 si hubo errores/leaks; en limpio
    # propaga el exit-code del programa.  Por eso comparamos == 99 explicitamente.
    vg = subprocess.run(
        ["valgrind", "--error-exitcode=99", "--leak-check=full",
         "--errors-for-leak-kinds=all", "-q", "./" + base + ".elf"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode
    if vg == 99:
        print("  VALGRIND FALLO (errores/leaks detectados)"); rc = 1
    else:
        print("  VALGRIND OK (0 leaks, 0 errores; prog exit=%d)" % vg)

sys.exit(rc)
