#!/usr/bin/env python3
"""FASE 2 de la semantica ARM: analisis del PSEUDOCODIGO (ASL) del MRAS.

NO ejecuta el pseudocodigo: lo ANALIZA con regex para extraer los efectos reales
de cada encoding y SUSTITUIR la heuristica de @ref mras_semantics (Fase 1):

  - registros LEIDOS / ESCRITOS  (via los accesores X(v)/V[v]/Z[v]/P[v]/Q[v])
  - NZCV leido / escrito         (PSTATE.<N,Z,C,V> / PSTATE.NZCV / PSTATE.C)
  - memoria leida / escrita      (Mem[]/MemStore/MemLoad/MemAtomic=RMW)
  - barrera / exclusividad       (Data*Barrier, ExclusiveMonitors) -> Fase 3

Puente var->campo->operando: el bloque Decode mapea las variables ASL a los
campos del encoding (`let d : ... = UInt(Rd);` -> var 'd' == campo 'Rd'); cada
operando sintactico ya lleva su campo (`encodedin`).  Asi un `X(d) = ...` marca
como ESCRITO el operando cuyo campo es Rd.

Cobertura: la aritmetica/logica/memoria/atomicos/comparaciones directas se
resuelven exactas.  Cuando el pseudocodigo es demasiado indirecto (helpers que
ocultan el acceso, escrituras condicionales complejas), @ref derive devuelve
@c None para ese aspecto y el llamador conserva la heuristica Fase 1.
"""
import re

# Decode: `let d : integer{} = UInt(Rd);`  ->  var 'd' == campo 'Rd'.
_VAR_FIELD = re.compile(r'\b(?:let|constant|integer)\s+(\w+)\s*(?::[^=]*)?=\s*'
                        r'UInt\((\w+)\)')
# Accesor de banco de registros: X{sz}(v) / V[v] / Z[v] / P[v] / Q[v] / D[v]...
_ACCESS = re.compile(r'\b([XVZPQD])\s*(?:\{[^}]*\})?\s*[\(\[]\s*(\w+)\s*[\)\]]'
                     r'(\s*=(?!=))?')
# NZCV: cada aparicion de PSTATE.<flag>.  Es ESCRITURA si le sigue (tras un ')'
# opcional, por el patron '(res, PSTATE.NZCV) = ...') un '='; si no, LECTURA.
# ARM usa PSTATE.<N,Z,C,V> (angulares) y PSTATE.[N,Z,C,V] (corchetes) -> ambos.
_PSTATE = re.compile(r'PSTATE\s*\.\s*(?:[<\[][\sNZCV,]+[>\]]|NZCV|[NZCV])')
_PSTATE_WR = re.compile(r'^\s*\)?\s*=(?!=)')


def _var_to_field(decode_ps):
    m = {}
    for var, field in _VAR_FIELD.findall(decode_ps or ''):
        m[var] = field
    return m


def derive(decode_ps, operation_ps, operands):
    """Devuelve (rw, wf, rf, mem_rw, flags_barrier) o None si no hay Operation.

    @c rw = list[(lee, escribe)] por operando (mismo orden que @c operands).
    @c mem_rw = (lee, escribe) del acceso a memoria, o None si no hay.
    @c flags_barrier = dict con 'barrier'/'exclusive'/'atomic' (para Fase 3)."""
    if not operation_ps:
        return None
    op = operation_ps
    v2f = _var_to_field(decode_ps)
    # campo -> indices de operando (un campo puede repetirse? normalmente 1).
    field_ops = {}
    for i, o in enumerate(operands):
        f = (o.field or '').strip()
        if f:
            field_ops.setdefault(f, []).append(i)

    reads = set()
    writes = set()
    for bank, var, assign in _ACCESS.findall(op):
        field = v2f.get(var)
        if not field:
            continue
        for i in field_ops.get(field, []):
            (writes if assign else reads).add(i)

    # NZCV: escritura si tras la aparicion (con ')' opcional) hay '='.
    wf = rf = False
    for m in _PSTATE.finditer(op):
        if _PSTATE_WR.match(op[m.end():m.end() + 6]):
            wf = True
        else:
            rf = True
    # helpers que LEEN NZCV sin nombrarlo (condicionales): ConditionHolds(cond).
    if re.search(r'ConditionHolds|CurrentCond', op):
        rf = True

    # memoria
    mem_rw = None
    has_atomic = 'MemAtomic' in op
    if has_atomic:
        mem_rw = (True, True)
    else:
        mem_w = bool(re.search(r'\bMem\[[^\]]*\]\s*=(?!=)', op)
                     or re.search(r'MemStore|MemSingle\w*\s*\([^=]*=', op))
        mem_r = bool(re.search(r'=\s*Mem\[', op)
                     or 'MemLoad' in op or ('Mem[' in op and not mem_w))
        if mem_w or mem_r:
            mem_rw = (mem_r or not mem_w, mem_w)

    fb = {
        'barrier': bool(re.search(r'(DataMemoryBarrier|DataSynchronizationBarrier|'
                                  r'InstructionSynchronizationBarrier)', op)),
        'exclusive': bool(re.search(r'(ExclusiveMonitors|SetExclusiveMonitors|'
                                    r'ClearExclusiveMonitors)', op)),
        'atomic': has_atomic,
    }

    # construir rw por operando; los operandos no-registro (imm/label/mem) se
    # dejan a None aqui (el llamador les pone la convencion: imm lee).
    rw = []
    for i, o in enumerate(operands):
        if o.kind == 'reg':
            r = i in reads
            w = i in writes
            if not r and not w:
                # el pseudocodigo no lo resolvio (helper indirecto) -> None
                rw.append(None)
            else:
                rw.append((r, w))
        else:
            rw.append(None)
    return rw, wf, rf, mem_rw, fb
