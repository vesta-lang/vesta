#!/usr/bin/env python3
"""Etapa SEMANTICA del pipeline ARM: SynForm -> ir.InstrForm.

RESPONSABILIDAD UNICA: decidir, para cada operando, si se LEE y/o ESCRIBE, y si
la instruccion toca NZCV / memoria.  El importador @ref mras_a64 solo aporta la
sintaxis (banco/ancho/tipo); aqui se anade la semantica y se llama a
@ref ir.derive_effects.  Separar esta etapa permite mejorar la fidelidad sin
tocar el importador ni el serializer.

FASE 1 (este modulo, ahora): HEURISTICA -- convencion load/store, forma-S para
NZCV, op0 = destino en data-processing.  Es un PUNTO DE PARTIDA, no definitivo:
atomicos (LDADD/LDCLR/LDSET/SWP/CAS/LDAPR) son read-modify-write, STLR/STXR y
las exclusivas tienen semantica especial que la heuristica NO capta.

FASE 2 (futuro, sustituye @ref annotate_rw): analizador del pseudocodigo ARM
(no se ejecuta; solo se extraen registros leidos/escritos, NZCV, memoria,
barreras, exclusividad).  La firma de @ref to_irform no cambia: el importador y
el serializer quedan intactos.

FASE 3 (futuro): overlay ARM (DSB->barrier, ISB->serializing, DMB->mem_release,
LDAR/STLR->mem_acquire/mem_release, CAS/SWP/LDADD->atomic, exclusivas->ll_sc),
igual que el overlay de x86, cargado por el build y no por este importador.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ir  # noqa: E402

# Mnemonicos que LEEN NZCV (condicionales + acarreo).  Heuristica Fase 1.
_READS_NZCV = re.compile(
    r'^(ADC|ADCS|SBC|SBCS|CSEL|CSINC|CSINV|CSNEG|CSET|CSETM|CINC|CINV|CNEG|'
    r'CCMP|CCMN|CFINV|RMIF)$|^B[A-Z]*\.', re.I)
# Mnemonicos load/store por familia (senal estructural + convencion Fase 1).
_LOADISH = re.compile(r'^(LD|LDR|LDP|LDAR|LDAPR|LDXR|LDAXR|PRFM|LDNP)', re.I)
_STOREISH = re.compile(r'^(ST|STR|STP|STLR|STXR|STLXR|STNP)', re.I)
# Familia atomica (read-modify-write): los reg transferidos y la memoria se
# leen Y escriben.  La Fase 1 los marca RMW; la Fase 2 dara el detalle exacto.
_ATOMIC = re.compile(
    r'^(CAS[ALBPH]*|SWP[ALBH]*|LD(ADD|CLR|EOR|SET|SMAX|SMIN|UMAX|UMIN)|'
    r'ST(ADD|CLR|EOR|SET|SMAX|SMIN|UMAX|UMIN))', re.I)


def _is_memory(syn):
    """Instruccion de memoria: senal ESTRUCTURAL (psname A64.memory.* o hay [])."""
    return syn.has_mem or syn.psname.startswith('A64.memory')


def annotate_rw(syn):
    """FASE 1 heuristica: devuelve (reads, writes) por operando + (wf, rf).

    Punto de sustitucion para la FASE 2 (pseudocodigo).  @return (list[(r,w)],
    writes_flags, reads_flags)."""
    mnem = syn.mnemonic.upper()
    mem = _is_memory(syn)
    atomic = bool(_ATOMIC.match(mnem))
    is_load = bool(_LOADISH.match(mnem)) and not atomic
    is_store = bool(_STOREISH.match(mnem)) and not atomic
    writeback = mem and re.search(r'(pre|post|immpre|immpost|_wb)', syn.encoding)
    rw = []
    seen_reg = False
    for i, o in enumerate(syn.operands):
        if o.kind in ('imm', 'relbr', '?'):
            rw.append((True, False))
            continue
        # registro
        if o.in_memory:                       # registro de direccion (base/indice)
            rw.append((True, bool(writeback and not seen_reg)))
            continue
        if atomic:                            # RMW: el reg transferido se lee y escribe
            rw.append((True, True))
        elif mem:                             # registro TRANSFERIDO (dato)
            # store lee el dato, load lo escribe; si no se sabe, por defecto lee.
            rw.append((is_store or not is_load, is_load))
        else:                                 # data-processing: op0 = destino
            rw.append((seen_reg, not seen_reg))
        seen_reg = True
    # NZCV: forma-S escribe (heuristica S final en general/float), condicionales leen.
    wf = (mnem.endswith('S') and syn.instr_class in ('general', 'float')
          and not atomic
          and not mnem.startswith(('LD', 'ST', 'CLS')))
    rf = bool(_READS_NZCV.match(mnem))
    return rw, wf, rf


def to_irform(syn):
    """SynForm -> ir.InstrForm: aplica la semantica (Fase 1) y derive_effects."""
    rw, wf_extra, rf_extra = annotate_rw(syn)
    ops = []
    for i, (o, (r, w)) in enumerate(zip(syn.operands, rw)):
        ops.append(ir.Operand(idx=i, kind=o.kind, width=o.width, read=r, write=w,
                              implicit=False, suppressed=False,
                              register_set=o.register_set))
    # operando de memoria sintetico (para has_mem y las mascaras).
    if syn.has_mem and not any(o.kind == 'mem' for o in ops):
        mnem = syn.mnemonic.upper()
        atomic = bool(_ATOMIC.match(mnem))
        is_store = bool(_STOREISH.match(mnem)) and not atomic
        ops.append(ir.Operand(idx=len(ops), kind='mem', width=0,
                              read=atomic or not is_store,
                              write=atomic or is_store,
                              implicit=False, suppressed=False,
                              register_set='-'))
    rm, wm, hm, hi, wf, rf = ir.derive_effects(ops)
    return ir.InstrForm(
        iform=syn.encoding, iclass=syn.mnemonic, mnemonic=syn.mnemonic,
        opcode=syn.opcode, extension=syn.ext,
        enc=ir.EncodingFeatures(isa_set=syn.feature or 'A64'),
        operands=ops, read_mask=rm, write_mask=wm, has_mem=hm, has_imm=hi,
        writes_flags=wf or wf_extra, reads_flags=rf or rf_extra,
        category=(syn.feature or syn.instr_class), summary=syn.brief,
        asm_string=(syn.mnemonic + (' ' + syn.datatype if syn.datatype else '')),
        url='')
