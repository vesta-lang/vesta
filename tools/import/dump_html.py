#!/usr/bin/env python3
"""Genera un volcado HTML autocontenido de la base de datos de instrucciones.

Lee los ficheros generados (via @c database.py), construye los registros y los
INYECTA en las plantillas de @c templates/ (viewer.html + viewer.css +
viewer.js).  La presentacion vive en esos ficheros (editables con herramientas
HTML/CSS/JS reales); este script solo aporta los datos.  La salida es un unico
`.html` autocontenido: se abre en el navegador (file://) o se sirve por GitHub
Pages, sin red ni dependencias.

Para que pese poco, los datos van en formato COLUMNAR (array de arrays, sin
repetir nombres de campo) y se omiten las listas largas de registros permitidos
(estan en el .vxisa si hacen falta).

    python tools/import/dump_html.py <dir_db> <salida.html>
    python tools/import/dump_html.py timings/x86 index.html
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import database  # noqa: E402

_TPL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "templates")
_KIND_LETTER = {"0": "R", "1": "A", "2": "F", "3": "M"}  # LatencyKind -> letra


def _ops_compact(s):
    """"idx,kind,width,flags,regset;..." -> "op0 reg64 rw, op1 mem64 r"."""
    if s == "-":
        return ""
    out = []
    for op in s.split(";"):
        idx, kind, width, flags, _ = op.split(",")
        f = int(flags)
        rw = "".join(l for b, l in ((1, "r"), (2, "w"), (4, "i"), (8, "s"))
                     if f & b)
        out.append("op%s %s%s %s" % (idx, kind, width, rw))
    return ", ".join(out)


def _pretty_lat(lat):
    """"so:to:kind:cyc[:ub],..." -> "op0->op0 1.0R, op1->op0 3.0M(ub)"."""
    if lat == "-":
        return ""
    out = []
    for e in lat.split(","):
        p = e.split(":")
        so, to, kind, cyc = p[0], p[1], p[2], p[3]
        ub = "(ub)" if len(p) > 4 else ""
        out.append("op%s->op%s %s%s%s" %
                   (so, to, cyc, _KIND_LETTER.get(kind, "?"), ub))
    return ", ".join(out)


def _pretty_ports(ports, legend):
    if ports == "-":
        return ""
    return " ".join("%sx%s" % (u, legend[int(g)]) for g, u in
                    (t.split("*") for t in ports.split(",")))


def build_records(db_dir):
    """(arch_names, rows) en formato columnar para embeber en el HTML.

    row = [id, uid, iclass, ext, opcode, enc, rmask, wmask, memflags, overlay,
           operandos, [ [tp,uops,notes,div,lat,ports] | null por microarq ],
           string, summary, category]."""
    import glob
    forms = database.load_vxisa(os.path.join(db_dir, "x86.vxisa"))
    arches = [database.load_vxarch(p)
              for p in sorted(glob.glob(os.path.join(db_dir, "*.vxarch")))]
    arch_names = [a[0] for a in arches]

    rows = []
    for fid in sorted(forms):
        fm = forms[fid]
        memflags = (int(fm["mem"]) | (int(fm["imm"]) << 1) |
                    (int(fm["wflags"]) << 2) | (int(fm["rflags"]) << 3))
        timings = []
        for name, ports, classes, form_class in arches:
            cid = form_class.get(fid)
            if cid is None:
                timings.append(None)
                continue
            c = classes[cid]
            timings.append([
                c["recip_tp"], int(c["uops"]),
                (int(c["microcoded"]) | (int(c["macro_fusible"]) << 1)),
                c["div_cycles"], _pretty_lat(c["latencies"]),
                _pretty_ports(c["ports"], ports)])
        rows.append([
            fid, fm["uid"], fm["iclass"], fm["ext"], fm["opcode"],
            "" if fm["enc"] == "-" else fm["enc"],
            fm["rmask"], fm["wmask"], memflags,
            "" if fm["overlay"] == "-" else fm["overlay"],
            _ops_compact(fm["operands"]), timings,
            "" if fm.get("string", "-") == "-" else fm["string"],
            "" if fm.get("summary", "-") == "-" else fm["summary"],
            "" if fm.get("category", "-") == "-" else fm["category"]])
    return arch_names, rows


def _read_meta(db_dir):
    """Procedencia desde la cabecera del .vxisa."""
    meta = {"date": "?", "sha": "?", "schema": "?"}
    with open(os.path.join(db_dir, "x86.vxisa"), "r", encoding="ascii") as f:
        for tok in f.readline().split():
            if tok.startswith("date="):
                meta["date"] = tok[5:]
            elif tok.startswith("xml_sha256="):
                meta["sha"] = tok[11:19] + "..."
            elif tok.startswith("schema="):
                meta["schema"] = tok[7:]
    return meta


def build_html(arch_names, rows, meta):
    """Inyecta datos y plantillas (viewer.css/js dentro de viewer.html)."""
    def tpl(name):
        with open(os.path.join(_TPL, name), "r", encoding="utf-8") as f:
            return f.read()
    data = json.dumps({"arches": arch_names, "rows": rows, "meta": meta},
                      separators=(",", ":"))
    opts = "".join("<option>%s</option>" % c
                   for c in sorted({r[2] for r in rows}))
    html = tpl("viewer.html")
    # Los placeholders de CSS/JS van envueltos en comentario en la plantilla
    # (para que sea valida por si sola); se sustituye el comentario completo.
    repl = {
        "/*{{CSS}}*/": tpl("viewer.css"),
        "/*{{JS}}*/": tpl("viewer.js"),
        "{{DATA}}": data, "{{OPTS}}": opts,
        "{{FORMS}}": "{:,}".format(len(rows)).replace(",", "."),
        "{{DATE}}": meta["date"], "{{SHA}}": meta["sha"],
        "{{SCHEMA}}": meta["schema"],
    }
    for k, v in repl.items():
        html = html.replace(k, v)
    return html


def main():
    if len(sys.argv) < 3:
        sys.exit("uso: python dump_html.py <dir_db> <salida.html>")
    db_dir, out = sys.argv[1], sys.argv[2]
    arch_names, rows = build_records(db_dir)
    html = build_html(arch_names, rows, _read_meta(db_dir))
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(html)
    print("[dump_html] %d formas -> %s (%.1f MB)"
          % (len(rows), out, os.path.getsize(out) / 1e6))


if __name__ == "__main__":
    main()
