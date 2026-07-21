#!/usr/bin/env python3
"""Reporte de completitud del coste ARM: cobertura UNION sobre todos los cores.

Para cada forma comprueba si ALGUN core (.vxarch) le da coste.  Lo que no tiene
coste en NINGUN core se clasifica por su motivo (extension) para distinguir:
  - AUSENCIA FISICA: la extension no la implementa ningun core que tengamos
    (SME/SME2 -> hace falta un core Armv9.2+; SVE -> ya cubierto por V2).
  - NO LISTADO: instrucciones que ninguna SWOG cronometra (sistema, coprocesador
    deprecado) -> "sin coste publicado", estado completo y honesto, no un hueco.

    python tools/import/arm_cost_report.py <arm.vxisa> <core1.vxarch> [core2 ...]
"""
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import database  # noqa: E402


def main():
    if len(sys.argv) < 3:
        sys.exit("uso: python arm_cost_report.py <arm.vxisa> <core.vxarch> ...")
    vxisa = sys.argv[1]
    forms = database.load_vxisa(vxisa)
    covered = set()               # form_ids con coste en >=1 core
    per_core = {}
    for vc in sys.argv[2:]:
        _, _, _, fc = database.load_vxarch(vc)
        n = 0
        for fid, cid in fc.items():
            if cid >= 0:
                covered.add(fid)
                n += 1
        per_core[os.path.basename(vc)] = n

    tot = len(forms)
    uni = len(covered)
    print("== COBERTURA UNION (%s) ==" % os.path.basename(vxisa))
    print("  formas: %d | con coste en >=1 core: %d (%d%%) | sin coste: %d"
          % (tot, uni, 100 * uni // tot, tot - uni))
    for c, n in sorted(per_core.items(), key=lambda x: -x[1]):
        print("    %-22s %d (%d%%)" % (c, n, 100 * n // tot))

    # sin coste en ningun core, por extension
    gap_ext = Counter()
    gap_mn = {}
    for fid in forms:
        if fid not in covered:
            e = forms[fid]["ext"]
            gap_ext[e] += 1
            gap_mn.setdefault(e, set()).add(forms[fid]["iclass"])
    print("\n== SIN COSTE EN NINGUN CORE, por extension ==")
    for e, n in gap_ext.most_common():
        print("  %-10s %5d formas  (%d mnemonicos distintos)"
              % (e, n, len(gap_mn[e])))
    # sugerencia
    need_core = [e for e in gap_ext if e in ("SME", "SME2")]
    if need_core:
        print("\n  -> %s: hace falta la SWOG de un core con SME (Armv9.2+: "
              "Cortex-X4, Neoverse V3...)." % "/".join(need_core))


if __name__ == "__main__":
    main()
