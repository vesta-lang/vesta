#!/usr/bin/env python3
"""Driver: genera un .vxarch por CADA microarquitectura de los SchedModel de LLVM.

Toma el JSON de `llvm-tblgen --dump-json AArch64.td`, enumera todos los
@c SchedMachineModel completos y produce un @c <core>-llvm.vxarch por cada uno
(reusando @ref llvm_sched.build_model).  Asi, de una sola extraccion salen ~20
microarquitecturas ARM con coste que cubre SVE/SVE2 (muy por encima de lo que
las SWOG cronometran).

Para regenerar el JSON (extraccion unica, requiere llvm-tblgen; en WSL:
  llvm-tblgen-19 --dump-json -I <AArch64_td_dir> -I /usr/include/llvm-19 \
      <AArch64_td_dir>/AArch64.td -o aarch64.json

    python tools/import/gen_llvm_arm.py <aarch64.json> <arm.vxisa> <dir_salida>
"""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ir           # noqa: E402
import serialize    # noqa: E402
import database     # noqa: E402
import llvm_sched   # noqa: E402


def _core_name(model):
    """NeoverseN2Model -> neoverse-n2-llvm; CortexA53Model -> cortex-a53-llvm."""
    s = re.sub(r'Model$', '', model)
    s = re.sub(r'(?<=[a-z])(?=[A-Z])', '-', s)        # solo minuscula->mayuscula
    return s.lower() + '-llvm'


def main():
    if len(sys.argv) < 4:
        sys.exit("uso: python gen_llvm_arm.py <aarch64.json> <arm.vxisa> <salida>")
    jsonp, vxisa, out = sys.argv[1:4]
    os.makedirs(out, exist_ok=True)
    d = json.load(open(jsonp, encoding='utf-8'))
    forms = database.load_vxisa(vxisa)
    models = [m for m in d['!instanceof'].get('SchedMachineModel', [])
              if d[m].get('CompleteModel') == 1 and m != 'NoSchedModel']
    print("[gen_llvm_arm] %d modelos completos" % len(models))
    for model in models:
        cost, port_names = llvm_sched.build_model(d, model)
        classes, class_key = [], {}
        form_class = [-1] * (max(forms) + 1)
        mapped = 0
        for fid in sorted(forms):
            fm = forms[fid]
            mn = fm['iclass'].upper()
            c = llvm_sched._EXT_CATS  # noqa (solo para claridad)
            got = None
            for cat in llvm_sched._EXT_CATS.get(fm['ext'], ('INT',)):
                got = cost.get((mn, cat))
                if got:
                    break
            if got is None and fm['category'].startswith('alias:'):
                base = fm['category'][6:].split('_')[0].upper()
                for cat in llvm_sched._EXT_CATS.get(fm['ext'], ('INT',)):
                    got = cost.get((base, cat))
                    if got:
                        break
            if got is None:
                continue
            lat, uops, pslots = got
            key = (lat, uops, tuple(pslots))
            cid = class_key.get(key)
            if cid is None:
                cid = len(classes)
                class_key[key] = cid
                classes.append(ir.SchedulerClass(
                    latencies=[ir.LatencyEdge(0, 0, float(lat), ir.LATENCY_RESULT)] if lat else [],
                    recip_tp=-1.0, uops=uops, microcoded=False, macro_fusible=False,
                    div_cycles=-1.0,
                    ports=[ir.PortSlot(group=g, uops=1.0) for g in pslots]))
            form_class[fid] = cid
            mapped += 1
        core = _core_name(model)
        sched = ir.ArchSchedule(
            spec=ir.MicroArchSpec(xml_name=model, canonical_name=core, family='arm'),
            port_names=port_names, classes=classes, form_class=form_class)
        serialize.write_vxarch(os.path.join(out, core + '.vxarch'), sched, 'llvm',
                               '-', isa='arm', source='llvm-sched')
        print("  %-22s %4d/%d formas (%d%%)  %d clases"
              % (core, mapped, len(forms), 100 * mapped // len(forms), len(classes)))


if __name__ == "__main__":
    main()
