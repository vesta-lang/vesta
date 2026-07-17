#!/usr/bin/env python3
"""Verificador de la base x86 (x86.vxisa) contra la VERDAD de la fuente (uops.info).

Equivalente a @ref verify_arm para x86: segundo lector INDEPENDIENTE del
instructions.xml de uops.info (no reusa el importador) que responde "obtenemos
todos los datos bien?" con dos bloques:

  COBERTURA  -- cuenta los <instruction> de la fuente (iforms), sus iclass e
                extensiones, y confirma que NINGUN iclass ni extension se pierde
                en x86.vxisa.  La diferencia iforms - formas = duplicados
                estructurales (colapsados a proposito por el importador).
  INVARIANTES -- opcode presente, kinds de operando validos, y CERO colisiones
                de clave estructural completa (iclass|ext|opcode|enc|operands):
                dos formas con la misma clave serian indistinguibles (el
                importador deduplica por form_key, asi que debe ser 0).

    python tools/import/verify_x86.py <instructions.xml> <x86.vxisa>
"""
import os
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import database  # noqa: E402

_VALID_KINDS = {"reg", "mem", "imm", "flags", "agen", "relbr", "absbr", "?"}


def truth(xml_path):
    """(n_iform, iclasses, extensions) del instructions.xml (streaming)."""
    n = 0
    iclasses = set()
    exts = set()
    for ev, el in ET.iterparse(xml_path, events=("end",)):
        if el.tag != "instruction":
            continue
        if el.get("iform", ""):
            n += 1
            iclasses.add(el.get("iclass", ""))
            exts.add(el.get("extension", ""))
        el.clear()
    return n, iclasses, exts


def main():
    if len(sys.argv) < 3:
        sys.exit("uso: python verify_x86.py <instructions.xml> <x86.vxisa>")
    xml_path, vxisa = sys.argv[1], sys.argv[2]
    n_iform, tclass, text = truth(xml_path)
    forms = database.load_vxisa(vxisa)

    our_class = set()
    our_ext = set()
    key_seen = {}
    dup_keys = []
    bad_opcode = bad_kind = no_rw = 0
    for fid, fm in forms.items():
        our_class.add(fm["iclass"])
        our_ext.add(fm["ext"])
        # clave estructural COMPLETA (incluye enc) -> colisiones reales
        k = (fm["iclass"], fm["ext"], fm["opcode"], fm["enc"], fm["operands"])
        if k in key_seen:
            dup_keys.append((fid, key_seen[k]))
        else:
            key_seen[k] = fid
        if not fm["opcode"] or fm["opcode"] == "-":
            bad_opcode += 1
        if fm["operands"] != "-":
            for op in fm["operands"].split(";"):
                c = op.split(",")
                if c[1] not in _VALID_KINDS:
                    bad_kind += 1
                if c[1] in ("reg", "mem") and (int(c[3]) & 3) == 0:
                    no_rw += 1

    miss_class = sorted(c for c in tclass if c and c not in our_class)
    miss_ext = sorted(e for e in text if e and e not in our_ext)

    print("== COBERTURA ==")
    print("  <instruction> (iforms) en la fuente: %d" % n_iform)
    print("  formas en x86.vxisa                : %d" % len(forms))
    print("  duplicados estructurales colapsados: %d" % (n_iform - len(forms)))
    print("  iclass en fuente / en base         : %d / %d"
          % (len(tclass), len(our_class)))
    print("  extensiones en fuente / en base    : %d / %d"
          % (len(text), len(our_ext)))
    print("  iclass PERDIDOS (fuente y no en base): %d" % len(miss_class))
    if miss_class:
        print("    " + ", ".join(miss_class[:20]))
    print("  extensiones PERDIDAS               : %d" % len(miss_ext))
    if miss_ext:
        print("    " + ", ".join(miss_ext[:20]))

    print("== INVARIANTES ==")
    print("  opcode ausente               : %d" % bad_opcode)
    print("  kind de operando invalido    : %d" % bad_kind)
    print("  operando reg/mem sin lee/escr: %d" % no_rw)
    print("  colisiones de clave completa : %d" % len(dup_keys))
    if dup_keys[:8]:
        print("    fids: " + ", ".join("%d~%d" % d for d in dup_keys[:8]))

    ok = (not miss_class and not miss_ext and not bad_opcode and not bad_kind
          and not dup_keys)
    print("\nRESULTADO:", "OK -- capturamos todos los datos" if ok
          else "REVISAR (ver diferencias arriba)")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
