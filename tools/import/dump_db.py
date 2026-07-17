#!/usr/bin/env python3
"""Volcado legible de la base de datos de instrucciones (puerta de validacion).

Para las formas que casan con una consulta imprime su SEMaNTICA (operandos,
efectos, encoding, overlay) y su COSTE por microarquitectura.  Si esto imprime
lo esperado para cientos de instrucciones, el parser + IR + optimizer + BD
funcionan, y backend/scheduler/LSP solo consumen esta API.

    python tools/import/dump_db.py <dir_db> <consulta> [--limit N] [--debug-key]
    python tools/import/dump_db.py timings/x86 ADD_GPRv_GPRv_01/64  --debug-key
    python tools/import/dump_db.py timings/x86 ADD --limit 0        # todos
"""
import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import database  # noqa: E402

# Bits del campo 'flags' de cada operando.
FLAG_R, FLAG_W, FLAG_IMPL, FLAG_SUPP = 1, 2, 4, 8
_FLAG_NAME = {FLAG_R: "r", FLAG_W: "w", FLAG_IMPL: "impl", FLAG_SUPP: "supp"}


def _parse_operands(s):
    """[(idx, kind, width, flags_int, regset), ...] desde la columna 'operands'."""
    if s == "-":
        return []
    out = []
    for op in s.split(";"):
        idx, kind, width, flags, regset = op.split(",")
        out.append((idx, kind, width, int(flags),
                    "" if regset == "-" else regset))
    return out


def _fmt_operands(ops):
    if not ops:
        return "    (ninguno)"
    lines = []
    for idx, kind, width, flags, regset in ops:
        fl = "|".join(v for k, v in _FLAG_NAME.items() if flags & k) or "-"
        rs = " {%s}" % regset if regset else ""
        lines.append("    op%s %s w%s [%s]%s" % (idx, kind, width, fl, rs))
    return "\n".join(lines)


def _print_debug_key(fm, ops):
    """Clave estructural 'bonita' (--debug-key): por que dos formas comparten o
    difieren en FormID.  Reconstruida de las columnas del .vxisa."""
    print("  form_key:")
    print("    iclass=%s extension=%s opcode=%s" %
          (fm["iclass"], fm["ext"], fm["opcode"]))
    print("    encoding: %s" % (fm["enc"] if fm["enc"] != "-" else "(ninguno)"))
    print("    operands:")
    for idx, kind, width, flags, regset in ops:
        fl = "|".join(v for k, v in _FLAG_NAME.items() if flags & k) or "-"
        print("      [%s] %s w%s %s regset=%s" %
              (idx, kind, width, fl, regset or "-"))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    debug_key = "--debug-key" in sys.argv
    limit = 20
    for a in sys.argv:
        if a.startswith("--limit"):
            limit = int(a.split("=", 1)[1]) if "=" in a else int(
                sys.argv[sys.argv.index(a) + 1])
    if len(args) < 2:
        sys.exit("uso: python dump_db.py <dir_db> <consulta> "
                 "[--limit N] [--debug-key]")
    db, query = args[0], args[1]
    forms = database.load_vxisa(os.path.join(db, "x86.vxisa"))
    arches = [database.load_vxarch(p)
              for p in sorted(glob.glob(os.path.join(db, "*.vxarch")))]

    hits = [(fid, fm) for fid, fm in sorted(forms.items())
            if query.lower() in fm["uid"].lower()]
    if not hits:
        sys.exit("sin coincidencias para '%s'" % query)
    shown = hits if limit == 0 else hits[:limit]
    for fid, fm in shown:
        ops = _parse_operands(fm["operands"])
        print("=" * 70)
        print("form: %s   (id=%d  checksum=%s)" % (fm["uid"], fid, fm["checksum"]))
        print("  iclass=%s ext=%s opcode=%s enc=%s" %
              (fm["iclass"], fm["ext"], fm["opcode"], fm["enc"]))
        print("  efectos: rmask=%s wmask=%s mem=%s imm=%s wflags=%s rflags=%s "
              "overlay=%s" % (fm["rmask"], fm["wmask"], fm["mem"], fm["imm"],
                              fm["wflags"], fm["rflags"], fm["overlay"]))
        print("  operandos:\n%s" % _fmt_operands(ops))
        if debug_key:
            _print_debug_key(fm, ops)
        for name, ports, classes, form_class in arches:
            cid = form_class.get(fid)
            if cid is None:
                print("  timing %-20s: (sin dato)" % name)
                continue
            c = classes[cid]
            pretty = c["ports"]
            if pretty != "-":
                pretty = " ".join("%sx%s" % (u, ports[int(g)]) for g, u in
                                  (t.split("*") for t in c["ports"].split(",")))
            print("  timing %-20s: recip_tp=%s uops=%s%s%s%s"
                  % (name, c["recip_tp"], c["uops"],
                     " microcoded" if c["microcoded"] == "1" else "",
                     " macro_fusible" if c["macro_fusible"] == "1" else "",
                     " div_cycles=" + c["div_cycles"]
                     if c["div_cycles"] != "-1.00" else ""))
            print("      latencias: %s" % c["latencies"])
            print("      puertos  : %s" % pretty)
    if limit and len(hits) > limit:
        print("... (%d mas; usa --limit 0 para todas)" % (len(hits) - limit))


if __name__ == "__main__":
    main()
