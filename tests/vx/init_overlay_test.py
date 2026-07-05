#!/usr/bin/env python3
"""Regresion de inicializacion de structs/clases + overlay structs (interp).

Compila cada ejemplo de examples_codes_vx con `--vesta`, lo ejecuta en el
interprete con `--stats` y verifica el valor de R00.  Los ejemplos hacen de
arnes: si una feature (defaults de campo, `= {}`, `default()`, compound
literals, `new` auto-init, contratos de tipo, overlay structs) regresiona, el
R00 cambia y el test falla.

    python3 tests/vx/init_overlay_test.py <build_dir>

Convencion del proyecto: los scripts de automatizacion van en Python (no .sh).
"""
import os
import re
import subprocess
import sys
import tempfile

if len(sys.argv) < 2:
    sys.exit("uso: python %s <build_dir>" % os.path.basename(sys.argv[0]))

BUILD = sys.argv[1]
ROOT = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(sys.argv[0])), "..", ".."))
VM = os.path.join(BUILD, "vm.exe")
if not os.path.exists(VM):
    VM = os.path.join(BUILD, "vm")
if not os.path.exists(VM):
    sys.exit("no encuentro vm(.exe) en %s" % BUILD)

TMP = tempfile.mkdtemp(prefix="vxinit_")

# (ruta del .vx relativa a la raiz, R00 esperado).  El valor de retorno de cada
# ejemplo esta calculado para ejercitar la feature completa.
CASES = [
    ("examples_codes_vx/258_struct_defaults.vx", 155),
    ("examples_codes_vx/259_compound_literals.vx", 119),
    ("examples_codes_vx/260_class_defaults.vx", 159),
    ("examples_codes_vx/analyze/type_contracts.vx", 140),
    ("examples_codes_vx/261_overlay_basics.vx", 23629),
    ("examples_codes_vx/262_overlay_dynamic.vx", 128),
    ("examples_codes_vx/263_overlay_block.vx", 1337),
    ("examples_codes_vx/264_overlay_block_if.vx", 666),
    ("examples_codes_vx/265_overlay_scan.vx", 140),
    ("examples_codes_vx/266_match_int.vx", 9334),
    ("examples_codes_vx/267_match_string.vx", 54231),
    ("examples_codes_vx/268_match_string_runtime.vx", 321),
    ("examples_codes_vx/269_match_range.vx", 1321),
    ("examples_codes_vx/270_overlay_array.vx", 660),
    ("examples_codes_vx/271_overlay_pe_sections.vx", 39936),
]


def run_r0(src, want):
    out = os.path.join(TMP, os.path.basename(src)[:-3])
    r = subprocess.run([VM, "--vesta", os.path.join(ROOT, src), "-o", out],
                       capture_output=True, text=True)
    velb = out + ".velb"
    if not os.path.exists(velb):
        print("FAIL: %s no compilo\n%s" % (src, r.stdout + r.stderr))
        return False
    r = subprocess.run([VM, "--run", velb, "--schedulers", "1", "--stats"],
                       capture_output=True, text=True)
    m = re.search(r"R00=0x([0-9a-fA-F]+)", r.stdout + r.stderr)
    got = int(m.group(1), 16) if m else None
    if got != want:
        print("FAIL: %s: R00 == %s, se esperaba %d" % (src, got, want))
        return False
    print("OK: %s -> R0 = %d" % (src, want))
    return True


def main():
    fails = sum(0 if run_r0(src, want) else 1 for src, want in CASES)
    if fails:
        sys.exit("=== init/overlay: %d fallidos ===" % fails)
    print("=== init/overlay: %d OK, 0 fallidos ===" % len(CASES))


if __name__ == "__main__":
    main()
