#!/usr/bin/env python3
"""Regresion del layout multi-seccion del ensamblador/linker .velb.

El formato .velb es una imagen PLANA: el loader deriva el offset de fichero de
una seccion a partir de su direccion virtual con

    file_offset = space->file_offset + (address_init - space->range.address_init)

Por tanto cada seccion debe ocupar un tramo CONTIGUO del flujo de bytecode y
cumplir  address_init == base_del_espacio + offset_en_el_flujo.

Historicamente el ensamblador daba a TODAS las secciones el rango [0, 0), con lo
que las direcciones de las labels acababan siendo su offset GLOBAL en el flujo:
con una sola seccion cuadraba por casualidad (base 0 + offset global == VA), pero
con dos o mas, la segunda seccion se solapaba con la primera y leer un dato
devolvia bytes del codigo.

Este test fija el layout: alineacion por seccion, direcciones relativas a la
seccion, tamano real por seccion y el caso de una sola seccion (el que emite el
frontend Vesta, y por tanto la regresion mas peligrosa).

    python3 tests/vx/multi_section_test.py <build_dir>

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
VM = os.path.join(BUILD, "vm.exe")
if not os.path.exists(VM):
    VM = os.path.join(BUILD, "vm")
if not os.path.exists(VM):
    sys.exit("no encuentro vm(.exe) en %s" % BUILD)

TMP = tempfile.mkdtemp(prefix="vxsec_")

# Preambulo comun: un unico espacio de direcciones con base 0.
HEAD = ('@Format("velb")\n'
        '@SpaceAddress { @Name("anonymous"), @IniAddress(0x0000000000000000),'
        ' @EndAddress(0xFFFFFFFFFFFFFFFF) }\n')


def section(name, align):
    return ('@Section { @Name("%s"), @SpaceAddress("anonymous")'
            ' @Align(0x%x) }\n' % (name, align))


def build(name, src, emit_map=True):
    """Ensambla `src`.  Devuelve (ok, salida, ruta_velb, ruta_map)."""
    vel = os.path.join(TMP, name + ".vel")
    with open(vel, "w") as f:
        f.write(src)
    out = os.path.join(TMP, name)
    cmd = [VM, "--worker", vel, "-o", out]
    if emit_map:
        cmd.append("--emit-map")
    r = subprocess.run(cmd, capture_output=True, text=True)
    velb = out + ".velb"
    return (os.path.exists(velb), r.stdout + r.stderr, velb, velb + "-map")


def run_r0(velb):
    r = subprocess.run([VM, "--run", velb, "--schedulers", "1", "--stats"],
                       capture_output=True, text=True)
    m = re.search(r"R00=0x([0-9a-fA-F]+)", r.stdout + r.stderr)
    return int(m.group(1), 16) if m else None


def section_ranges(map_path):
    """Extrae {nombre: (va_init, va_final)} del bloque === SECTIONS === del mapa."""
    out = {}
    if not os.path.exists(map_path):
        return out
    with open(map_path) as f:
        txt = f.read()
    body = txt.split("=== SECTIONS ===", 1)
    if len(body) < 2:
        return out
    cur = None
    for line in body[1].splitlines():
        m = re.match(r"^\[([^\]]+)\]", line)
        if m:
            cur = m.group(1)
            continue
        m = re.search(r"VA Range:\s*0x([0-9a-fA-F]+)\s*-\s*0x([0-9a-fA-F]+)",
                      line)
        if m and cur:
            out[cur] = (int(m.group(1), 16), int(m.group(2), 16))
            cur = None
    return out


def sym_file_offsets(map_path):
    """Extrae {simbolo: (VA, FILE)} del bloque de simbolos del mapa."""
    out = {}
    with open(map_path) as f:
        txt = f.read()
    for m in re.finditer(
            r"^  ([\w.]+)\n\s*VA:\s*0x([0-9a-fA-F]+)\n\s*FILE:\s*0x([0-9a-fA-F]+)",
            txt, re.M):
        out[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16))
    return out


FAILS = []


def check(cond, msg):
    if not cond:
        print("FAIL: " + msg)
        FAILS.append(msg)
    return cond


# ---------------------------------------------------------------------------
# 1. Dos secciones: leer un dato de una seccion secundaria.
#    Pre-fix: gdata nacia en la VA 0x13 (su offset GLOBAL en el flujo), solapada
#    con code, y R00 devolvia bytes del codigo (0x0000030000000000).
# ---------------------------------------------------------------------------
def t_dos_secciones():
    src = (HEAD + section("code", 0x1000) + "@Module(m2)\n@Export(main)\n"
           "main:\n"
           '    mov r1, @Absolute("gdata.g0")\n'
           "    mov r0, [r1]\n"
           "    hlt\n" + section("gdata", 0x1000) + "g0 dq 0x2a\n")
    ok, out, velb, mp = build("dos", src)
    if not check(ok, "dos secciones: no ensamblo\n" + out):
        return
    check(run_r0(velb) == 0x2a, "dos secciones: R00 != 0x2a (lee el dato real)")

    r = section_ranges(mp)
    check(r.get("code", (None,))[0] == 0x0, "dos secciones: code no empieza en 0")
    # gdata va detras de code (0x13) alineada a 0x1000
    check(r.get("gdata", (None,))[0] == 0x1000,
          "dos secciones: gdata deberia estar alineada en 0x1000, esta en %s" %
          (hex(r["gdata"][0]) if "gdata" in r else "?"))
    # tamano real por seccion: code son instrucciones (pre-fix quedaba a 0)
    check(r.get("code", (0, 0))[1] - r.get("code", (0, 0))[0] > 0,
          "dos secciones: code tiene tamano 0 (size_real no cuenta el codigo)")
    check(r.get("gdata", (0, 0))[1] - r.get("gdata", (0, 0))[0] == 8,
          "dos secciones: gdata deberia medir 8 bytes")

    # imagen plana: FILE == VA - base_espacio (base 0) para cada simbolo
    s = sym_file_offsets(mp)
    for name, (va, fo) in s.items():
        check(va == fo, "dos secciones: %s VA(0x%x) != FILE(0x%x); la imagen "
                        "debe ser plana" % (name, va, fo))
    print("OK: dos secciones -> R0 = 0x2a, gdata @ 0x1000")


# ---------------------------------------------------------------------------
# 2. Tres secciones con alineaciones distintas.  gdata2 usa @Align(0x10), asi
#    que debe caer justo detras de gdata (0x1008 -> 0x1010), no en otra pagina.
# ---------------------------------------------------------------------------
def t_tres_secciones():
    src = (HEAD + section("code", 0x1000) + "@Module(m3)\n@Export(main)\n"
           "main:\n"
           '    mov r1, @Absolute("gdata.g0")\n'
           "    mov r0, [r1]\n"
           '    mov r2, @Absolute("gdata2.g1")\n'
           "    mov r3, [r2]\n"
           "    adds r0, r3\n"
           "    hlt\n" + section("gdata", 0x1000) + "g0 dq 0x2a\n" +
           section("gdata2", 0x10) + "g1 dq 0x11\n")
    ok, out, velb, mp = build("tres", src)
    if not check(ok, "tres secciones: no ensamblo\n" + out):
        return
    check(run_r0(velb) == 0x3b,
          "tres secciones: R00 != 0x3b (0x2a + 0x11: ambas secciones bien "
          "ubicadas)")
    r = section_ranges(mp)
    check(r.get("gdata", (None,))[0] == 0x1000, "tres secciones: gdata != 0x1000")
    check(r.get("gdata2", (None,))[0] == 0x1010,
          "tres secciones: gdata2 con @Align(0x10) deberia caer en 0x1010")
    print("OK: tres secciones -> R0 = 0x3b, gdata @ 0x1000, gdata2 @ 0x1010")


# ---------------------------------------------------------------------------
# 3. Seccion vacia entre medias: no debe romper el layout ni el solapamiento.
# ---------------------------------------------------------------------------
def t_seccion_vacia():
    src = (HEAD + section("code", 0x1000) + "@Module(mv)\n@Export(main)\n"
           "main:\n"
           '    mov r1, @Absolute("gdata.g0")\n'
           "    mov r0, [r1]\n"
           "    hlt\n" + section("vacia", 0x100) + section("gdata", 0x1000) +
           "g0 dq 0x55\n")
    ok, out, velb, mp = build("vacia", src)
    if not check(ok, "seccion vacia: no ensamblo\n" + out):
        return
    check(run_r0(velb) == 0x55, "seccion vacia: R00 != 0x55")
    r = section_ranges(mp)
    check(r.get("vacia", (0, 1))[0] == r.get("vacia", (0, 1))[1],
          "seccion vacia: deberia tener tamano 0")
    print("OK: seccion vacia -> R0 = 0x55")


# ---------------------------------------------------------------------------
# 4. Label de datos que cruza la alineacion: la seccion siguiente debe
#    reservarle todos sus bytes (el tamano real, no solo el ultimo elemento).
# ---------------------------------------------------------------------------
def t_label_grande():
    big = ", ".join("0x%02x" % (i & 0xFF) for i in range(600))  # > 0x200 bytes
    src = (HEAD + section("code", 0x1000) + "@Module(mg)\n@Export(main)\n"
           "main:\n"
           '    mov r1, @Absolute("tail.t0")\n'
           "    mov r0, [r1]\n"
           "    hlt\n" + section("blob", 0x10) + "b0 db " + big + "\n" +
           section("tail", 0x100) + "t0 dq 0x99\n")
    ok, out, velb, mp = build("grande", src)
    if not check(ok, "label grande: no ensamblo\n" + out):
        return
    check(run_r0(velb) == 0x99,
          "label grande: R00 != 0x99 (tail pisada por el blob?)")
    r = section_ranges(mp)
    if "blob" in r and "tail" in r:
        check(r["blob"][1] - r["blob"][0] == 600,
              "label grande: blob deberia medir 600 bytes, mide %d" %
              (r["blob"][1] - r["blob"][0]))
        check(r["tail"][0] >= r["blob"][1],
              "label grande: tail (0x%x) se solapa con el final de blob (0x%x)" %
              (r["tail"][0], r["blob"][1]))
        check(r["tail"][0] % 0x100 == 0,
              "label grande: tail no respeta @Align(0x100)")
    print("OK: label grande -> R0 = 0x99, blob = 600 bytes, tail alineada")


# ---------------------------------------------------------------------------
# 5. Re-entrar en una seccion ya declarada: los bytes de una seccion han de ser
#    contiguos, asi que debe fallar con un mensaje claro (antes sobrescribia la
#    seccion en silencio y perdia sus labels).
# ---------------------------------------------------------------------------
def t_reentrada():
    src = (HEAD + section("code", 0x1000) + "@Module(mr)\n@Export(main)\n"
           "main:\n"
           "    hlt\n" + section("gdata", 0x1000) + "g0 dq 0x2a\n" +
           section("code", 0x1000) + "otro:\n    hlt\n")
    ok, out, velb, mp = build("reentrada", src, emit_map=False)
    if check(not ok, "re-entrada en seccion: deberia fallar el ensamblado"):
        check("no se puede re-entrar" in out or "ya fue declarada" in out,
              "re-entrada: el mensaje de error no es claro:\n" + out)
        print("OK: re-entrada en seccion -> error claro")


# ---------------------------------------------------------------------------
# 6. Una sola seccion (lo que emite el frontend Vesta): debe seguir funcionando
#    y sin relleno, ya que la primera seccion arranca en el offset 0.
# ---------------------------------------------------------------------------
def t_una_seccion():
    src = (HEAD + section("code", 0x1000) + "@Module(m1)\n@Export(main)\n"
           "main:\n"
           '    mov r1, @Absolute("code.g0")\n'
           "    mov r0, [r1]\n"
           "    hlt\n"
           "g0 dq 0x7\n")
    ok, out, velb, mp = build("una", src)
    if not check(ok, "una seccion: no ensamblo\n" + out):
        return
    check(run_r0(velb) == 0x7, "una seccion: R00 != 0x7")
    r = section_ranges(mp)
    check(r.get("code", (None,))[0] == 0x0,
          "una seccion: code debe empezar en 0 (sin relleno inicial)")
    print("OK: una seccion -> R0 = 0x7 (caso del frontend Vesta)")


def main():
    for t in (t_dos_secciones, t_tres_secciones, t_seccion_vacia,
              t_label_grande, t_reentrada, t_una_seccion):
        t()
    if FAILS:
        sys.exit("=== multi-seccion: %d fallidos ===" % len(FAILS))
    print("=== multi-seccion: todos OK, 0 fallidos ===")


if __name__ == "__main__":
    main()
