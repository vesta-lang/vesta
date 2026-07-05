#!/usr/bin/env python3
"""Validacion ROBUSTA del dump PE de overlays (examples 273).

Parsea un PE REAL (la propia vm.exe) con el dumper de overlays y compara su
salida estructural (DOS + File + Optional headers, los 16 data directories, la
tabla de secciones) contra un parseo INDEPENDIENTE de los mismos bytes hecho
aqui en Python (struct).  No es circular.

    python3 tests/vx/pe_parser_test.py <build_dir>
"""
import os
import re
import shutil
import struct
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

PARSER = os.path.join(ROOT, "examples_codes_vx", "273_overlay_pe_parser.vx")
TMP = tempfile.mkdtemp(prefix="vxpe_")


def sh(args, **kw):
    return subprocess.run(args, capture_output=True, text=True, **kw)


def parse_pe_independiente(path):
    with open(path, "rb") as f:
        d = f.read()
    if d[0:2] != b"MZ":
        return None
    lf = struct.unpack_from("<I", d, 0x3C)[0]
    if struct.unpack_from("<I", d, lf)[0] != 0x00004550:
        return None
    fh = lf + 4                      # IMAGE_FILE_HEADER
    oh = fh + 20                     # IMAGE_OPTIONAL_HEADER64
    opt_size = struct.unpack_from("<H", d, fh + 16)[0]
    nsec = struct.unpack_from("<H", d, fh + 2)[0]
    g = {
        "e_lfanew": lf,
        "machine": struct.unpack_from("<H", d, fh + 0)[0],
        "num_sections": nsec,
        "opt_size": opt_size,
        "opt_magic": struct.unpack_from("<H", d, oh + 0)[0],
        "entry": struct.unpack_from("<I", d, oh + 16)[0],
        "image_base": struct.unpack_from("<Q", d, oh + 24)[0],
        "sect_align": struct.unpack_from("<I", d, oh + 32)[0],
        "size_image": struct.unpack_from("<I", d, oh + 56)[0],
        "subsystem": struct.unpack_from("<H", d, oh + 68)[0],
        "num_dirs": struct.unpack_from("<I", d, oh + 108)[0],
    }
    dirs = []
    for i in range(16):
        off = oh + 112 + i * 8
        dirs.append({
            "rva": struct.unpack_from("<I", d, off + 0)[0],
            "size": struct.unpack_from("<I", d, off + 4)[0],
        })
    g["dirs"] = dirs
    tab = oh + opt_size
    secs = []
    for i in range(nsec):
        off = tab + i * 40
        secs.append({
            "vsize": struct.unpack_from("<I", d, off + 8)[0],
            "vaddr": struct.unpack_from("<I", d, off + 0x0C)[0],
            "rsize": struct.unpack_from("<I", d, off + 0x10)[0],
            "rptr": struct.unpack_from("<I", d, off + 0x14)[0],
        })
    g["secs"] = secs
    return g


def parse_salida(text):
    def gi(key):
        m = re.search(r"^%s=(?:0x)?([0-9a-fA-F]+)" % re.escape(key), text,
                      re.MULTILINE)
        if not m:
            return None
        s = m.group(0).split("=", 1)[1]
        return int(s, 16) if s.lower().startswith("0x") else int(s)
    g = {k: gi(K) for k, K in (
        ("e_lfanew", "E_LFANEW"), ("machine", "MACHINE"),
        ("num_sections", "NUM_SECTIONS"), ("opt_size", "OPT_SIZE"),
        ("opt_magic", "OPT_MAGIC"), ("entry", "ENTRY"),
        ("image_base", "IMAGE_BASE"), ("sect_align", "SECT_ALIGN"),
        ("size_image", "SIZE_IMAGE"), ("subsystem", "SUBSYSTEM"),
        ("num_dirs", "NUM_DIRS"))}
    dirs = []
    for m in re.finditer(r"DIR (\d+) RVA=(\d+) SIZE=(\d+)", text):
        dirs.append({"rva": int(m.group(2)), "size": int(m.group(3))})
    g["dirs"] = dirs
    secs = []
    for m in re.finditer(
            r"SEC \d+ \S* VSIZE=(\d+) VADDR=(\d+) RSIZE=(\d+) RPTR=(\d+)", text):
        secs.append({"vsize": int(m.group(1)), "vaddr": int(m.group(2)),
                     "rsize": int(m.group(3)), "rptr": int(m.group(4))})
    g["secs"] = secs
    return g


def comparar(ref, got):
    fails = []
    for k in ("e_lfanew", "machine", "num_sections", "opt_size", "opt_magic",
              "entry", "image_base", "sect_align", "size_image", "subsystem",
              "num_dirs"):
        if ref[k] != got.get(k):
            fails.append("%s: ref=%s parser=%s" % (k, ref[k], got.get(k)))
    for i in range(min(len(ref["dirs"]), len(got["dirs"]))):
        for k in ("rva", "size"):
            if ref["dirs"][i][k] != got["dirs"][i][k]:
                fails.append("dir %d %s: ref=%s parser=%s" %
                             (i, k, ref["dirs"][i][k], got["dirs"][i][k]))
    if len(got["secs"]) != len(ref["secs"]):
        fails.append("num secciones: ref=%d parser=%d" %
                     (len(ref["secs"]), len(got["secs"])))
    else:
        for i, (rs, gs) in enumerate(zip(ref["secs"], got["secs"])):
            for k in ("vsize", "vaddr", "rsize", "rptr"):
                if rs[k] != gs[k]:
                    fails.append("sec %d %s: ref=%s parser=%s" %
                                 (i, k, rs[k], gs[k]))
    return fails


def main():
    # Usar la propia vm.exe como PE real (si vm no es PE, saltar).
    if not VM.endswith(".exe"):
        print("SKIP: vm no es un PE en esta plataforma")
        return
    target = os.path.join(TMP, "pe_target.bin")
    shutil.copy(VM, target)
    ref = parse_pe_independiente(target)
    if ref is None:
        sys.exit("el target no es un PE valido")
    print("PE real: num_sections=%d image_base=0x%x entry=%d" %
          (ref["num_sections"], ref["image_base"], ref["entry"]))

    velb = os.path.join(TMP, "parser.velb")
    r = sh([VM, "--vesta", PARSER, "-o", os.path.join(TMP, "parser")])
    if not os.path.exists(velb):
        sys.exit("el parser no compilo\n" + r.stdout + r.stderr)

    fails_total = 0
    for mode, extra in (("interp", []), ("jit", ["-m", "jit"])):
        r = sh([VM, "--run", velb, "--schedulers", "1"] + extra, cwd=TMP)
        got = parse_salida(r.stdout + r.stderr)
        fails = comparar(ref, got)
        if fails:
            fails_total += len(fails)
            print("FAIL (%s):" % mode)
            for f in fails[:20]:
                print("  " + f)
        else:
            print("OK (%s): parser == parseo independiente "
                  "(11 campos + 16 data dirs + %d secciones)"
                  % (mode, len(ref["secs"])))

    if fails_total:
        sys.exit("=== pe_parser: %d discrepancias ===" % fails_total)
    print("=== pe_parser: dump PE de overlays validado contra PE REAL ===")


if __name__ == "__main__":
    main()
