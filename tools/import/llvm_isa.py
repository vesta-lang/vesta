#!/usr/bin/env python3
"""Extractor generico de la SINTAXIS de una ISA desde LLVM (`--dump-json`).

A diferencia de x86 (uops.info) y ARM (MRAS), para ISAs como RISC-V no hay una
fuente de sintaxis aparte: se extrae de los propios records `Instruction` de
LLVM.  Cada record da mnemonico (AsmString), operandos (In/OutOperandList con su
RegisterClass), extension (Predicates), memoria (mayLoad/mayStore) y flujo de
control (isCall/isReturn/isBranch) -> se construye la forma IR y se reutiliza el
MISMO pipeline (serialize.write_vxisa, FormID, overlay).

Extraccion UNICA via llvm-tblgen; el runtime NO depende de LLVM.

    python tools/import/llvm_isa.py <json> <ns> <isa> <salida.vxisa>
    #   ns  = Namespace LLVM (RISCV, ...)
    #   isa = etiqueta de la DB (riscv, ...)
"""
import hashlib
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ir           # noqa: E402
import serialize    # noqa: E402
from llvm_sched import _dname   # noqa: E402

# Predicado LLVM de extension -> nombre corto (RISC-V).  Se toma el PRIMERO que
# case; 'AOrZaamo'/'AOrZalrsc' -> 'A' (atomicos del set A).
_EXT_PRED = [
    (r'HasStdExtA', 'A'), (r'HasStdExtM', 'M'), (r'HasStdExtD', 'D'),
    (r'HasStdExtF', 'F'), (r'HasStdExtC', 'C'), (r'HasStdExtV', 'V'),
    (r'HasStdExtZ(\w+)', None),      # Zba, Zbb, Zbs, Zicsr, Zfh, ... (usa el grupo)
    (r'HasVendor(\w+)', None),       # extensiones de fabricante
]


def _extension(rec):
    """Nombre de extension a partir de los Predicates (I base si no hay)."""
    preds = [_dname(p) or '' for p in rec.get('Predicates', [])]
    for p in preds:
        for pat, name in _EXT_PRED:
            m = re.match(pat, p)
            if m:
                if name:
                    return name
                g = m.group(1)
                return 'Z' + g if pat.startswith('HasStdExtZ') else g
    return 'I'


def _opcode(rec):
    """Opcode (hex) desde los bits fijos de Inst (LSB primero).  Size 4 -> 7 bits;
    Size 2 (comprimida) -> 2 bits de cuadrante.  Bits variables -> se ignoran."""
    inst = rec.get('Inst')
    if not isinstance(inst, list):
        return '-'
    n = 7 if rec.get('Size', 4) != 2 else 2
    val = 0
    for i in range(min(n, len(inst))):
        b = inst[i]
        if b in (0, 1):
            val |= (b << i)
    return '%02x' % val


# clases de operando float/vector conocidas -> (set, ancho)
_FP = {'FPR16': 16, 'FPR32': 32, 'FPR64': 64, 'FPR128': 128}
_IMM_RX = re.compile(r'(\d+)')


def _classify_op(cls, regclasses):
    """(kind, width, register_set) de una clase de operando LLVM."""
    if cls in _FP:
        return 'reg', _FP[cls], cls
    if cls.startswith('FPR'):
        return 'reg', 0, 'FPR'
    if cls.startswith(('VR', 'VM', 'VRM')):
        return 'reg', 0, 'VR'                       # vectores (LMUL variable)
    if cls in regclasses or cls.startswith(('GPR', 'SP', 'GPRC')):
        return 'reg', 0, 'GPR'                       # xlen-agnostico
    # resto: inmediato / atributo (simm12, uimm5, frmarg, vtypei...)
    m = _IMM_RX.search(cls)
    return 'imm', (int(m.group(1)) if m else 0), '-'


def _operands(rec, regclasses):
    """Lista de ir.Operand: OutOperandList (escritos) + InOperandList (leidos).
    Anade un operando 'mem' sintetico si mayLoad/mayStore."""
    ops = []

    def add(dag, read, write):
        args = dag.get('args', []) if isinstance(dag, dict) else []
        for a in args:
            cls = a[0] if isinstance(a, list) else a
            cls = cls.get('def') if isinstance(cls, dict) else cls
            if not cls:
                continue
            kind, width, rs = _classify_op(cls, regclasses)
            ops.append(ir.Operand(idx=len(ops), kind=kind, width=width,
                                  read=read, write=write, implicit=False,
                                  suppressed=False, register_set=rs))

    add(rec.get('OutOperandList', {}), False, True)
    add(rec.get('InOperandList', {}), True, False)
    if (rec.get('mayLoad') or rec.get('mayStore')) \
            and not any(o.kind == 'mem' for o in ops):
        ops.append(ir.Operand(idx=len(ops), kind='mem', width=0,
                              read=bool(rec.get('mayLoad')),
                              write=bool(rec.get('mayStore')), implicit=False,
                              suppressed=False, register_set='-'))
    return ops


def _overlay(rec, mnem):
    """Etiquetas de overlay (mismo vocabulario que x86/ARM) desde los campos del
    record y el mnemonico."""
    props = set()
    mu = mnem.upper()
    if rec.get('isCall'):
        props.add('call')
    elif rec.get('isReturn'):
        props.add('ret')
    elif rec.get('isBranch') or mu in (
            'JAL', 'JALR', 'J', 'JR', 'C_J', 'C_JAL', 'C_JR', 'C_JALR',
            'C_BEQZ', 'C_BNEZ', 'TAIL', 'CALL'):
        props.add('branch')
    if mu.startswith('AMO') or mu.startswith(('AMOCAS',)):
        props.add('atomic')
    if mu.startswith('LR') or mu.startswith('SC'):
        props.add('ll_sc')
    if '.AQ' in mu or mu.endswith('_AQ') or mu.endswith('_AQRL'):
        props.add('mem_acquire')
    if '.RL' in mu or mu.endswith('_RL') or mu.endswith('_AQRL'):
        props.add('mem_release')
    if mu in ('FENCE', 'FENCE_TSO', 'SFENCE_VMA', 'SFENCE_W_INVAL',
              'SFENCE_INVAL_IR', 'SINVAL_VMA'):
        props.add('barrier')
    if mu in ('FENCE_I', 'CBO_FLUSH', 'CBO_INVAL', 'CBO_CLEAN'):
        props.add('serializing')
    if mu in ('ECALL', 'EBREAK', 'C_EBREAK', 'C_EBREAK', 'SRET', 'MRET',
              'WFI', 'DRET'):
        props.add('syscall')
    return props


def main():
    if len(sys.argv) < 5:
        sys.exit("uso: python llvm_isa.py <json> <ns> <isa> <salida.vxisa>")
    jsonp, ns, isa, out = sys.argv[1:5]
    d = json.load(open(jsonp, encoding='utf-8'))
    inst = d['!instanceof']
    regclasses = set(inst.get('RegisterClass', []))

    forms = []
    overlay = {}
    seen = set()
    n_raw = 0
    for name in inst.get('Instruction', []):
        rec = d[name]
        if rec.get('Namespace') != ns:
            continue
        if rec.get('isPseudo') or rec.get('isCodeGenOnly') \
                or rec.get('isAsmParserOnly') or not rec.get('Size'):
            continue
        asm = rec.get('AsmString', '') or ''
        mnem = re.split(r'[\t ]', asm.strip(), 1)[0]
        if not mnem or not re.match(r'^[A-Za-z]', mnem) or name.startswith('Pseudo'):
            continue
        n_raw += 1
        ext = _extension(rec)
        ops = _operands(rec, regclasses)
        rm, wm, hm, hi, dwf, drf = ir.derive_effects(ops)
        form = ir.InstrForm(
            iform=name, iclass=mnem.upper(), mnemonic=mnem,
            opcode=_opcode(rec), extension=ext,
            enc=ir.EncodingFeatures(isa_set=ext),
            operands=ops, read_mask=rm, write_mask=wm, has_mem=hm, has_imm=hi,
            writes_flags=dwf, reads_flags=drf, category=ext,
            summary='', asm_string=asm.replace('\t', ' '), url='')
        k = ir.form_key(form)
        if k in seen:
            continue                                 # dedup por identidad
        seen.add(k)
        forms.append(form)
        props = _overlay(rec, mnem)
        if props:
            overlay[name] = props

    forms.sort(key=ir.form_key)                       # FormID = indice denso
    h = hashlib.sha256(("llvm-isa %s %d %d" % (ns, n_raw, len(forms)))
                       .encode()).hexdigest()[:16]
    serialize.write_vxisa(out, forms, overlay, "llvm-19", h,
                          isa=isa, source="llvm-tblgen")
    ids = os.path.join(os.path.dirname(os.path.abspath(out)),
                       "instr_form_ids_%s.h" % isa)
    serialize.write_ids_header(ids, forms)
    print("[llvm_isa] ns=%s: %d records -> %d formas -> %s"
          % (ns, n_raw, len(forms), out))


if __name__ == "__main__":
    main()
