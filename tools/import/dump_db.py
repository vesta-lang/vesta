#!/usr/bin/env python3
"""Volcado legible de la base de datos de instrucciones (puerta de validacion).

Lee los ficheros generados (x86.vxisa + *.vxarch) y para las formas que casan
con una consulta imprime su SEMaNTICA (operandos, efectos, encoding, overlay) y
su COSTE por microarquitectura (latencias, throughput, uops, puertos).  Si esto
imprime lo esperado para cientos de instrucciones, el parser + el IR + el
optimizer + la BD funcionan, y el backend/scheduler/LSP solo consumen esta API.

    python tools/import/dump_db.py <dir_db> <consulta-substring-de-uid>
    python tools/import/dump_db.py timings/x86 ADD_GPRv_GPRv_01/64
    python tools/import/dump_db.py timings/x86 CPUID
"""
import glob
import os
import sys

_FLAG = {1: "r", 2: "w", 4: "impl", 8: "supp"}


def _load_vxisa(path):
    forms = {}
    with open(path, "r", encoding="ascii") as f:
        for line in f:
            if line.startswith(("vxisa", "#")) or not line.strip():
                continue
            c = line.rstrip("\n").split("|")
            forms[int(c[0])] = {
                "checksum": c[1], "uid": c[2], "iclass": c[3], "ext": c[4],
                "opcode": c[5], "enc": c[6], "rmask": c[7], "wmask": c[8],
                "mem": c[9], "imm": c[10], "wflags": c[11], "rflags": c[12],
                "operands": c[13], "overlay": c[14]}
    return forms


def _load_vxarch(path):
    ports, classes, form_class = [], {}, {}
    name = ""
    with open(path, "r", encoding="ascii") as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("vxarch"):
                for tok in line.split():
                    if tok.startswith("name="):
                        name = tok[5:]
            elif line.startswith("ports:"):
                for tok in line[6:].split():
                    if "=" in tok:
                        ports.append(tok.split("=", 1)[1])
            elif line.startswith("class "):
                c = line[6:].split("|")
                classes[int(c[0])] = c[1:]
            elif line and line[0].isdigit():
                fid, cid = line.split("|")
                form_class[int(fid)] = int(cid)
    return name, ports, classes, form_class


def _fmt_operands(s):
    if s == "-":
        return "(ninguno)"
    out = []
    for op in s.split(";"):
        idx, kind, width, flags, regset = op.split(",")
        fl = "|".join(v for k, v in _FLAG.items() if int(flags) & k) or "-"
        rs = "" if regset == "-" else " {%s}" % regset
        out.append("    op%s %s w%s [%s]%s" % (idx, kind, width, fl, rs))
    return "\n".join(out)


def main():
    if len(sys.argv) < 3:
        sys.exit("uso: python dump_db.py <dir_db> <consulta>")
    db, query = sys.argv[1], sys.argv[2]
    forms = _load_vxisa(os.path.join(db, "x86.vxisa"))
    arches = [_load_vxarch(p) for p in sorted(glob.glob(os.path.join(db, "*.vxarch")))]

    hits = [(fid, fm) for fid, fm in sorted(forms.items())
            if query.lower() in fm["uid"].lower()]
    if not hits:
        sys.exit("sin coincidencias para '%s'" % query)
    for fid, fm in hits[:20]:
        print("=" * 70)
        print("iform: %s   (id=%d  checksum=%s)" % (fm["uid"], fid, fm["checksum"]))
        print("  iclass=%s ext=%s opcode=%s enc=%s" %
              (fm["iclass"], fm["ext"], fm["opcode"], fm["enc"]))
        print("  efectos: rmask=%s wmask=%s mem=%s imm=%s wflags=%s rflags=%s "
              "overlay=%s" % (fm["rmask"], fm["wmask"], fm["mem"], fm["imm"],
                              fm["wflags"], fm["rflags"], fm["overlay"]))
        print("  operandos:\n%s" % _fmt_operands(fm["operands"]))
        for name, ports, classes, form_class in arches:
            cid = form_class.get(fid)
            if cid is None:
                print("  timing %-20s: (sin dato)" % name)
                continue
            tp, uops, mc, mf, div, lat, pt = classes[cid]
            pretty_ports = pt
            if pt != "-":
                pretty_ports = " ".join(
                    "%sx%s" % (u, ports[int(g)]) for g, u in
                    (t.split("*") for t in pt.split(",")))
            print("  timing %-20s: recip_tp=%s uops=%s%s%s%s"
                  % (name, tp, uops, " microcoded" if mc == "1" else "",
                     " macro_fusible" if mf == "1" else "",
                     " div_cycles=" + div if div != "-1.00" else ""))
            print("      latencias: %s" % lat)
            print("      puertos  : %s" % pretty_ports)


if __name__ == "__main__":
    main()
