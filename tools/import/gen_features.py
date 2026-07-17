#!/usr/bin/env python3
"""Genera la DB de FEATURES por CPU/microarquitectura (multi-arch: x86 y ARM),
SERIALIZADA con tabla+IDs (como las ISAs con FormID) para no repetir strings.

De una extraccion unica de LLVM (`llvm-tblgen --dump-json`), un UNICO archivo por
ISA con:
  - una tabla de features canonicas (id denso sobre nombres ordenados);
  - una fila por CPU real (record Processor) que referencia esos IDs.

Sirve para especializar codegen (que ISA/extension implementa cada core),
filtrar coste (no dar AVX512 a un core sin AVX512) y como doc en la pagina.

Formato (ASCII):

    vxfeat 1 isa=<x86|arm> nfeat=<N> ncpu=<M>
    feats: 0=<nombre> 1=<nombre> ...          (ordenados, id denso)
    # cpu|sched|featid,featid,...
    haswell|HaswellModel|2,3,10,...

    python tools/import/gen_features.py <json> <arm|x86> <salida.vxfeat>
"""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from llvm_sched import _dname, _expand_implies   # noqa: E402

DBV = 1


def _short(feat):
    """FeatureAVX512F -> AVX512F ; conserva el resto tal cual."""
    return re.sub(r'^Feature', '', feat or '')


def main():
    if len(sys.argv) < 4:
        sys.exit("uso: python gen_features.py <json> <arm|x86> <salida.vxfeat>")
    jsonp, isa, out = sys.argv[1:4]
    d = json.load(open(jsonp, encoding='utf-8'))
    inst = d['!instanceof']

    rows = []                                    # (cpu, sched, set[feat_short])
    seen = set()
    all_feats = set()
    for n in inst.get('Processor', []):
        r = d[n]
        cpu = r.get('Name')
        if not cpu or cpu in seen:
            continue
        seen.add(cpu)
        sched = _dname(r.get('SchedModel')) or '-'
        feats = {_short(f) for f in
                 _expand_implies(d, (_dname(f) for f in r.get('Features', []))) if f}
        all_feats |= feats
        rows.append((cpu, sched, feats))

    table = sorted(all_feats)                     # id denso sobre nombres ordenados
    fid = {name: i for i, name in enumerate(table)}
    rows.sort(key=lambda x: x[0])

    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, 'w', encoding='ascii', newline='\n') as fo:
        fo.write("vxfeat %d isa=%s nfeat=%d ncpu=%d\n"
                 % (DBV, isa, len(table), len(rows)))
        fo.write("feats: " + " ".join("%d=%s" % (i, n) for i, n in enumerate(table)) + "\n")
        fo.write("# cpu|sched|featid,...\n")
        for cpu, sched, feats in rows:
            ids = ",".join(str(fid[x]) for x in sorted(feats, key=lambda z: fid[z]))
            fo.write("%s|%s|%s\n" % (cpu, sched, ids))
    print("[gen_features] isa=%s: %d CPU, %d features unicas -> %s"
          % (isa, len(rows), len(table), out))


if __name__ == "__main__":
    main()
