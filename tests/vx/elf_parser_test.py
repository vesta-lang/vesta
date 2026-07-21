#!/usr/bin/env python3
"""Validacion ROBUSTA del parser ELF64 de overlays (examples 272).

No es circular: genera un ELF64 REAL (compilando un programa trivial a AOT),
corre el parser de overlays sobre ese fichero, y compara su salida contra un
parseo INDEPENDIENTE de los mismos bytes hecho aqui en Python (struct).  Si el
parser de overlays leyera mal cualquier campo (offset/stride/endian/ancho), la
comparacion falla.

    python3 tests/vx/elf_parser_test.py <build_dir>

Convencion del proyecto: los scripts de automatizacion van en Python.
"""
import os
import re
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

PARSER = os.path.join(ROOT, "examples_codes_vx", "272_overlay_elf_parser.vx")
TMP = tempfile.mkdtemp(prefix="vxelf_")


def sh(args, **kw):
    return subprocess.run(args, capture_output=True, text=True, **kw)


def parse_elf_independiente(path):
    """Verdad de referencia: parseo directo de los bytes ELF64 con struct."""
    with open(path, "rb") as f:
        d = f.read()
    g = {
        "magic": struct.unpack_from("<I", d, 0)[0],
        "ei_class": d[4],
        "ei_data": d[5],
        "e_type": struct.unpack_from("<H", d, 0x10)[0],
        "e_machine": struct.unpack_from("<H", d, 0x12)[0],
        "e_version": struct.unpack_from("<I", d, 0x14)[0],
        "e_entry": struct.unpack_from("<Q", d, 0x18)[0],
        "e_phoff": struct.unpack_from("<Q", d, 0x20)[0],
        "e_shoff": struct.unpack_from("<Q", d, 0x28)[0],
        "e_ehsize": struct.unpack_from("<H", d, 0x34)[0],
        "e_phentsize": struct.unpack_from("<H", d, 0x36)[0],
        "e_phnum": struct.unpack_from("<H", d, 0x38)[0],
        "e_shentsize": struct.unpack_from("<H", d, 0x3A)[0],
        "e_shnum": struct.unpack_from("<H", d, 0x3C)[0],
        "e_shstrndx": struct.unpack_from("<H", d, 0x3E)[0],
    }
    segs = []
    for i in range(g["e_phnum"]):
        off = g["e_phoff"] + i * g["e_phentsize"]
        segs.append({
            "type": struct.unpack_from("<I", d, off + 0)[0],
            "offset": struct.unpack_from("<Q", d, off + 8)[0],
            "vaddr": struct.unpack_from("<Q", d, off + 0x10)[0],
            "filesz": struct.unpack_from("<Q", d, off + 0x20)[0],
            "memsz": struct.unpack_from("<Q", d, off + 0x28)[0],
        })
    g["segs"] = segs
    secs = []
    for i in range(g["e_shnum"]):
        off = g["e_shoff"] + i * g["e_shentsize"]
        secs.append({
            "type": struct.unpack_from("<I", d, off + 4)[0],
            "addr": struct.unpack_from("<Q", d, off + 0x10)[0],
            "offset": struct.unpack_from("<Q", d, off + 0x18)[0],
            "size": struct.unpack_from("<Q", d, off + 0x20)[0],
            "nameoff": struct.unpack_from("<I", d, off + 0)[0],
        })
    g["secs"] = secs
    return g


def parse_salida_parser(text):
    """Parsea la salida legible por maquina del parser de overlays."""
    def geti(key):
        m = re.search(r"%s=(?:0x)?([0-9a-fA-F]+)" % re.escape(key), text)
        if not m:
            return None
        s = m.group(0).split("=", 1)[1]
        return int(s, 16) if s.lower().startswith("0x") else int(s)
    g = {
        "magic": geti("MAGIC"),
        "ei_class": geti("EI_CLASS"),
        "ei_data": geti("EI_DATA"),
        "e_type": geti("E_TYPE"),
        "e_machine": geti("E_MACHINE"),
        "e_version": geti("E_VERSION"),
        "e_entry": geti("E_ENTRY"),
        "e_phoff": geti("E_PHOFF"),
        "e_shoff": geti("E_SHOFF"),
        "e_ehsize": geti("E_EHSIZE"),
        "e_phentsize": geti("E_PHENTSIZE"),
        "e_phnum": geti("E_PHNUM"),
        "e_shentsize": geti("E_SHENTSIZE"),
        "e_shnum": geti("E_SHNUM"),
        "e_shstrndx": geti("E_SHSTRNDX"),
    }
    segs = []
    for m in re.finditer(
            r"PHDR (\d+) TYPE=(\d+) FLAGS=(\d+) OFFSET=(\d+) VADDR=(\d+) "
            r"FILESZ=(\d+) MEMSZ=(\d+) ALIGN=(\d+)", text):
        segs.append({
            "type": int(m.group(2)),
            "offset": int(m.group(4)),
            "vaddr": int(m.group(5)),
            "filesz": int(m.group(6)),
            "memsz": int(m.group(7)),
        })
    g["segs"] = segs
    secs = []
    for m in re.finditer(
            r"SHDR (\d+) TYPE=(\d+) ADDR=(\d+) OFFSET=(\d+) SIZE=(\d+) "
            r"NAMEOFF=(\d+)", text):
        secs.append({
            "type": int(m.group(2)),
            "addr": int(m.group(3)),
            "offset": int(m.group(4)),
            "size": int(m.group(5)),
            "nameoff": int(m.group(6)),
        })
    g["secs"] = secs
    return g


def comparar(ref, got):
    fails = []
    for k in ("magic", "ei_class", "ei_data", "e_type", "e_machine",
              "e_version", "e_entry", "e_phoff", "e_shoff", "e_ehsize",
              "e_phentsize", "e_phnum", "e_shentsize", "e_shnum", "e_shstrndx"):
        if ref[k] != got.get(k):
            fails.append("%s: ref=%s parser=%s" % (k, ref[k], got.get(k)))
    if len(got["segs"]) != len(ref["segs"]):
        fails.append("num program headers: ref=%d parser=%d" %
                     (len(ref["segs"]), len(got["segs"])))
    else:
        for i, (rs, gs) in enumerate(zip(ref["segs"], got["segs"])):
            for k in ("type", "offset", "vaddr", "filesz", "memsz"):
                if rs[k] != gs[k]:
                    fails.append("phdr %d %s: ref=%s parser=%s" %
                                 (i, k, rs[k], gs[k]))
    if len(got["secs"]) != len(ref["secs"]):
        fails.append("num section headers: ref=%d parser=%d" %
                     (len(ref["secs"]), len(got["secs"])))
    else:
        for i, (rs, gs) in enumerate(zip(ref["secs"], got["secs"])):
            for k in ("type", "addr", "offset", "size", "nameoff"):
                if rs[k] != gs[k]:
                    fails.append("shdr %d %s: ref=%s parser=%s" %
                                 (i, k, rs[k], gs[k]))
    return fails


def main():
    # 1. Generar un ELF64 REAL (AOT de un programa trivial).
    tiny = os.path.join(TMP, "tiny.vx")
    with open(tiny, "w") as f:
        f.write("i32 main() { return 7; }\n")
    target = os.path.join(TMP, "elf_target.bin")
    r = sh([VM, "--vesta", tiny, "-m", "aot", "--format", "elf", "--emit",
            "exe", "-o", target])
    if not os.path.exists(target):
        sys.exit("no se genero el ELF de prueba\n" + r.stdout + r.stderr)

    ref = parse_elf_independiente(target)
    print("ELF real: e_phnum=%d entry=0x%x machine=%d" %
          (ref["e_phnum"], ref["e_entry"], ref["e_machine"]))

    # 2. Compilar el parser de overlays.
    velb = os.path.join(TMP, "parser.velb")
    r = sh([VM, "--vesta", PARSER, "-o", os.path.join(TMP, "parser")])
    if not os.path.exists(velb):
        sys.exit("el parser no compilo\n" + r.stdout + r.stderr)

    # 3. Correr el parser (cwd = TMP, donde vive elf_target.bin) en interp y jit,
    #    y comparar contra el parseo independiente.
    fails_total = 0
    for mode, extra in (("interp", []), ("jit", ["-m", "jit"])):
        r = sh([VM, "--run", velb, "--schedulers", "1"] + extra, cwd=TMP)
        got = parse_salida_parser(r.stdout + r.stderr)
        fails = comparar(ref, got)
        if fails:
            fails_total += len(fails)
            print("FAIL (%s):" % mode)
            for f in fails:
                print("  " + f)
        else:
            print("OK (%s): parser == parseo independiente "
                  "(15 campos cabecera + %d program headers + %d section headers)"
                  % (mode, len(ref["segs"]), len(ref["secs"])))

    if fails_total:
        sys.exit("=== elf_parser: %d discrepancias ===" % fails_total)
    print("=== elf_parser: parser ELF64 de overlays validado contra ELF REAL ===")


if __name__ == "__main__":
    main()
