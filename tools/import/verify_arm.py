#!/usr/bin/env python3
"""Verificador de la base ARM (arm.vxisa) contra la VERDAD de la fuente (MRAS).

Responde "obtenemos todos los datos bien?" con dos bloques:

  COBERTURA  -- reparsea el MRAS de forma independiente (segundo lector, no el
                importador) y compara: numero de instrucciones, alias y
                encodings.  Lista los <encoding> del MRAS que NO estan en
                arm.vxisa (y viceversa).  Cero diferencia = capturamos todo.
  INVARIANTES -- comprueba propiedades que deben cumplirse SIEMPRE: opcode de 32
                bits [01x], kinds validos, ningun operando sin lee/escribe,
                ninguna colision de clave estructural (dos encodings
                indistinguibles), mnemonicos y anchos sanos.

    python tools/import/verify_arm.py <dir_ISA_A64_xml> <arm.vxisa>
"""
import glob
import os
import re
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import database  # noqa: E402

_VALID_KINDS = {"reg", "mem", "imm", "relbr", "absbr", "flags", "agen", "?"}


def truth_encodings(src):
    """Conjunto de nombres de <encoding> del MRAS (instruction + alias) y cuentas."""
    encs = set()
    n_instr = n_alias = 0
    for p in sorted(glob.glob(os.path.join(src, "*.xml"))):
        b = os.path.basename(p)
        if b.startswith(("index", "encodingindex", "fpsimdindex", "notice",
                        "shared_pseudocode", "constraint_text")):
            continue
        try:
            r = ET.parse(p).getroot()
        except ET.ParseError:
            continue
        if r.tag != 'instructionsection':
            continue
        t = r.get('type')
        if t == 'instruction':
            n_instr += 1
        elif t == 'alias':
            n_alias += 1
        else:
            continue
        for e in r.findall('./classes/iclass/encoding'):
            encs.add(e.get('name'))
    return encs, n_instr, n_alias


def main():
    if len(sys.argv) < 3:
        sys.exit("uso: python verify_arm.py <dir_ISA_A64_xml> <arm.vxisa>")
    src, vxisa = sys.argv[1], sys.argv[2]
    truth, n_instr, n_alias = truth_encodings(src)
    forms = database.load_vxisa(vxisa)

    ours = set()          # uid sin el sufijo de anchos = nombre del encoding
    key_seen = {}
    dup_keys = []         # mismo key Y mismo nombre -> indistinguibles (fallo)
    dup_docvar = []       # mismo bits, nombre distinto -> variante de doc (ok)
    bad_opcode = bad_kind = no_rw = 0
    for fid, fm in forms.items():
        enc = fm["uid"].split("/")[0]
        ours.add(enc)
        # clave estructural (iclass+ext+opcode+enc+operands) -> colisiones. Incluye
        # enc (en AArch32 lleva isa_set=A32/T32, que distingue encodings).
        k = (fm["iclass"], fm["ext"], fm["opcode"], fm["enc"], fm["operands"])
        if k in key_seen:
            prev = key_seen[k]
            # nombres iguales = verdaderamente indistinguible; distintos = el MRAS
            # lista dos nombres para los mismos bits (ADR vs ADD-a-PC): legitimo.
            if forms[prev]["uid"].split("/")[0] == enc:
                dup_keys.append((fid, prev))
            else:
                dup_docvar.append((fid, prev))
        else:
            key_seen[k] = fid
        # opcode: A64/A32 = 32 bits; T32 = 16 o 32 (Thumb).
        if not re.fullmatch(r"[01x]{16}|[01x]{32}", fm["opcode"]):
            bad_opcode += 1
        if fm["operands"] != "-":
            for op in fm["operands"].split(";"):
                c = op.split(",")
                if c[1] not in _VALID_KINDS:
                    bad_kind += 1
                flags = int(c[3])
                if c[1] in ("reg", "mem") and (flags & 3) == 0:
                    no_rw += 1

    missing = sorted(truth - ours)     # en el MRAS, no en la base
    extra = sorted(ours - truth)       # en la base, no en el MRAS (deberia ser 0)

    print("== COBERTURA ==")
    print("  instrucciones MRAS : %d" % n_instr)
    print("  alias MRAS         : %d" % n_alias)
    print("  encodings MRAS     : %d" % len(truth))
    print("  formas en arm.vxisa: %d" % len(forms))
    print("  encodings unicos   : %d" % len(ours))
    print("  FALTAN (MRAS y no en base): %d" % len(missing))
    if missing:
        print("    " + ", ".join(missing[:20])
              + (" ..." if len(missing) > 20 else ""))
    print("  SOBRAN (base y no en MRAS): %d" % len(extra))
    if extra:
        print("    " + ", ".join(extra[:20]))

    print("== INVARIANTES ==")
    print("  opcode != 32 bits [01x]     : %d" % bad_opcode)
    print("  kind de operando invalido   : %d" % bad_kind)
    print("  operando reg/mem sin lee/escr: %d" % no_rw)
    print("  colisiones de clave (encodings indistinguibles): %d" % len(dup_keys))
    if dup_keys[:10]:
        print("    fids: " + ", ".join("%d~%d" % d for d in dup_keys[:10]))
    if dup_docvar:
        print("  mismos bits con nombre distinto (variantes MRAS, ok): %d"
              % len(dup_docvar))

    ok = (not missing and not extra and not bad_opcode and not bad_kind
          and not no_rw and not dup_keys)
    print("\nRESULTADO:", "OK -- capturamos todos los datos" if ok
          else "REVISAR (ver diferencias arriba)")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
