#!/usr/bin/env python3
"""Driver generico: genera un .vxarch por microarquitectura de los SchedModel de
LLVM, para CUALQUIER ISA (arm/x86).  Extraccion unica via llvm-tblgen.

  arm : mapea por (mnemonico, categoria) contra arm.vxisa (INT/FP/VEC/SVE/SME).
  x86 : mapea por mnemonico contra x86.vxisa (los mnemonicos x86 son inequivocos).

Los nombres de core casan los de uops.info para poder fusionar despues
(HaswellModel -> intel-haswell, Znver2Model -> amd-zen2...).

    python tools/import/gen_llvm.py <json> <arm|x86> <vxisa> <dir_salida>
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

# nombres LLVM -> nombres canonicos (para casar con uops.info y fusionar).
_X86_NAME = {
    'HaswellModel': 'intel-haswell', 'BroadwellModel': 'intel-broadwell',
    'SandyBridgeModel': 'intel-sandybridge', 'SkylakeClientModel': 'intel-skylake',
    'SkylakeServerModel': 'intel-skylake-x', 'IceLakeModel': 'intel-icelake',
    'AlderlakePModel': 'intel-alderlake-p', 'SapphireRapidsModel': 'intel-sapphirerapids',
    'AtomModel': 'intel-atom', 'SLMModel': 'intel-silvermont',
    'Znver1Model': 'amd-zen1', 'Znver2Model': 'amd-zen2', 'Znver3Model': 'amd-zen3',
    'Znver4Model': 'amd-zen4', 'BdVer2Model': 'amd-bdver2', 'BtVer2Model': 'amd-btver2',
}
_SKIP = {'NoSchedModel', 'GenericModel', 'GenericPostRAModel'}

# core canonico -> Name de la CPU LLVM concreta (para tomar SUS features, no el
# union del SchedModel que a veces comparten varias CPU distintas).
_X86_CPU = {
    'intel-haswell': 'haswell', 'intel-broadwell': 'broadwell',
    'intel-sandybridge': 'sandybridge', 'intel-skylake': 'skylake',
    'intel-skylake-x': 'skylake-avx512', 'intel-icelake': 'icelake-client',
    'intel-alderlake-p': 'alderlake', 'intel-sapphirerapids': 'sapphirerapids',
    'intel-atom': 'atom', 'intel-silvermont': 'silvermont',
    'amd-zen1': 'znver1', 'amd-zen2': 'znver2', 'amd-zen3': 'znver3',
    'amd-zen4': 'znver4', 'amd-bdver2': 'bdver2', 'amd-btver2': 'btver2',
}


# RISC-V: SchedModel -> nombre de core canonico (LLVM es la unica fuente, no hay
# fusion; se nombra por el core representativo).
_RISCV_NAME = {
    'RocketModel': 'rocket', 'SiFive7Model': 'sifive-7-series',
    'SiFiveP400Model': 'sifive-p450', 'SiFiveP600Model': 'sifive-p670',
    'SyntacoreSCR1Model': 'syntacore-scr1', 'SyntacoreSCR3RV32Model': 'syntacore-scr3-rv32',
    'SyntacoreSCR3RV64Model': 'syntacore-scr3-rv64', 'XiangShanNanHuModel': 'xiangshan-nanhu',
}


def _core_name(model, isa):
    if isa == 'x86':
        return _X86_NAME.get(model, 'x86-' + re.sub(r'Model$', '', model).lower())
    if isa == 'riscv':
        return _RISCV_NAME.get(model, re.sub(r'Model$', '', model).lower())
    s = re.sub(r'Model$', '', model)
    s = re.sub(r'(?<=[a-z])(?=[A-Z])', '-', s)
    return s.lower() + '-llvm'


def main():
    if len(sys.argv) < 5:
        sys.exit("uso: python gen_llvm.py <json> <arm|x86> <vxisa> <salida>")
    jsonp, isa, vxisa, out = sys.argv[1:5]
    os.makedirs(out, exist_ok=True)
    d = json.load(open(jsonp, encoding='utf-8'))
    forms = database.load_vxisa(vxisa)
    if isa == 'arm':
        cats_of = lambda ext: llvm_sched._EXT_CATS.get(ext, ('INT',))
    elif isa == 'x86':
        cats_of = lambda ext: (llvm_sched.x86_bucket(ext),)
    else:
        cats_of = lambda ext: (isa.upper(),)          # riscv: categoria unica
    models = [m for m in d['!instanceof'].get('SchedMachineModel', []) if m not in _SKIP]
    print("[gen_llvm] isa=%s, %d modelos" % (isa, len(models)))
    for model in models:
        core = _core_name(model, isa)
        feats = None
        if isa == 'x86':
            cpu = _X86_CPU.get(core)
            feats = llvm_sched.processor_features(d, cpu) if cpu else None
        cost, port_names = llvm_sched.build_model(d, model, isa, feats)
        if not cost:
            continue
        classes, class_key = [], {}
        form_class = [-1] * (max(forms) + 1)
        mapped = 0
        for fid in sorted(forms):
            fm = forms[fid]
            mn = fm['iclass'].upper()
            got = None
            for cat in cats_of(fm['ext']):
                got = cost.get((mn, cat))
                if got:
                    break
            if got is None and fm.get('category', '').startswith('alias:'):
                base = fm['category'][6:].split('_')[0].upper()
                for cat in cats_of(fm['ext']):
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
        if isa == 'arm':
            fam = 'arm'
        elif isa == 'riscv':
            fam = 'riscv'
        else:
            fam = 'amd' if core.startswith('amd') else 'intel'
        sched = ir.ArchSchedule(
            spec=ir.MicroArchSpec(xml_name=model, canonical_name=core, family=fam),
            port_names=port_names, classes=classes, form_class=form_class)
        serialize.write_vxarch(os.path.join(out, core + '.vxarch'), sched, 'llvm',
                               '-', isa=isa, source='llvm-sched')
        print("  %-24s %5d/%d (%d%%)  %d clases"
              % (core, mapped, len(forms), 100 * mapped // len(forms), len(classes)))


if __name__ == "__main__":
    main()
