#!/usr/bin/env python3
"""Importador de coste desde los SchedModel de LLVM (TableGen JSON).

EXTRACCION UNICA via llvm-tblgen: `llvm-tblgen --dump-json AArch64.td` produce un
JSON con TODOS los records; este modulo lo resuelve al coste por-instruccion de
un subtarget (NeoverseN1/N2/V1/V2, Cortex-A/X...) y lo escribe en NUESTRO formato
(<core>.vxarch), sin dependencia de LLVM en runtime (mismo modelo que uops.info /
SWOG).  Los SchedModel de LLVM son COMPLETOS (CompleteModel=1): cubren SVE/SVE2 y
buena parte de lo que las SWOG no cronometran.

Resolucion (replica del SubtargetEmitter, casos principales):
  instruccion --(InstRW override | Instruction.SchedRW)--> SchedWrites
  SchedWrite  --(SchedWriteRes directo | SchedAlias | WriteRes | WriteSequence)-->
              (latency, uops, ProcResources[])
Se agrega por (mnemonico, categoria) -- INT/FP/VEC/SVE/SME -- y se mapea a
nuestras formas por (iclass, categoria compatible), como el importador SWOG.

    python tools/import/llvm_sched.py <aarch64.json> <Model> <arm.vxisa> <out> <core>
    python tools/import/llvm_sched.py a64.json NeoverseN2Model arm.vxisa out/ neoverse-n2-llvm
"""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ir          # noqa: E402
import serialize   # noqa: E402
import database    # noqa: E402

# extension de la forma -> categorias LLVM admisibles (fallback en orden).
_EXT_CATS = {
    'GENERAL': ('INT',), 'SYSTEM': ('INT',),
    'ADVSIMD': ('VEC', 'FP'), 'FLOAT': ('FP', 'VEC'), 'FPSIMD': ('FP', 'VEC'),
    'SVE': ('SVE',), 'SVE2': ('SVE',), 'SME': ('SME', 'SVE'), 'SME2': ('SME', 'SVE'),
}


def _dname(x):
    return x.get('def') if isinstance(x, dict) else x


def _clean_port(pr):
    """Normaliza el nombre de un ProcResource de LLVM al esquema de uops.info,
    para que uops.info y LLVM se COMPLEMENTEN al fusionar (mismo puerto = mismo
    nombre).  Intel: <uarch>Port0156 -> p0156.  AMD: <uarch>FPU01 -> FP01,
    <uarch>ALU03 -> ALU03, <uarch>AGU -> AGU.  ARM: N2UnitV0 -> V0."""
    m = re.match(r'^\w*?Port([0-9]+)$', pr)
    if m:
        return 'p' + m.group(1)                # Intel: HWPort06 -> p06
    if re.search(r'FPDivider$', pr):
        return 'pfpdiv'
    if re.search(r'Divider$', pr):
        return 'pdiv'
    if re.search(r'Multiplier$', pr):
        return 'pmul'
    m = re.match(r'^\w*?FPU([0-9]*)$', pr)
    if m:
        return 'FP' + m.group(1)               # AMD: Zn2FPU01 -> FP01
    m = re.match(r'^\w*?(ALU|AGU|IntMul|IntDiv|Load|Store)([0-9]*)$', pr)
    if m:
        return m.group(1) + m.group(2)         # AMD: Zn2ALU03 -> ALU03
    m = re.match(r'^\w*?Unit(.+)$', pr)
    if m:
        return m.group(1)                      # ARM: N2UnitV0 -> V0
    return pr


def _superclasses(rec):
    return rec.get('!superclasses', [])


def _mnemonic(asm):
    """Primer token del AsmString -> mnemonico en mayusculas ('add\\t..' -> ADD)."""
    if not asm:
        return None
    m = re.match(r'^([A-Za-z][A-Za-z0-9]*)', asm)
    return m.group(1).upper() if m else None


def _x86_cat(rec):
    """Nivel de ISA de una instruccion x86 (por sus predicados): BASE/SSE/AVX/
    AVX512/MMX.  Evita que una forma AVX512 reciba coste en un core sin AVX512:
    (mnem, AVX512) solo existe si ese core modela AVX512."""
    preds = ' '.join(_dname(p) or '' for p in rec.get('Predicates', []))
    if 'AVX512' in preds or 'AVX10' in preds:
        return 'AVX512'
    if 'AVX' in preds:
        return 'AVX'
    if 'SSE' in preds:
        return 'SSE'
    if 'MMX' in preds or '3DNow' in preds:
        return 'MMX'
    return 'BASE'


def x86_bucket(ext):
    """Extension de NUESTRA forma x86 -> mismo bucket que _x86_cat."""
    e = (ext or '').upper()
    if 'AVX512' in e or 'EVEX' in e or 'AVX10' in e:
        return 'AVX512'
    if 'AVX' in e or 'FMA' in e:
        return 'AVX'
    if 'SSE' in e:
        return 'SSE'
    if 'MMX' in e or '3DNOW' in e:
        return 'MMX'
    return 'BASE'


def _category(d, rec):
    """INT / FP / VEC / SVE / SME segun predicados y clases de registro.

    SME solo si toca la matriz ZA (MatrixOp/MPR/ZPRxN...); 'HasSVEorSME' es una
    instruccion SVE usable en modo streaming -> categoria SVE, no SME."""
    preds = ' '.join(_dname(p) or '' for p in rec.get('Predicates', []))
    ops = json.dumps(rec.get('InOperandList', '')) + json.dumps(rec.get('OutOperandList', ''))
    if 'MatrixOp' in ops or '"MPR' in ops or 'ZPR_4b' in ops or 'MatrixIndex' in ops \
            or 'TileOp' in ops or ('HasSME' in preds and 'SVEorSME' not in preds):
        return 'SME'
    if 'SVE' in preds or 'ZPR' in ops or 'PPR' in ops:
        return 'SVE'
    if 'NEON' in preds or re.search(r'"(V64|V128|FPR128|FPR64|FPR8|FPR16)"', ops):
        return 'VEC'
    if 'FPR32' in ops or 'FPR64' in ops or 'HasFP' in preds or 'FullFP16' in preds:
        return 'FP'
    return 'INT'


def model_features(d, model):
    """Conjunto (transitivo, expandiendo Implies) de features del procesador que
    usa este SchedMachineModel.  Es la base de la DB de features por core y del
    filtro de soporte de ISA."""
    inst = d['!instanceof']
    feats = set()
    for cls in ('Processor', 'ProcessorModel'):
        for n in inst.get(cls, []):
            r = d[n]
            if _dname(r.get('SchedModel')) == model:
                for f in r.get('Features', []):
                    nm = _dname(f)
                    if nm:
                        feats.add(nm)
    return _expand_implies(d, feats)


def _expand_implies(d, feats):
    """Cierre transitivo de Implies sobre un conjunto de features."""
    feats = set(feats)
    changed = True
    while changed:
        changed = False
        for f in list(feats):
            r = d.get(f)
            if r:
                for imp in r.get('Implies', []):
                    nm = _dname(imp)
                    if nm and nm not in feats:
                        feats.add(nm)
                        changed = True
    return feats


def processor_features(d, cpu):
    """Features (transitivas) del Processor cuyo Name es @c cpu.  Es lo correcto
    cuando VARIAS CPU comparten SchedModel (p.ej. haswell y knl comparten
    HaswellModel pero solo knl tiene AVX512): hay que usar la CPU concreta."""
    inst = d['!instanceof']
    for n in inst.get('Processor', []):
        r = d[n]
        if r.get('Name') == cpu:
            return _expand_implies(d, (_dname(f) for f in r.get('Features', [])))
    return set()


def _x86_supported(cat, feats):
    """El core (por sus features) soporta ese nivel de ISA?  Evita dar coste a
    formas AVX512 en cores que no lo implementan (Haswell modela WriteFAdd
    generico, del que heredarian coste las AVX512 sin este filtro)."""
    if cat == 'AVX512':
        return any('AVX512' in f for f in feats)
    if cat == 'AVX':
        return any(f in feats for f in ('FeatureAVX', 'FeatureAVX2'))
    if cat == 'SSE':
        return any('SSE' in f for f in feats)
    if cat == 'MMX':
        return 'FeatureMMX' in feats
    return True


def build_model(d, model, isa='arm', feats=None):
    """Devuelve (cost{(mnem,cat):(lat,uops,ports[])}, port_names[]).

    @c isa='arm' usa categorias INT/FP/VEC/SVE/SME; 'x86' agrupa por (mnem,nivel
    de ISA: BASE/SSE/AVX/AVX512).  @c feats (features de la CPU concreta) filtra
    las formas cuyo nivel de ISA no soporta el core; si None se toma el union del
    modelo (menos preciso cuando varias CPU lo comparten)."""
    inst = d['!instanceof']
    # tablas del modelo
    alias = {}                                 # SchedWrite generico -> write del modelo
    writeres = {}                              # WriteType -> record (WriteRes generico)
    for n in inst.get('SchedAlias', []):
        r = d[n]
        if _dname(r.get('SchedModel')) == model and r.get('MatchRW') and r.get('AliasRW'):
            alias[_dname(r['MatchRW'])] = _dname(r['AliasRW'])
    for n in inst.get('WriteRes', []):
        r = d[n]
        if _dname(r.get('SchedModel')) == model and r.get('WriteType'):
            writeres[_dname(r['WriteType'])] = r

    port_index = {}
    port_names = []

    def port_id(pr):
        nm = _clean_port(pr)
        if nm not in port_index:
            port_index[nm] = len(port_names)
            port_names.append(nm)
        return port_index[nm]

    seen = {}

    def write_cost(w, depth=0):
        """(lat, uops, [ProcResource names]) de un SchedWrite en este modelo."""
        if depth > 8:
            return None
        if w in seen:
            return seen[w]
        rec = d.get(w)
        res = None
        if rec is not None:
            sc = _superclasses(rec)
            if 'WriteSequence' in sc:
                lat = uops = 0
                ports = []
                rep = int(rec.get('Repeat', 1) or 1)
                for sub in rec.get('Writes', []):
                    c = write_cost(_dname(sub), depth + 1)
                    if c:
                        lat += c[0] * rep
                        uops += c[1] * rep
                        ports += c[2] * rep
                res = (lat, uops, ports)
            elif ('SchedWriteRes' in sc or 'ProcWriteResources' in sc) \
                    and _dname(rec.get('SchedModel')) == model and 'Latency' in rec:
                res = (rec['Latency'], rec.get('NumMicroOps', 1),
                       [_dname(p) for p in rec.get('ProcResources', [])])
        if res is None and w in alias:
            res = write_cost(alias[w], depth + 1)
        if res is None and w in writeres:
            r = writeres[w]
            res = (r['Latency'], r.get('NumMicroOps', 1),
                   [_dname(p) for p in r.get('ProcResources', [])])
        seen[w] = res
        return res

    # InstRW del modelo: concretos + regex
    concrete = {}        # instr name -> [write SchedWrites]
    regexes = []         # (compiled, [writes])
    for n in inst.get('InstRW', []):
        r = d[n]
        if _dname(r.get('SchedModel')) != model:
            continue
        writes = [_dname(w) for w in r.get('OperandReadWrites', [])
                  if 'SchedWrite' in _superclasses(d.get(_dname(w), {}))]
        dag = r.get('Instrs') or {}
        op = _dname(dag.get('operator')) if isinstance(dag, dict) else None
        for a in (dag.get('args', []) if isinstance(dag, dict) else []):
            val = a[0] if isinstance(a, list) else a
            if op == 'instregex' and isinstance(val, str):
                try:
                    regexes.append((re.compile(val), writes))
                except re.error:
                    pass
            else:
                nm = _dname(val)
                if nm:
                    concrete[nm] = writes

    def writes_for(name, rec):
        if name in concrete:
            return concrete[name]
        for rx, ws in regexes:
            if rx.search(name):
                return ws
        return [_dname(w) for w in rec.get('SchedRW', [])
                if 'SchedWrite' in _superclasses(d.get(_dname(w), {}))]

    want_ns = 'AArch64' if isa == 'arm' else 'X86'
    if isa == 'x86' and feats is None:
        feats = model_features(d, model)             # fallback (union del modelo)
    cost = {}
    for name in inst.get('Instruction', []):
        rec = d[name]
        if rec.get('Namespace') != want_ns:
            continue
        asm = rec.get('AsmString', '')
        mn = _mnemonic(asm)
        if not mn or 'Pseudo' in _superclasses(rec):
            continue
        cat = _category(d, rec) if isa == 'arm' else _x86_cat(rec)
        if isa == 'x86' and not _x86_supported(cat, feats):
            continue                                 # ISA no soportada por el core
        lat = uops = 0
        ports = []
        for w in writes_for(name, rec):
            c = write_cost(w)
            if c:
                lat = max(lat, c[0])
                uops += c[1]
                ports += c[2]
        if lat == 0 and not ports:
            continue
        key = (mn, cat)
        cand = (lat, max(uops, 1), [port_id(p) for p in ports])
        cur = cost.get(key)
        # representativa: preferir la instancia CON puertos (mas informacion) y,
        # dentro de esas, la de MENOR latencia (forma reg-reg tipica).  latencia
        # y puertos vienen SIEMPRE de la misma instancia (no se mezclan).
        if cur is None or (bool(cand[2]) > bool(cur[2])) \
                or (bool(cand[2]) == bool(cur[2]) and cand[0] < cur[0]):
            cost[key] = cand
    return cost, port_names


def main():
    if len(sys.argv) < 6:
        sys.exit("uso: python llvm_sched.py <json> <Model> <vxisa> <out> <core>")
    jsonp, model, vxisa, out, core = sys.argv[1:6]
    os.makedirs(out, exist_ok=True)
    d = json.load(open(jsonp, encoding='utf-8'))
    cost, port_names = build_model(d, model)
    forms = database.load_vxisa(vxisa)

    classes = []
    class_key = {}
    form_class = [-1] * (max(forms) + 1)

    def lookup(mn, ext):
        for cat in _EXT_CATS.get(ext, ('INT',)):
            c = cost.get((mn, cat))
            if c is not None:
                return c
        return None

    mapped = 0
    for fid in sorted(forms):
        fm = forms[fid]
        mn = fm['iclass'].upper()
        c = lookup(mn, fm['ext'])
        if c is None and fm['category'].startswith('alias:'):
            base = fm['category'][6:].split('_')[0].upper()
            c = lookup(base, fm['ext'])
        if c is None:
            continue
        lat, uops, pslots = c
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

    sched = ir.ArchSchedule(
        spec=ir.MicroArchSpec(xml_name=model, canonical_name=core, family='arm'),
        port_names=port_names, classes=classes, form_class=form_class)
    serialize.write_vxarch(os.path.join(out, core + '.vxarch'), sched, 'llvm', '-',
                           isa='arm', source='llvm-sched')
    print("[llvm_sched] %s (%s): %d (mnem,cat) con coste, %d/%d formas, %d clases, "
          "puertos=%s" % (core, model, len(cost), mapped, len(forms), len(classes),
                          ",".join(port_names)))


if __name__ == "__main__":
    main()
