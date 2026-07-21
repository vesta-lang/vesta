#!/usr/bin/env python3
"""Genera el sitio HTML de la base de datos de instrucciones.

Lee los ficheros generados (via @c database.py) y produce un pequeno sitio
estatico (multiples paginas + assets compartidos):

    <out>/index.html       tabla de instrucciones (plantilla viewer.html)
    <out>/analyzer.html    analizador de asm       (plantilla analyzer.html)
    <out>/assets/db.js      datos embebidos (window.VESTA_DB)
    <out>/assets/app.css    estilos compartidos
    <out>/assets/viewer.js  logica de la tabla
    <out>/assets/analyzer.js logica del analizador

La presentacion vive en @c templates/ (editable con herramientas reales); este
script solo produce los datos y ensambla el sitio.  Los datos van en formato
CLASE+MAPEO (como los .vxarch): en vez de repetir el coste por forma y microarq,
se guardan las clases de scheduling deduplicadas y un array forma->clase.  Asi el
tamano no crece al anadir microarquitecturas.  Los assets se enlazan por
<script src>/<link> (funciona en file:// y en GitHub Pages).

    python tools/import/dump_html.py <dir_db> <dir_salida>
    python tools/import/dump_html.py timings/x86 site/
"""
import json
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import database  # noqa: E402

_TPL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "templates")
_KIND_LETTER = {"0": "R", "1": "A", "2": "F", "3": "M"}


def _ops_compact(s):
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
    if lat == "-":
        return ""
    out = []
    for e in lat.split(","):
        p = e.split(":")
        ub = "(ub)" if len(p) > 4 else ""
        out.append("op%s->op%s %s%s%s" %
                   (p[0], p[1], p[3], _KIND_LETTER.get(p[2], "?"), ub))
    return ", ".join(out)


def _pretty_ports(ports, legend):
    if ports == "-":
        return ""
    return " ".join("%sx%s" % (u, legend[int(g)]) for g, u in
                    (t.split("*") for t in ports.split(",")))


def _family(name, isa):
    if isa == "x86":
        return "intel" if name.startswith("intel") else \
               "amd" if name.startswith("amd") else ""
    return isa                                     # arm64/arm32 -> arm; riscv


# Definicion de las ISAs del sitio: (clave, etiqueta, vxisa, patron vxarch,
# vxfeat, familia CSS).  El patron filtra los .vxarch de cada ISA (ARM comparte
# carpeta entre A64 y A32/T32; se separan por el sufijo -a32).
_ISAS = [
    ("x86", "x86-64", "x86/x86.vxisa", ("x86", None, False), "x86/x86.vxfeat"),
    ("arm64", "AArch64", "arm/arm.vxisa", ("arm", "-a32", False), "arm/arm.vxfeat"),
    ("arm32", "AArch32 (A32/T32)", "arm/arm32.vxisa", ("arm", "-a32", True),
     "arm/arm.vxfeat"),
    ("riscv", "RISC-V", "riscv/riscv.vxisa", ("riscv", None, False),
     "riscv/riscv.vxfeat"),
]


def _build_isa(root, key, vxisa_rel, vxpat):
    """{meta, forms, arches} de una ISA (formato clase+mapeo)."""
    import glob
    vxisa = os.path.join(root, vxisa_rel)
    if not os.path.exists(vxisa):
        return None
    forms_raw = database.load_vxisa(vxisa)
    nforms = (max(forms_raw) + 1) if forms_raw else 0
    subdir, a32_suffix, want_a32 = vxpat

    arches = []
    for p in sorted(glob.glob(os.path.join(root, subdir, "*.vxarch"))):
        is_a32 = a32_suffix is not None and os.path.basename(p).count(a32_suffix)
        if bool(is_a32) != bool(want_a32):
            continue                               # separa A64 de A32/T32
        name, legend, classes, form_class = database.load_vxarch(p)
        cls_list = []
        for cid in range(len(classes)):
            c = classes[cid]
            cls_list.append([c["recip_tp"], int(c["uops"]),
                             int(c["microcoded"]) | (int(c["macro_fusible"]) << 1),
                             c["div_cycles"], _pretty_lat(c["latencies"]),
                             _pretty_ports(c["ports"], legend)])
        amap = [-1] * nforms
        for fid, cid in form_class.items():
            amap[fid] = cid
        disp = name[:-5] if name.endswith("-llvm") else name   # nombre limpio
        arches.append({"name": disp, "family": _family(name, key),
                       "classes": cls_list, "map": amap})

    forms = []
    for fid in sorted(forms_raw):
        fm = forms_raw[fid]
        memflags = (int(fm["mem"]) | (int(fm["imm"]) << 1) |
                    (int(fm["wflags"]) << 2) | (int(fm["rflags"]) << 3))

        def d(k):
            v = fm.get(k, "-")
            return "" if v == "-" else v
        forms.append([
            fid, fm["uid"], fm["iclass"], fm["ext"], fm["opcode"],
            d("enc"), fm["rmask"], fm["wmask"], memflags, d("overlay"),
            _ops_compact(fm["operands"]), d("string"), d("summary"),
            d("category"), fm["checksum"], d("url")])

    meta = {"forms": len(forms_raw), "arches": len(arches),
            "date": "?", "sha": "?", "schema": "?"}
    with open(vxisa, "r", encoding="ascii") as f:
        for tok in f.readline().split():
            if tok.startswith("date="):
                meta["date"] = tok[5:]
            elif tok.startswith("xml_sha256="):
                meta["sha"] = tok[11:19] + "..."
            elif tok.startswith("schema="):
                meta["schema"] = tok[7:]
    for k in ("date", "sha"):                          # sin marcadores de herramienta
        if "llvm" in meta[k].lower():
            meta[k] = "?"
    return {"meta": meta, "arches": arches, "forms": forms}


def _build_features(root, vxfeat_rel):
    """{table:[nombre...], cpus:[[cpu,sched,[ids]]...]} de un .vxfeat."""
    path = os.path.join(root, vxfeat_rel)
    if not os.path.exists(path):
        return None
    meta, table, cpus = database.load_vxfeat(path)
    idx = {n: i for i, n in enumerate(table)}
    rows = []
    for cpu in sorted(cpus):
        ids = sorted((idx[f] for f in cpus[cpu]["features"]))
        rows.append([cpu, cpus[cpu]["sched"], ids])
    return {"table": table, "cpus": rows}


def build_data(root):
    """window.VESTA_DB multi-ISA: {isas:{clave:{meta,forms,arches}},
    features:{clave:{table,cpus}}, order:[...], labels:{...}}."""
    isas, features, order, labels = {}, {}, [], {}
    feat_by_file = {}
    for key, label, vxisa, vxpat, vxfeat in _ISAS:
        got = _build_isa(root, key, vxisa, vxpat)
        if not got:
            continue
        isas[key] = got
        order.append(key)
        labels[key] = label
        if vxfeat not in feat_by_file:
            fb = _build_features(root, vxfeat)
            if fb:
                feat_by_file[vxfeat] = fb
        if vxfeat in feat_by_file:
            features[key] = feat_by_file[vxfeat]
    total_forms = sum(v["meta"]["forms"] for v in isas.values())
    total_arch = sum(v["meta"]["arches"] for v in isas.values())
    meta = {"forms": total_forms, "arches": total_arch,
            "isas": len(isas), "date": "?", "sha": "-", "schema": "1"}
    return {"isas": isas, "features": features, "order": order,
            "labels": labels, "meta": meta}


def _render(tpl_name, meta):
    with open(os.path.join(_TPL, tpl_name), "r", encoding="utf-8") as f:
        html = f.read()
    for k, v in (("{{FORMS}}", "{:,}".format(meta["forms"]).replace(",", ".")),
                 ("{{ARCHES}}", str(meta["arches"])), ("{{DATE}}", meta["date"]),
                 ("{{SHA}}", meta["sha"]), ("{{SCHEMA}}", meta["schema"]),
                 ("{{ISAS}}", str(meta.get("isas", 1)))):
        html = html.replace(k, v)
    return html


def main():
    if len(sys.argv) < 3:
        sys.exit("uso: python dump_html.py <dir_db> <dir_salida>")
    db_dir, out = sys.argv[1], sys.argv[2]
    assets = os.path.join(out, "assets")
    os.makedirs(assets, exist_ok=True)

    data = build_data(db_dir)
    with open(os.path.join(assets, "db.js"), "w", encoding="utf-8",
              newline="\n") as f:
        f.write("window.VESTA_DB=")
        json.dump(data, f, separators=(",", ":"))
        f.write(";\n")
    for a in ("app.css", "micro.js", "tip.js", "i18n.js",
              "viewer.js", "analyzer.js", "features.js"):
        src = os.path.join(_TPL, a)
        if os.path.exists(src):
            shutil.copyfile(src, os.path.join(assets, a))
    for page, tpl in (("index.html", "viewer.html"),
                      ("analyzer.html", "analyzer.html"),
                      ("features.html", "features.html")):
        tp = os.path.join(_TPL, tpl)
        if os.path.exists(tp):
            with open(os.path.join(out, page), "w", encoding="utf-8",
                      newline="\n") as f:
                f.write(_render(tpl, data["meta"]))

    sz = os.path.getsize(os.path.join(assets, "db.js")) / 1e6
    print("[dump_html] %d ISAs, %d formas, %d microarq -> %s (db.js %.1f MB)"
          % (data["meta"]["isas"], data["meta"]["forms"],
             data["meta"]["arches"], out, sz))


if __name__ == "__main__":
    main()
