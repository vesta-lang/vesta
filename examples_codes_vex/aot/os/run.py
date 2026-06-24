#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
run.py -- construye y ejecuta VestaOS (kernel AOT de Vesta) en QEMU.

Portable (Windows / Linux / macOS): localiza el binario `vm`/`vm.exe` del
build y un `qemu-system-*` en el PATH (o en ubicaciones habituales), compila
el .vex a un binario plano de boot (.bin) con el AOT de Vesta y lo arranca.

Uso:
    python run.py build [target] [--build-dir DIR]
    python run.py run   [target] [--build-dir DIR]   # con ventana de QEMU
    python run.py test  [target] [--build-dir DIR]   # headless + validacion
    python run.py test  all                          # ambos targets

    target := kernel (64-bit, por defecto) | protected (32-bit)

Ejemplos:
    python run.py run               # construye y muestra el kernel en QEMU
    python run.py test all          # valida ambos OS sin ventana (CI)
"""
import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# .../examples_codes_vex/aot/os -> raiz del repo
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))

# Cada target: fuente .vex, emulador adecuado y la cadena que el kernel emite
# por el debugcon (puerto 0xE9) para validar la ejecucion headless.
TARGETS = {
    "kernel": {
        "vex": "kernel.vex",
        "qemu": "qemu-system-x86_64",
        # El kernel lanza un shell INTERACTIVO (no termina solo en headless);
        # el auto-test del arranque carga el programa 'calc' del disco y lo
        # ejecuta, emitiendo estos resultados por el debugcon.  El test valida
        # esa salida y tolera el timeout del shell (interactive=True).
        "want": ["= 14", "= 20"],
        "interactive": True,
    },
    "protected": {
        "vex": "os_protected.vex",
        "qemu": "qemu-system-i386",
        "want": "Protected mode OK!",
        "interactive": False,
    },
}

# Ubicaciones habituales de QEMU ademas del PATH.
QEMU_DIRS = [
    "/d/QEMU", "D:\\QEMU",
    "C:\\Program Files\\qemu",
    "/usr/bin", "/usr/local/bin", "/opt/homebrew/bin",
]


def find_vm(build_dir):
    """Localiza el ejecutable del compilador/VM de Vesta."""
    for name in ("vm.exe", "vm"):
        cand = os.path.join(ROOT, build_dir, name)
        if os.path.isfile(cand):
            return cand
    sys.exit("error: no se encontro 'vm'/'vm.exe' en %s (usa --build-dir)"
             % os.path.join(ROOT, build_dir))


def find_qemu(name):
    """Localiza un qemu-system-* en el PATH o en ubicaciones habituales."""
    for cand in (name, name + ".exe"):
        p = shutil.which(cand)
        if p:
            return p
    for d in QEMU_DIRS:
        for cand in (name, name + ".exe"):
            full = os.path.join(d, cand)
            if os.path.isfile(full):
                return full
    return None


def build(vm, vexfile, out):
    """Compila el .vex a un .bin de boot con el AOT de Vesta.  --no-pie:
    direcciones absolutas (la imagen se carga en una base fija), necesario
    para que `(u64) funcion` de una direccion correcta."""
    cmd = [vm, "-m", "aot", "--vex", vexfile, "--emit", "bin",
           "--bin-base", "0x7C00", "--no-pie", "-o", out]
    print("[build]", " ".join(cmd))
    subprocess.run(cmd, check=True)
    sz = os.path.getsize(out)
    print("[build] %s (%d bytes)" % (out, sz))
    return sz


def qemu_args(qemu, path, headless, debugcon, media="disk"):
    args = [qemu, "-accel", "tcg"]
    if media == "cdrom":
        args += ["-cdrom", path, "-boot", "d"]    # arrancar desde el CD (ISO)
    else:
        # Disco duro (IDE): la BIOS arranca el sector 0 (firma 0xAA55) como MBR
        # con DL=0x80.  El kernel lee el resto con int 0x13 AH=42 (LBA
        # extendido), que solo existe en HD/CD, no en floppy.
        args += ["-drive", "format=raw,file=%s,if=ide" % path]
    if headless:
        # isa-debug-exit: el kernel sale con `out 0xF4,0x42` -> exit 133.
        args += ["-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
                 "-display", "none"]
        if debugcon:
            args += ["-debugcon", "file:%s" % debugcon]
    return args


import struct

# Programas de disco del kernel: (nombre de comando, fuente .vex).
KERNEL_PROGRAMS = [
    ("calc", "prog_calc.vex"),
    ("edit", "prog_notepad.vex"),
]
_DIR_SECTOR = 96          # sector del directorio de programas
_IMG_SECTORS = 128        # tamano de la imagen (64 KiB); el boot lee 127
_SECT = 512


def build_kernel_disk(build_dir):
    """Ensambla la imagen de disco de VestaOS: kernel (sector 0..) +
    directorio de programas (sector 96) + programas (sector 97+).  El kernel
    carga la imagen completa como ramdisk y ejecuta los programas a peticion.
    """
    vm = find_vm(build_dir)
    out = os.path.join(HERE, "kernel.bin")
    # 1. Kernel (boot + drivers + shell + cargador), base 0x7C00.
    kbin = os.path.join(HERE, "_kernel_core.bin")
    build(vm, os.path.join(HERE, "kernel.vex"), kbin)
    # 2. Programas, cada uno binario plano independiente (base 0x100000).
    prog_data = []
    for name, src in KERNEL_PROGRAMS:
        pb = os.path.join(HERE, "_%s.prog" % name)
        cmd = [vm, "-m", "aot", "--vex", os.path.join(HERE, src),
               "--emit", "bin", "--bin-base", "0x100000", "--no-pie", "-o", pb]
        print("[prog]", name, "<-", src)
        subprocess.run(cmd, check=True)
        with open(pb, "rb") as f:
            prog_data.append((name, f.read()))
        os.remove(pb)
    # 3. Ensamblar la imagen.
    img = bytearray(_IMG_SECTORS * _SECT)
    with open(kbin, "rb") as f:
        kb = f.read()
    os.remove(kbin)
    if len(kb) > _DIR_SECTOR * _SECT:
        sys.exit("error: el kernel (%d B) no cabe antes del directorio (sector %d)"
                 % (len(kb), _DIR_SECTOR))
    img[0:len(kb)] = kb
    cur = _DIR_SECTOR + 1
    dir_off = _DIR_SECTOR * _SECT
    for name, data in prog_data:
        nsect = (len(data) + _SECT - 1) // _SECT
        if (cur + nsect) > _IMG_SECTORS - 1:
            sys.exit("error: los programas no caben en la imagen")
        img[cur * _SECT:cur * _SECT + len(data)] = data
        # Entrada de directorio (32 bytes): nombre[20] + sector + size + pad.
        ent = bytearray(32)
        nb = name.encode("ascii")[:19]
        ent[0:len(nb)] = nb
        ent[20:24] = struct.pack("<I", cur)
        ent[24:28] = struct.pack("<I", len(data))
        img[dir_off:dir_off + 32] = ent
        dir_off += 32
        print("[dir] %-8s sector=%d bytes=%d" % (name, cur, len(data)))
        cur += nsect
    with open(out, "wb") as f:
        f.write(img)
    print("[disk] %s (%d KiB, %d programa(s))"
          % (out, _IMG_SECTORS * _SECT // 1024, len(prog_data)))
    return out


def do_build(target, build_dir):
    if target == "kernel":
        return build_kernel_disk(build_dir)
    vm = find_vm(build_dir)
    t = TARGETS[target]
    out = os.path.join(HERE, "%s.bin" % target)
    build(vm, os.path.join(HERE, t["vex"]), out)
    return out


# --- Generacion de ISO booteable (ISO9660 + El Torito, no-emulacion) --------
import struct

_ISO = 2048  # tamano de sector logico ISO9660


def _both16(v):
    return struct.pack("<H", v) + struct.pack(">H", v)


def _both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def _dir_record(lba, length, is_dir, ident):
    """Registro de directorio ISO9660 (para '.', '..' y la raiz en el PVD)."""
    rec = bytearray()
    rec += b"\x00"                       # 0: longitud (se rellena al final)
    rec += b"\x00"                       # 1: ext attr len
    rec += _both32(lba)                  # 2: extent LBA
    rec += _both32(length)               # 10: data length
    rec += bytes(7)                      # 18: fecha (7 bytes, 0)
    rec += bytes([0x02 if is_dir else 0x00])  # 25: flags (0x02=dir)
    rec += b"\x00\x00"                   # 26: unit size + interleave
    rec += _both16(1)                    # 28: volume sequence number
    rec += bytes([len(ident)])           # 32: long del identificador
    rec += ident                          # 33: identificador
    if len(rec) % 2:                      # padding a par
        rec += b"\x00"
    rec[0] = len(rec)
    return bytes(rec)


def make_iso(bin_path, iso_path):
    """Crea un ISO booteable (BIOS El Torito no-emulacion) con el .bin como
    imagen de arranque.  Sin dependencias externas (no necesita xorriso)."""
    with open(bin_path, "rb") as f:
        boot = f.read()
    n_boot_512 = (len(boot) + 511) // 512          # sectores virtuales (512 B)
    pad = (_ISO - (len(boot) % _ISO)) % _ISO
    boot_img = boot + b"\x00" * pad
    n_img = len(boot_img) // _ISO

    LBA_PVD, LBA_BREC, LBA_TERM = 16, 17, 18
    LBA_PATHL, LBA_PATHM, LBA_ROOT, LBA_CAT = 19, 20, 21, 22
    LBA_IMG = 23
    total = LBA_IMG + n_img
    img = bytearray(total * _ISO)

    def put(lba, data):
        img[lba * _ISO:lba * _ISO + len(data)] = data

    # --- Directorio raiz (LBA_ROOT): entradas '.' y '..' ---
    root = bytearray()
    root += _dir_record(LBA_ROOT, _ISO, True, b"\x00")   # '.'
    root += _dir_record(LBA_ROOT, _ISO, True, b"\x01")   # '..'
    put(LBA_ROOT, bytes(root))

    # --- Tablas de path (L y M): solo la raiz ---
    # entrada: len_id(1) + ext_attr(1) + extent(4) + parent(2) + id
    ptl = bytes([1, 0]) + struct.pack("<I", LBA_ROOT) + struct.pack("<H", 1) + b"\x00"
    ptm = bytes([1, 0]) + struct.pack(">I", LBA_ROOT) + struct.pack(">H", 1) + b"\x00"
    put(LBA_PATHL, ptl)
    put(LBA_PATHM, ptm)

    # --- PVD (LBA 16) ---
    pvd = bytearray(_ISO)
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[6] = 1
    pvd[8:40] = b" " * 32                              # system id
    vid = b"VESTAOS"
    pvd[40:72] = vid + b" " * (32 - len(vid))           # volume id
    pvd[80:88] = _both32(total)                         # volume space size
    pvd[120:124] = _both16(1)                          # volume set size
    pvd[124:128] = _both16(1)                          # volume seq number
    pvd[128:132] = _both16(_ISO)                       # logical block size
    pvd[132:140] = _both32(len(ptl))                   # path table size
    pvd[140:144] = struct.pack("<I", LBA_PATHL)        # type-L path table
    pvd[148:152] = struct.pack(">I", LBA_PATHM)        # type-M path table
    pvd[156:156 + 34] = _dir_record(LBA_ROOT, _ISO, True, b"\x00")  # raiz
    pvd[881] = 1                                        # file structure version
    put(LBA_PVD, bytes(pvd))

    # --- Boot Record Volume Descriptor (LBA 17) ---
    brec = bytearray(_ISO)
    brec[0] = 0
    brec[1:6] = b"CD001"
    brec[6] = 1
    eltorito = b"EL TORITO SPECIFICATION"
    brec[7:7 + len(eltorito)] = eltorito
    brec[71:75] = struct.pack("<I", LBA_CAT)           # ptr al boot catalog
    put(LBA_BREC, bytes(brec))

    # --- Volume Descriptor Set Terminator (LBA 18) ---
    term = bytearray(_ISO)
    term[0] = 0xFF
    term[1:6] = b"CD001"
    term[6] = 1
    put(LBA_TERM, bytes(term))

    # --- Boot Catalog (LBA 22): validation entry + default entry ---
    cat = bytearray(_ISO)
    val = bytearray(32)
    val[0] = 1                  # header id
    val[1] = 0                  # plataforma 80x86
    val[30] = 0x55
    val[31] = 0xAA
    # checksum: suma de words 16-bit LE == 0 (mod 0x10000)
    s = 0
    for i in range(0, 32, 2):
        s += val[i] | (val[i + 1] << 8)
    chk = (-s) & 0xFFFF
    val[28] = chk & 0xFF
    val[29] = (chk >> 8) & 0xFF
    cat[0:32] = val
    ent = bytearray(32)
    ent[0] = 0x88              # bootable
    ent[1] = 0                 # no-emulacion
    ent[2:4] = struct.pack("<H", 0)        # load segment 0 -> 0x7C00
    ent[6:8] = struct.pack("<H", n_boot_512)   # sectores virtuales a cargar
    ent[8:12] = struct.pack("<I", LBA_IMG)     # LBA de la imagen de arranque
    cat[32:64] = ent
    put(LBA_CAT, bytes(cat))

    # --- Imagen de arranque ---
    put(LBA_IMG, boot_img)

    with open(iso_path, "wb") as f:
        f.write(img)
    print("[iso] %s (%d sectores ISO, boot=%d sectores 512B)"
          % (iso_path, total, n_boot_512))
    return iso_path


def do_iso(target, build_dir):
    out = do_build(target, build_dir)
    iso = os.path.join(HERE, "%s.iso" % target)
    return make_iso(out, iso)


def do_run(target, build_dir, use_iso=False):
    media = "cdrom" if use_iso else "floppy"
    boot = do_iso(target, build_dir) if use_iso else do_build(target, build_dir)
    qemu = find_qemu(TARGETS[target]["qemu"])
    if not qemu:
        sys.exit("error: %s no encontrado (instala QEMU)" % TARGETS[target]["qemu"])
    # Modo visual: SIN isa-debug-exit para que el `out 0xF4` del kernel no
    # cierre QEMU; la pantalla VGA queda visible (cierra la ventana a mano).
    args = qemu_args(qemu, boot, headless=False, debugcon=None, media=media)
    print("[run]", " ".join(args))
    return subprocess.run(args).returncode


def do_test(target, build_dir, use_iso=False):
    media = "cdrom" if use_iso else "floppy"
    boot = do_iso(target, build_dir) if use_iso else do_build(target, build_dir)
    t = TARGETS[target]
    qemu = find_qemu(t["qemu"])
    if not qemu:
        print("[test] SKIP %s (%s no encontrado)" % (target, t["qemu"]))
        return True
    dbg = os.path.join(HERE, "%s.debugcon" % target)
    if os.path.exists(dbg):
        os.remove(dbg)
    args = qemu_args(qemu, boot, headless=True, debugcon=dbg, media=media)
    interactive = t.get("interactive", False)
    rc = None
    try:
        rc = subprocess.run(args, timeout=18 if interactive else 40).returncode
    except subprocess.TimeoutExpired:
        if not interactive:
            print("[test] FALLO %s: QEMU no termino (timeout)" % target)
            return False
        # El shell interactivo no termina solo: el timeout es esperado; se
        # valida solo por la salida del debugcon.
    got = ""
    if os.path.exists(dbg):
        with open(dbg, "rb") as f:
            got = f.read().replace(b"\x00", b"").decode("latin-1")
    wants = t["want"] if isinstance(t["want"], list) else [t["want"]]
    ok = all(w in got for w in wants)
    if not interactive:
        ok = ok and (rc == 133)
    status = "OK" if ok else "FALLO"
    print("[test] %s %s: qemu_exit=%s debugcon=%r (esperaba %r)"
          % (status, target, rc, got.strip(), wants))
    return ok


def main():
    ap = argparse.ArgumentParser(description="Construye/ejecuta VestaOS en QEMU.")
    ap.add_argument("cmd", choices=["build", "run", "test", "iso"])
    ap.add_argument("target", nargs="?", default="kernel",
                    help="kernel | protected | all (default: kernel)")
    ap.add_argument("--build-dir", default="cmake-build-release",
                    help="directorio del build con vm/vm.exe")
    ap.add_argument("--iso", action="store_true",
                    help="run/test: arrancar desde una ISO (CD) en vez de floppy")
    a = ap.parse_args()

    targets = list(TARGETS.keys()) if a.target == "all" else [a.target]
    for tg in targets:
        if tg not in TARGETS:
            sys.exit("target invalido: %s (kernel|protected|all)" % tg)

    if a.cmd == "build":
        for tg in targets:
            if a.iso:
                do_iso(tg, a.build_dir)    # build --iso: tambien genera la ISO
            else:
                do_build(tg, a.build_dir)
    elif a.cmd == "iso":
        for tg in targets:
            do_iso(tg, a.build_dir)
    elif a.cmd == "run":
        return do_run(targets[0], a.build_dir, use_iso=a.iso)
    elif a.cmd == "test":
        ok = True
        for tg in targets:
            ok = do_test(tg, a.build_dir, use_iso=a.iso) and ok
        print("=== %s ===" % ("TODO OK" if ok else "FALLOS"))
        return 0 if ok else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
