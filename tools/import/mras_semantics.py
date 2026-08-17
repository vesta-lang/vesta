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
import ir              # noqa: E402
import mras_pseudocode  # noqa: E402

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
    """SynForm -> ir.InstrForm.

    Semantica: FASE 2 (pseudocodigo ARM) AUTORITATIVA; la heuristica FASE 1 solo
    rellena lo que el pseudocodigo no resuelve (operando via helper indirecto).
    """
    rw_h, wf_h, rf_h = annotate_rw(syn)       # Fase 1 (fallback)
    ps = mras_pseudocode.derive(syn.decode_ps, syn.operation_ps, syn.operands)
    mem_rw = None
    if ps is not None:
        rw_ps, wf, rf, mem_rw, _fb, rflags, wflags = ps
        # En A32 la MISMA codificacion cubre `add` y `adds`: el pseudocodigo
        # escribe las banderas bajo `if setflags`, y el bit S del encoding decide.
        # Leerlo como una escritura incondicional hacia que las 21 formas de `ADD`
        # dijeran que escriben `c,n,v,z`, que es justo lo contrario de lo que las
        # distingue.  Quien lo decide es el SUFIJO del mnemonico, que aqui se
        # conoce.
        if 'setflags' in (syn.operation_ps or '') and \
                not syn.mnemonic.upper().endswith('S'):
            wflags = []
            wf = False
        # Un ALIAS hereda el pseudocodigo de su base pero NO sus operandos:
        # `cmp x0, x1` es `subs xzr, x0, x1`, asi que el destino de la base no
        # aparece escrito y los que si aparecen estan desplazados.  Con eso, la
        # heuristica "el primero es el destino" hace que `cmp` declare que escribe
        # `x0` -- justo el valor que compara --.
        #
        # Tomar todos los no resueltos de un alias como FUENTE arregla `cmp`,
        # `cmn` y `tst` y ROMPE `mov`, que tambien es un alias y si escribe su
        # primer operando: pasaba a declarar que no escribe nada, y eso es peor --
        # un consumidor creeria que el registro conserva su valor y movería una
        # lectura por encima --.  Se cambia un error del lado conservador por uno
        # que no lo es.
        #
        # Lo correcto es REMAPEAR los operandos del alias contra los de su base
        # (que campo del alias corresponde a que campo de la base) en vez de
        # adivinar por posicion.  Hasta entonces se deja la heuristica, cuyo error
        # -- una escritura de mas en las comparaciones -- cae del lado que no
        # habilita transformaciones.
        rw = [rw_ps[i] if rw_ps[i] is not None else rw_h[i]
              for i in range(len(syn.operands))]
    else:
        rw, wf, rf = rw_h, wf_h, rf_h
        # Sin pseudocodigo no se sabe CUALES: la heuristica solo da el bit.
        rflags, wflags = [], []

    ops = []
    for i, (o, (r, w)) in enumerate(zip(syn.operands, rw)):
        ops.append(ir.Operand(idx=i, kind=o.kind, width=o.width, read=r, write=w,
                              implicit=False, suppressed=False,
                              register_set=o.register_set,
                              optional=getattr(o, "optional", False)))
    # operando de memoria sintetico (r/w del pseudocodigo si esta; si no,
    # convencion Fase 1 load/store/atomico).
    if (syn.has_mem or mem_rw) and not any(o.kind == 'mem' for o in ops):
        if mem_rw is not None:
            mem_r, mem_w = mem_rw
        else:
            mnem = syn.mnemonic.upper()
            atomic = bool(_ATOMIC.match(mnem))
            is_store = bool(_STOREISH.match(mnem)) and not atomic
            mem_r, mem_w = (atomic or not is_store, atomic or is_store)
        ops.append(ir.Operand(idx=len(ops), kind='mem', width=0,
                              read=mem_r, write=mem_w,
                              implicit=False, suppressed=False,
                              register_set='-'))
    rm, wm, hm, hi, dwf, drf = ir.derive_effects(ops)
    return ir.InstrForm(
        iform=syn.encoding, iclass=syn.mnemonic, mnemonic=syn.mnemonic,
        opcode=syn.opcode, extension=syn.ext,
        enc=ir.EncodingFeatures(isa_set=syn.feature or 'A64'),
        operands=ops, read_mask=rm, write_mask=wm, has_mem=hm, has_imm=hi,
        writes_flags=wf or dwf, reads_flags=rf or drf,
        flags_written=",".join(wflags), flags_read=",".join(rflags),
        category=("alias:" + syn.alias_of if syn.is_alias
                  else (syn.feature or syn.instr_class)),
        summary=syn.brief,
        asm_string=(syn.mnemonic + (' ' + syn.datatype if syn.datatype else '')),
        url='')
