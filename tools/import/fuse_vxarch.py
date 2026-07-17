#!/usr/bin/env python3
"""Fusiona varios .vxarch del MISMO core en UNO (una version por microarq).

Motivacion: un core puede tener coste de dos fuentes -- la SWOG oficial de ARM
(medida en hardware real, AUTORITATIVA) y el SchedModel de LLVM (modelado,
cubre SVE/SVE2/sistema que la SWOG no cronometra).  En vez de dos DB del mismo
core, se fusionan: por PRIORIDAD (orden de los argumentos), cada forma toma su
coste de la PRIMERA fuente que lo tenga.  Los puertos se unifican por NOMBRE
(SWOG 'V0' y LLVM 'V0' son el mismo puerto -> mismo indice).

    python tools/import/fuse_vxarch.py <salida.vxarch> <core> <isa> <fuente1> [fuente2 ...]
    # fuente1 = prioritaria (p.ej. la SWOG); fuente2 = rellena huecos (LLVM)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import database    # noqa: E402
import serialize   # noqa: E402

DBV = serialize.DB_FORMAT_VERSION


def main():
    if len(sys.argv) < 5:
        sys.exit("uso: python fuse_vxarch.py <salida> <core> <isa> <fuente1> ...")
    out, core, isa = sys.argv[1:4]
    srcs = sys.argv[4:]
    loaded = []                      # (port_names, classes, form_class)
    for p in srcs:
        name, ports, classes, fc = database.load_vxarch(p)
        loaded.append((ports, classes, fc))

    # puertos unificados por NOMBRE
    uni = []
    uni_idx = {}
    for ports, _, _ in loaded:
        for nm in ports:
            if nm not in uni_idx:
                uni_idx[nm] = len(uni)
                uni.append(nm)

    def remap_ports(ports_str, local_names):
        if ports_str in ('', '-'):
            return '-'
        out_parts = []
        for tok in ports_str.split(','):
            gi, uo = tok.split('*')
            nm = local_names[int(gi)] if int(gi) < len(local_names) else str(gi)
            out_parts.append('%d*%s' % (uni_idx[nm], uo))
        return ','.join(out_parts) or '-'

    # por forma, la primera fuente (prioridad) que tenga coste
    all_fids = set()
    for _, _, fc in loaded:
        all_fids.update(fc)
    fused_classes = []               # lista de tuplas serializables
    class_key = {}
    form_class = {}
    for fid in sorted(all_fids):
        for ports, classes, fc in loaded:
            cid = fc.get(fid, -1)
            if cid < 0:
                continue
            c = classes[cid]
            pstr = remap_ports(c['ports'], ports)
            key = (c['recip_tp'], c['uops'], c['microcoded'], c['macro_fusible'],
                   c['div_cycles'], c['latencies'], pstr)
            ncid = class_key.get(key)
            if ncid is None:
                ncid = len(fused_classes)
                class_key[key] = ncid
                fused_classes.append(key)
            form_class[fid] = ncid
            break

    with open(out, 'w', encoding='ascii', newline='\n') as f:
        f.write("vxarch %d name=%s family=arm isa=%s source=arm-swog+llvm "
                "src_arch=%s date=fused xml_sha256=- classes=%d mapped=%d\n"
                % (DBV, core, isa, core, len(fused_classes), len(form_class)))
        f.write("ports: " + " ".join("%d=%s" % (i, g) for i, g in enumerate(uni)) + "\n")
        f.write("# class|recip_tp|uops|microcoded|macro_fusible|div_cycles"
                "|latencies(so:to:kind:cyc[:ub],...)|ports(idx*uops,...)\n")
        for cid, k in enumerate(fused_classes):
            f.write("class %d|%s|%s|%s|%s|%s|%s|%s\n"
                    % (cid, k[0], k[1], k[2], k[3], k[4], k[5], k[6]))
        f.write("# form_id|class_id\n")
        for fid in sorted(form_class):
            f.write("%d|%d\n" % (fid, form_class[fid]))
    # resumen de aporte por fuente
    contrib = [0] * len(loaded)
    for fid in form_class:
        for i, (ports, classes, fc) in enumerate(loaded):
            if fc.get(fid, -1) >= 0:
                contrib[i] += 1
                break
    print("[fuse] %s: %d formas con coste | aporte por fuente %s"
          % (core, len(form_class),
             ", ".join("%s=%d" % (os.path.basename(srcs[i]), contrib[i])
                       for i in range(len(srcs)))))


if __name__ == "__main__":
    main()
