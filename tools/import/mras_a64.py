#!/usr/bin/env python3
"""Importador MRAS A64 -- etapa SINTACTICA (ARM Machine Readable Architecture).

Fuente: los XML por-instruccion de ISA_A64_xml_A_profile-* (developer.arm.com,
Exploration tools).  EXTRACCION UNICA -> nuestro formato serializado; el
compilador y el sitio solo consumen eso, cero dependencia de ARM/LLVM en runtime
(mismo modelo que uops.info para x86).

RESPONSABILIDAD UNICA de este modulo: extraer la FORMA (sintaxis) de cada
<encoding> -- mnemonico, opcode (patron de bits), operandos con su BANCO y ANCHO
sacados de la METADATA estructural del XML (campo @c encodedin: Rd/Zn/Pg/Vn ->
GPR/SVE_Z/predicado/SIMD; la letra del token solo da el ancho).  NO decide
lee/escribe/flags: eso es responsabilidad de la etapa @ref mras_semantics
(Fase 1 heuristica; Fase 2 = analisis del pseudocodigo).  Asi la fidelidad
semantica mejora sin mezclar heuristicas dentro del importador.

Pipeline: MRAS XML -> [este parser] -> SynForm -> [mras_semantics] ->
          derive_effects -> serialize.

    python tools/import/mras_a64.py <dir_ISA_A64_xml> <dir_salida>
"""
import glob
import hashlib
import os
import re
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field as dfield

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ir              # noqa: E402
import serialize       # noqa: E402
import mras_semantics  # noqa: E402

# Cualificadores del operando anterior (arrangement / tamano de elemento SVE/
# SIMD): <T>, <Ts>, <Tb>, <Ta>, <Tsz>...  No son operandos con valor propio.
_QUALIFIER = re.compile(r'^<T[a-z]{0,3}>$')
# Ancho (bits) por letra del token de display (la letra ES la convencion ARM de
# ancho; el BANCO se toma del campo encodedin, mas estructural).
_WIDTH_BY_LETTER = {'W': 32, 'X': 64, 'B': 8, 'H': 16, 'S': 32, 'D': 64,
                    'Q': 128, 'V': 128, 'Z': 0, 'P': 0}
# Banco por primera letra del campo encodedin (metadata estructural del XML).
_BANK_BY_FIELD = {'R': 'GPR', 'Z': 'SVE_Z', 'P': 'SVE_P', 'V': 'SIMD'}


@dataclass
class SynOperand:
    """Operando SINTACTICO: banco/ancho/tipo sin marcar aun lee/escribe."""
    disp: str                 # <Wd>, <Xn|SP>, #<imm>, <label>...
    field: str                # encodedin (Rd, Zn, Pg, imm12, ...)
    kind: str                 # reg | imm | relbr | mem | ?
    width: int                # bits (0 si vector escalable / no aplica)
    register_set: str         # GPR32|GPR64|GPR64_SP|SVE_Z|SVE_P|SIMD128|...
    in_memory: bool           # aparece dentro de [ ... ]


@dataclass
class SynForm:
    """Forma SINTACTICA de un <encoding> (sin semantica de lee/escribe)."""
    mnemonic: str
    encoding: str
    opcode: str               # patron de 32 bits (identidad del encoding)
    instr_class: str          # general|sve|sve2|advsimd|float|system|mortlach...
    ext: str                  # instr-class canonizado (SME/SME2/...) en mayus
    feature: str              # FEAT_* si el XML lo da
    brief: str
    datatype: str             # 32|64|... si el encoding lo declara
    psname: str               # A64.memory.* / A64.dpreg.* ... (senal estructural)
    has_mem: bool             # hay acceso a memoria [ ... ]
    is_alias: bool = False    # es un alias (MOV, CMP, NOP, RET...) de una base
    alias_of: str = ""        # iform de la instruccion base (si is_alias)
    decode_ps: str = ""       # pseudocodigo Decode (mapea vars ASL a campos)
    operation_ps: str = ""    # pseudocodigo Operation (efectos: X()/Mem/PSTATE)
    operands: list = dfield(default_factory=list)   # list[SynOperand]


def _txt(el):
    return ''.join(el.itertext())


def _regdiagram(rd):
    """(bits[32], fields) del <regdiagram>: bits '0'/'1' fijos, 'x' variables;
    fields = {nombre_campo: (hibit, width)} para poder aplicar los bitdiffs."""
    bits = ['x'] * 32
    fields = {}
    for box in rd.findall('box'):
        hibit = int(box.get('hibit'))
        width = int(box.get('width', '1'))
        name = box.get('name')
        if name:
            fields[name] = (hibit, width)
        vals = []
        for c in box.findall('c'):
            span = int(c.get('colspan', '1'))
            t = (c.text or '').strip()
            vals += [t if t in ('0', '1') else 'x'] * span
        if len(vals) < width:
            vals += ['x'] * (width - len(vals))
        for i in range(width):
            pos = hibit - i
            if 0 <= pos < 32:
                bits[31 - pos] = vals[i]
    return bits, fields


_BITDIFF = re.compile(r'([A-Za-z_]\w*)\s*==\s*([01]+)')


def _apply_bitdiffs(bits, fields, bitdiffs):
    """Fija en el patron los bits que distinguen ESTE encoding (attr bitdiffs,
    p.ej. 'sf == 0 && op == 1').  Solo igualdades con valor binario; ignora !=
    y condiciones no pinables (quedan 'x')."""
    out = list(bits)
    if not bitdiffs:
        return ''.join(out)
    for name, val in _BITDIFF.findall(bitdiffs):
        if name not in fields:
            continue
        hibit, width = fields[name]
        v = val.zfill(width)[-width:] if len(val) <= width else val[-width:]
        for i, ch in enumerate(v):
            pos = hibit - i
            if 0 <= pos < 32 and ch in '01':
                out[31 - pos] = ch
    return ''.join(out)


def _classify(disp, field):
    """(kind, width, register_set) SOLO desde estructura: el BANCO viene del
    campo @c encodedin; la letra del display solo aporta el ancho.  Sin prosa."""
    d = disp.strip().strip('{}').strip()
    dl = d.lower()
    f = (field or '').lstrip('([').strip()
    fl = f[:1].upper() if f else ''
    # inmediatos / etiquetas por el nombre del campo (imm*, uimm*, simm*, pimm*).
    if d.startswith('#') or re.match(r'^[a-z]*imm', f.lower()) or 'imm' in dl:
        return ('imm', 0, '-')
    if dl == '<label>' or f.lower() in ('label', 'imm19', 'imm26', 'imm14') \
            and 'label' in dl:
        return ('relbr', 0, '-')
    # registros: banco del campo, ancho de la letra del display.
    letterm = re.match(r'^<([A-Za-z])', d)
    letter = letterm.group(1).upper() if letterm else ''
    width = _WIDTH_BY_LETTER.get(letter, 0)
    bank = _BANK_BY_FIELD.get(fl)
    if bank is None and letter in ('W', 'X'):
        bank = 'GPR'
    if bank is None and letter in ('B', 'H', 'S', 'D', 'Q', 'V'):
        bank = 'SIMD'
    if bank == 'GPR':
        rs = 'GPR64' if width == 64 else 'GPR32'
        if '|SP' in d or 'OrSP' in d:
            rs += '_SP'
        return ('reg', width, rs)
    if bank == 'SIMD':
        return ('reg', width or 128, 'SIMD%d' % (width or 128))
    if bank == 'SVE_Z':
        return ('reg', 0, 'SVE_Z')
    if bank == 'SVE_P':
        return ('reg', 0, 'SVE_P')
    if letter in _WIDTH_BY_LETTER:               # ultimo recurso: solo letra
        return ('reg', width, 'SIMD%d' % width if width else 'REG')
    return ('?', 0, '-')


def parse_syntactic(path):
    """list[SynForm] de un XML de instruccion (o [] si no aplica)."""
    try:
        root = ET.parse(path).getroot()
    except ET.ParseError:
        return []
    if root.tag != 'instructionsection' or root.get('type') not in ('instruction', 'alias'):
        return []
    is_alias = root.get('type') == 'alias'
    top = {d.get('key'): d.get('value') for d in root.findall('./docvars/docvar')}
    # en un alias el mnemonico visible es alias_mnemonic (MOV), no el base (MOVN).
    mnem = (top.get('alias_mnemonic') if is_alias else None) \
        or top.get('mnemonic') or root.get('id')
    alias_of = ''
    if is_alias:
        at = root.find('.//aliasto')
        alias_of = (at.get('iformid') if at is not None else '') or ''
    # pseudocodigo por encoding: el name de cada <ps> acaba en el nombre del
    # encoding; secttype=Operation es el efecto, el resto es Decode (var->campo).
    ps_op, ps_dec = {}, {}
    for ps in root.findall('.//ps'):
        nm = (ps.get('name') or '').rsplit('.', 1)[-1]
        if ps.get('secttype') == 'Operation':
            ps_op[nm] = _txt(ps)
        else:
            ps_dec[nm] = ps_dec.get(nm, '') + '\n' + _txt(ps)
    brief = ''
    b = root.find('./desc/brief/para')
    if b is not None:
        brief = _txt(b).strip()
    # explicaciones: (encoding, link) -> encodedin (el campo del regdiagram).
    field_of = {}
    for e in root.findall('./explanations/explanation'):
        sym = e.find('symbol')
        acc = e.find('account')
        if sym is None:
            continue
        fld = acc.get('encodedin') if acc is not None else ''
        # enclist es separado por COMAS ("A, B, C"), no por espacios.
        for en in (e.get('enclist') or '').replace(',', ' ').split():
            field_of[(en, sym.get('link'))] = fld or ''

    out = []
    for icl in root.findall('./classes/iclass'):
        dv = {d.get('key'): d.get('value') for d in icl.findall('./docvars/docvar')}
        instr_class = dv.get('instr-class') or top.get('instr-class') or 'general'
        feature = dv.get('feature', '') or top.get('feature', '')
        rd = icl.find('regdiagram')
        base_bits, fields = _regdiagram(rd) if rd is not None else (['x'] * 32, {})
        psname = rd.get('psname', '') if rd is not None else ''
        ext = {'mortlach': 'SME', 'mortlach2': 'SME2'}.get(
            instr_class, instr_class).upper()
        for enc in icl.findall('encoding'):
            ename = enc.get('name')
            opcode = _apply_bitdiffs(base_bits, fields, enc.get('bitdiffs'))
            edv = {d.get('key'): d.get('value')
                   for d in enc.findall('./docvars/docvar')}
            asm = enc.find('asmtemplate')
            ops = []
            depth = 0
            has_mem = False
            idx = 0
            if asm is not None:
                for ch in list(asm):
                    if ch.tag == 'text':
                        s = ch.text or ''
                        if '[' in s:
                            has_mem = True
                        depth += s.count('[') - s.count(']')
                    elif ch.tag == 'a':
                        disp = _txt(ch)
                        if _QUALIFIER.match(disp.strip()):
                            continue              # arrangement -> cualificador
                        field = field_of.get((ename, ch.get('link')), '')
                        kind, width, rs = _classify(disp, field)
                        ops.append(SynOperand(disp=disp, field=field, kind=kind,
                                              width=width, register_set=rs,
                                              in_memory=depth > 0))
                        idx += 1
            out.append(SynForm(
                mnemonic=mnem, encoding=ename, opcode=opcode,
                instr_class=instr_class, ext=ext, feature=feature, brief=brief,
                datatype=edv.get('datatype', ''), psname=psname,
                has_mem=has_mem, is_alias=is_alias, alias_of=alias_of,
                decode_ps=ps_dec.get(ename, ''),
                operation_ps=ps_op.get(ename, ''),
                operands=ops))
    return out


def main():
    if len(sys.argv) < 3:
        sys.exit("uso: python mras_a64.py <dir_ISA_A64_xml> <dir_salida>")
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    syns = []
    ninstr = 0
    for p in sorted(glob.glob(os.path.join(src, "*.xml"))):
        base = os.path.basename(p)
        if base.startswith(("index", "encodingindex", "fpsimdindex", "notice",
                            "shared_pseudocode")):
            continue
        got = parse_syntactic(p)
        if got:
            ninstr += 1
            syns.extend(got)
    # Fase 2.5: un alias no trae su pseudocodigo propio -> HEREDA el de su base.
    # El encoding del alias es "<ALIAS_MNEMONIC>_<encoding_base>" (p.ej.
    # CMP_SUBS_32S_addsub_ext -> base SUBS_32S_addsub_ext); asi CMP toma la
    # semantica REAL de SUBS (escribe NZCV) sin heuristica.
    ps_by_enc = {s.encoding: (s.decode_ps, s.operation_ps)
                 for s in syns if s.operation_ps}
    inherited = 0
    for s in syns:
        if s.is_alias and not s.operation_ps:
            pref = s.mnemonic + '_'
            base_enc = s.encoding[len(pref):] if s.encoding.startswith(pref) else ''
            got = ps_by_enc.get(base_enc)
            if got:
                s.decode_ps, s.operation_ps = got
                inherited += 1
    # Etapa semantica: SynForm -> ir.InstrForm (Fase 2 pseudocodigo; Fase 1
    # heuristica solo como fallback).
    forms = [mras_semantics.to_irform(s) for s in syns]
    forms.sort(key=ir.form_key)                   # FormID = indice denso
    #   overlay (barrera/serializante/atomica/ll_sc/mem_acquire-release/
    # branch/call/ret/syscall) DERIVADO del pseudocodigo, keyed por encoding.
    import mras_pseudocode
    overlay = {}
    for s in syns:
        props = mras_pseudocode.overlay_props(s.operation_ps)
        if props:
            overlay[s.encoding] = props
    ver = os.path.basename(src.rstrip("/\\"))
    m = re.search(r'(\d{4}-\d{2})', ver)
    date = m.group(1) if m else "?"
    h = hashlib.sha256(("mras-a64 %s %d %d" % (ver, ninstr, len(forms)))
                       .encode()).hexdigest()[:16]
    serialize.write_vxisa(os.path.join(out, "arm.vxisa"), forms, overlay, date, h,
                          isa="arm", source="arm-mras-a64")
    serialize.write_ids_header(os.path.join(out, "instr_form_ids_arm.h"), forms)
    print("[mras_a64] %d instrucciones, %d formas (encodings) -> %s"
          % (ninstr, len(forms), out))


if __name__ == "__main__":
    main()
