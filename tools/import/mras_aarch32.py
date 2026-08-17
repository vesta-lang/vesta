#!/usr/bin/env python3
"""Importador MRAS AArch32 (A32 + T32/Thumb) -- etapa SINTACTICA.

Fuente: los XML de ISA_AArch32_xml_A_profile-* (developer.arm.com).  Mismo
pipeline y mismas etapas que @ref mras_a64 (sintaxis -> semantica del
pseudocodigo -> serialize); reutiliza @ref mras_a64 (SynForm/_classify),
@ref mras_semantics y @ref mras_pseudocode sin duplicarlos.

Diferencias frente a A64, tratadas aqui:
  - cada <iclass> lleva isa="A32" o "T32" (dos codificaciones de la misma
    instruccion) -> se emite una forma por encoding, con la isa en la faceta.
  - el opcode tiene ANCHO VARIABLE: A32 = 32 bits; T32 = 16 o 32 (regdiagram
    form).  El patron de bits se construye del ancho real.
  - el asmtemplate lleva {<c>} (condicion) y {<q>} (qualifier del ensamblador):
    NO son operandos de valor -> se omiten.  La lectura de NZCV por la condicion
    la capta el pseudocodigo (ConditionPassed).

    python tools/import/mras_aarch32.py <dir_ISA_AArch32_xml> <dir_salida>
"""
import glob
import hashlib
import os
import re
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ir              # noqa: E402
import serialize       # noqa: E402
import mras_semantics  # noqa: E402
import mras_pseudocode  # noqa: E402
import mras_a64        # noqa: E402  (SynForm, SynOperand, _classify, _txt, ...)

# Tokens del asmtemplate que NO son operandos de valor en AArch32.
_SKIP_TOK = re.compile(r'^<(c|q)>$')
_BITDIFF = re.compile(r'([A-Za-z_]\w*)\s*==\s*([01]+)')


def _regdiagram(rd):
    """(bits[width], fields) del <regdiagram>, con ancho real (A32=32, T32=16/32
    segun @c form)."""
    # form: "32" (A32/T32-32), "16" (T32-16), "16x2" (dos halfwords = 32).
    form = rd.get('form', '32') if rd is not None else '32'
    if 'x' in form:
        a, b = form.split('x')
        width = int(a) * int(b)
    else:
        width = int(form) if form.isdigit() else 32
    if width not in (16, 32):
        width = 32
    bits = ['x'] * width
    fields = {}
    if rd is None:
        return bits, fields, width
    for box in rd.findall('box'):
        hibit = int(box.get('hibit'))
        w = int(box.get('width', '1'))
        name = box.get('name')
        if name:
            fields[name] = (hibit, w)
        vals = []
        for c in box.findall('c'):
            span = int(c.get('colspan', '1'))
            t = (c.text or '').strip()
            vals += [t if t in ('0', '1') else 'x'] * span
        if len(vals) < w:
            vals += ['x'] * (w - len(vals))
        for i in range(w):
            pos = hibit - i
            if 0 <= pos < width:
                bits[width - 1 - pos] = vals[i]
    return bits, fields, width


def _apply_bitdiffs(bits, fields, width, bitdiffs):
    out = list(bits)
    if not bitdiffs:
        return ''.join(out)
    for name, val in _BITDIFF.findall(bitdiffs):
        if name not in fields:
            continue
        hibit, w = fields[name]
        v = val.zfill(w)[-w:]
        for i, ch in enumerate(v):
            pos = hibit - i
            if 0 <= pos < width and ch in '01':
                out[width - 1 - pos] = ch
    return ''.join(out)


def parse_syntactic(path):
    try:
        root = ET.parse(path).getroot()
    except ET.ParseError:
        return []
    if root.tag != 'instructionsection' or root.get('type') not in ('instruction', 'alias'):
        return []
    is_alias = root.get('type') == 'alias'
    top = {d.get('key'): d.get('value') for d in root.findall('./docvars/docvar')}
    alias_of = ''
    if is_alias:
        at = root.find('.//aliasto')
        alias_of = (at.get('iformid') if at is not None else '') or ''
    brief = ''
    b = root.find('./desc/brief/para')
    if b is not None:
        brief = mras_a64._txt(b).strip()
    field_of = {}
    for e in root.findall('./explanations/explanation'):
        sym = e.find('symbol')
        acc = e.find('account')
        if sym is None:
            continue
        fld = acc.get('encodedin') if acc is not None else ''
        for en in (e.get('enclist') or '').replace(',', ' ').split():
            field_of[(en, sym.get('link'))] = fld or ''
    ps_op, ps_dec = {}, {}
    for ps in root.findall('.//ps'):
        nm = (ps.get('name') or '').rsplit('.', 1)[-1]
        if ps.get('secttype') == 'Operation':
            ps_op[nm] = mras_a64._txt(ps)
        else:
            ps_dec[nm] = ps_dec.get(nm, '') + '\n' + mras_a64._txt(ps)
    # En AArch32 el Operation suele ser UNICO y compartido por A32 y T32
    # (un 'if CurrentInstrSet()...'); si hay uno solo, sirve a todos los
    # encodings.  Idem el Decode (el puente var->campo es igual).
    def_op = next(iter(ps_op.values())) if len(ps_op) == 1 else ''
    def_dec = next(iter(ps_dec.values())) if len(ps_dec) == 1 else ''

    out = []
    for icl in root.findall('./classes/iclass'):
        dv = {d.get('key'): d.get('value') for d in icl.findall('./docvars/docvar')}
        isa = icl.get('isa') or dv.get('isa') or 'A32'
        instr_class = dv.get('instr-class') or top.get('instr-class') or 'general'
        rd = icl.find('regdiagram')
        base_bits, fields, width = _regdiagram(rd)
        psname = rd.get('psname', '') if rd is not None else ''
        ext = instr_class.upper()
        for enc in icl.findall('encoding'):
            ename = enc.get('name')
            edv = {d.get('key'): d.get('value')
                   for d in enc.findall('./docvars/docvar')}
            # mnemonico: encoding > iclass > alias > prefijo del nombre (ADDS_i_A1
            # -> ADDS).  El top no lo trae en AArch32.
            mnem = (top.get('alias_mnemonic') if is_alias else None) \
                or edv.get('mnemonic') or dv.get('mnemonic') \
                or top.get('mnemonic') or ename.split('_')[0]
            opcode = _apply_bitdiffs(base_bits, fields, width, enc.get('bitdiffs'))
            asm = enc.find('asmtemplate')
            ops = []
            depth = 0
            depth_opt = 0   # llaves: lo que se puede omitir al escribirla
            has_mem = False
            if asm is not None:
                for ch in list(asm):
                    if ch.tag == 'text':
                        s = ch.text or ''
                        if '[' in s:
                            has_mem = True
                        depth += s.count('[') - s.count(']')
                        depth_opt += s.count('{') - s.count('}')
                    elif ch.tag == 'a':
                        disp = mras_a64._txt(ch)
                        d = disp.strip()
                        if _SKIP_TOK.match(d) or mras_a64._QUALIFIER.match(d):
                            continue          # {<c>} condicion / {<q>} qualifier
                        field = field_of.get((ename, ch.get('link')), '')
                        kind, w, rs = mras_a64._classify(disp, field)
                        ops.append(mras_a64.SynOperand(
                            disp=disp, field=field, kind=kind, width=w,
                            register_set=rs, in_memory=depth > 0,
                            optional=depth_opt > 0))
            out.append(mras_a64.SynForm(
                mnemonic=mnem, encoding=ename, opcode=opcode,
                instr_class=instr_class, ext=ext, feature=isa, brief=brief,
                datatype='', psname=psname, has_mem=has_mem, is_alias=is_alias,
                alias_of=alias_of, decode_ps=ps_dec.get(ename, def_dec),
                operation_ps=ps_op.get(ename, def_op), operands=ops))
    return out


def main():
    if len(sys.argv) < 3:
        sys.exit("uso: python mras_aarch32.py <dir_ISA_AArch32_xml> <dir_salida>")
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    syns = []
    ninstr = 0
    for p in sorted(glob.glob(os.path.join(src, "*.xml"))):
        base = os.path.basename(p)
        if base.startswith(("index", "encodingindex", "fpsimdindex", "notice",
                            "shared_pseudocode", "constraint")):
            continue
        got = parse_syntactic(p)
        if got:
            ninstr += 1
            syns.extend(got)
    # Fase 2.5: aliases heredan el pseudocodigo de su base.
    ps_by_enc = {s.encoding: (s.decode_ps, s.operation_ps)
                 for s in syns if s.operation_ps}
    for s in syns:
        if s.is_alias and not s.operation_ps:
            pref = s.mnemonic + '_'
            base_enc = s.encoding[len(pref):] if s.encoding.startswith(pref) else ''
            got = ps_by_enc.get(base_enc)
            if got:
                s.decode_ps, s.operation_ps = got
    forms = [mras_semantics.to_irform(s) for s in syns]
    forms.sort(key=ir.form_key)
    overlay = {}
    for s in syns:
        props = mras_pseudocode.overlay_props(s.operation_ps)
        if props:
            overlay[s.encoding] = props
    ver = os.path.basename(src.rstrip("/\\"))
    m = re.search(r'(\d{4}-\d{2})', ver)
    date = m.group(1) if m else "?"
    h = hashlib.sha256(("mras-aarch32 %s %d %d" % (ver, ninstr, len(forms)))
                       .encode()).hexdigest()[:16]
    serialize.write_vxisa(os.path.join(out, "arm32.vxisa"), forms, overlay,
                          date, h, isa="arm32", source="arm-mras-aarch32")
    serialize.write_ids_header(os.path.join(out, "instr_form_ids_arm32.h"), forms)
    n_a32 = sum(1 for s in syns if s.feature == 'A32')
    n_t32 = sum(1 for s in syns if s.feature == 'T32')
    print("[mras_aarch32] %d instrucciones, %d formas (A32=%d T32=%d) -> %s"
          % (ninstr, len(forms), n_a32, n_t32, out))


if __name__ == "__main__":
    main()
