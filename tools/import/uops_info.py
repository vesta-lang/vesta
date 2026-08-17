#!/usr/bin/env python3
"""Importador de uops.info (instructions.xml) -> IR comun de Vesta.

Responsabilidad UNICA: traducir XML -> IR (@ref ir.InstrForm + @ref
ir.RawSchedule).  NO asigna IDs (eso es @c optimize) ni serializa (eso es
@c serialize).  Falla ante atributos DESCONOCIDOS de <instruction>/<operand>/
<latency>/<measurement>: uops.info cambia, y un campo nuevo debe ser una
decision consciente, no colarse en silencio.

Convencion del proyecto: los scripts de automatizacion van en Python (no .sh).
"""
import os
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ir  # noqa: E402

# Atributos de <instruction> en TRES categorias (no todos los "known" son
# iguales): identidad (entran en form_key via EncodingFeatures), consumidos
# (se leen pero no via encoding) e ignorados (documentacion).
ENCODING_MAP = {                       # atributo XML -> campo de EncodingFeatures
    "isa-set": "isa_set", "eosz": "eosz", "evex": "evex", "vex": "vex",
    "mask": "mask", "bcast": "bcast", "roundc": "roundc", "sae": "sae",
    "nf": "nf", "zeroing": "zeroing", "incomplete_opcode": "incomplete_opcode",
    "rep": "rep", "locked": "locked", "high8": "high8", "agen": "agen",
    "immzero": "immzero", "rm": "rm", "mxcsr": "mxcsr",
    "no_reg_match": "no_reg_match", "no_src_dest_match": "no_src_dest_match",
}
_INSTR_IDENTITY = set(ENCODING_MAP)
_INSTR_CONSUMED = {"iclass", "iform", "opcode", "extension", "asm"}
_INSTR_IGNORED = {"category", "cpl", "string", "summary", "url", "url-ref"}
_INSTR_KNOWN = _INSTR_IDENTITY | _INSTR_CONSUMED | _INSTR_IGNORED

_OPERAND_KNOWN = {"idx", "name", "type", "r", "w", "width", "suppressed",
                  "implicit", "xtype", "memory-prefix", "memory-suffix",
                  "moffs", "multireg", "opmask", "seg", "VSIB", "base",
                  "index", "conditionalWrite"}

_LAT_BASE = {"start_op", "target_op", "cycles", "cycles_mem", "cycles_addr",
             "cycles_addr_index", "cycles_same_reg", "max_cycles", "min_cycles",
             "max_cycles_addr", "max_cycles_addr_index", "min_cycles_addr",
             "min_cycles_addr_index"}
_LATENCY_KNOWN = set(_LAT_BASE) | {b + "_is_upper_bound" for b in _LAT_BASE}

_MEAS_BASE = {"TP_loop", "TP_unrolled", "TP_ports", "uops", "uops_MS",
              "uops_MITE", "uops_retire_slots", "ports", "macro_fusible",
              "micro_fusible", "complex_decoder", "available_simple_decoders",
              "div_cycles"}
_MEAS_SUFFIXES = ("", "_indexed", "_same_reg")
_MEAS_KNOWN = {b + s for b in _MEAS_BASE for s in _MEAS_SUFFIXES}


def _check_attrs(el, known, elem, unknown):
    """Anade a @p unknown los atributos de @p el que no esten en @p known (los
    @c flag_* de operandos son validos como grupo)."""
    for k in el.attrib:
        if k not in known and not k.startswith("flag_"):
            unknown.add("%s.%s" % (elem, k))


def _flags_rw(attrib):
    """(lee, escribe, leidas, escritas) de los atributos @c flag_* de la fuente.

    La fuente lo dice POR BANDERA (@c flag_CF="w", @c flag_ZF="r",
    @c flag_CF="cw" para las condicionales, @c flag_AF="undef") y aqui se
    colapsaba a dos booleanos, con lo que un @c bt -- que solo deja el acarreo --
    y un @c cmp -- que deja las seis -- salian identicos.  Se conservan los
    nombres ademas de los booleanos: lo segundo es lo que ya habia, lo primero es
    lo que faltaba.

    @c undef se cuenta como ESCRITA: la bandera queda con un valor que no se
    puede predecir, asi que quien la tuviera la ha perdido igual.  Decir que no
    se toca seria lo unico que romperia.
    """
    r = w = False
    leidas, escritas = [], []
    for k, v in attrib.items():
        if not k.startswith("flag_"):
            continue
        nombre = k[len("flag_"):].lower()
        if "r" in v:
            r = True
            leidas.append(nombre)
        # `w`, `cw` (condicional) y `undef` dejan la bandera cambiada o inservible.
        if "w" in v or v == "undef":
            w = True
            escritas.append(nombre)
    return (r, w, sorted(leidas, key=ir.flag_sort_key),
            sorted(escritas, key=ir.flag_sort_key))


def _parse_operands(el, unknown, flag_sets=None):
    """Lista de @ref ir.Operand (los efectos derivados los da ir.derive_effects).

    @param flag_sets Si se pasa una lista, se le anaden las banderas LEIDAS y
                     ESCRITAS por nombre (dos listas).  Va aparte de los operandos
                     porque es un efecto de la forma, no un operando mas -- y
                     sobre todo porque los operandos entran en la IDENTIDAD de la
                     forma, y meterlo ahi moveria todos los FormID.
    """
    operands = []
    for op in el.findall("operand"):
        _check_attrs(op, _OPERAND_KNOWN, "operand", unknown)
        a = op.attrib
        kind = a.get("type", "")
        idx0 = int(a.get("idx", "1")) - 1
        reads = a.get("r", "0") == "1"
        writes = a.get("w", "0") == "1"
        suppressed = a.get("suppressed", "0") == "1"
        implicit = suppressed or a.get("implicit", "0") == "1"
        width = int(a.get("width", "0") or "0")
        if kind == "flags":
            fr, fw, leidas, escritas = _flags_rw(a)
            reads = reads or fr
            writes = writes or fw
            if flag_sets is not None:
                flag_sets.append((leidas, escritas))
        operands.append(ir.Operand(idx0, kind, width, reads, writes,
                                   implicit, suppressed, (op.text or "").strip()))
    return operands


def _encoding(el):
    """EncodingFeatures desde los atributos de encoding presentes (mapa
    explicito, sin acoplar dashes<->underscores por replace)."""
    kw = {}
    for xml_attr, field in ENCODING_MAP.items():
        v = el.get(xml_attr)
        if v is not None:
            kw[field] = v
    return ir.EncodingFeatures(**kw)


def _parse_timing(arch_el, unknown):
    m = arch_el.find("measurement")
    if m is None:
        return None
    _check_attrs(m, _MEAS_KNOWN, "measurement", unknown)
    a = m.attrib
    tps = []
    for k in ("TP_loop", "TP_unrolled", "TP_ports"):
        if k in a:
            try:
                tps.append(float(a[k]))
            except ValueError:
                pass
    recip_tp = min(tps) if tps else -1.0
    try:
        uops = int(a.get("uops", "0") or "0")
    except ValueError:
        uops = 0
    try:
        div_cycles = float(a["div_cycles"]) if "div_cycles" in a else -1.0
    except ValueError:
        div_cycles = -1.0
    lats = []
    for lel in m.findall("latency"):
        _check_attrs(lel, _LATENCY_KNOWN, "latency", unknown)
        la = lel.attrib
        so = int(la.get("start_op", "0") or "0") - 1
        to = int(la.get("target_op", "0") or "0") - 1
        for attr, kind in (("cycles", "reg"), ("cycles_mem", "mem"),
                           ("cycles_addr", "addr"),
                           ("cycles_addr_index", "addr_index")):
            if attr not in la:
                continue
            try:
                c = float(la[attr])
            except ValueError:
                continue
            ub = la.get(attr + "_is_upper_bound", "0") == "1"
            lats.append(ir.LatencyEdge(start_operand=so, target_operand=to,
                                       cycles=c, kind=ir.LATENCY_KIND_ID[kind],
                                       upper_bound=ub))
    ports = []
    for term in a.get("ports", "").split("+"):
        term = term.strip()
        if "*" not in term:
            continue
        w, grp = term.split("*", 1)
        try:
            ports.append(ir.PortUse(port_group=grp.strip(), uops=float(w)))
        except ValueError:
            pass
    return ir.RawSchedule(latencies=lats, recip_tp=recip_tp, uops=uops,
                          microcoded=a.get("uops_MS", "0") not in ("0", ""),
                          macro_fusible="macro_fusible" in a,
                          div_cycles=div_cycles, ports=ports)


def parse(xml_path, specs, report=None):
    """@return (forms, timings, xml_date, unknown_attrs).

    forms=list[InstrForm] (una por clave estructural unica); timings=
    {xml_name: {form_key: RawSchedule}}; unknown_attrs=set (si != vacio, el
    caller DEBE abortar: el esquema del XML cambio)."""
    forms = []
    known_forms = {}
    timings = {s.xml_name: {} for s in specs}
    wanted = set(timings)
    xml_date = ""
    unknown = set()
    n_meas = n_impl = n_supp = n_dup = 0

    for ev, el in ET.iterparse(xml_path, events=("start", "end")):
        if ev == "start":
            if el.tag == "root":
                xml_date = el.get("date", "")
            continue
        if el.tag != "instruction":
            continue
        iform = el.get("iform", "")
        if iform:
            _check_attrs(el, _INSTR_KNOWN, "instruction", unknown)
            flag_sets = []
            ops = _parse_operands(el, unknown, flag_sets)
            (rm, wm, mem, imm, wf, rf) = ir.derive_effects(ops)
            # Una forma tiene como mucho un operando de banderas; si no lo tiene,
            # los conjuntos quedan vacios y los booleanos ya dicen que no toca
            # ninguna.
            leidas = ",".join(flag_sets[0][0]) if flag_sets else ""
            escritas = ",".join(flag_sets[0][1]) if flag_sets else ""
            form = ir.InstrForm(iform, el.get("iclass", ""), el.get("asm", ""),
                                el.get("opcode", ""), el.get("extension", ""),
                                _encoding(el), ops, rm, wm, mem, imm, wf, rf,
                                el.get("category", ""), el.get("summary", ""),
                                el.get("string", ""), el.get("url", ""),
                                escritas, leidas)
            key = ir.form_key(form)
            if key not in known_forms:
                known_forms[key] = form
                forms.append(form)
                for o in ops:
                    if o.implicit:
                        n_impl += 1
                    if o.suppressed:
                        n_supp += 1
            else:
                n_dup += 1
            for arch in el.findall("architecture"):
                name = arch.get("name", "")
                if name in wanted:
                    if arch.find("measurement") is not None:
                        n_meas += 1
                    t = _parse_timing(arch, unknown)
                    if t is not None and key not in timings[name]:
                        timings[name][key] = t
        el.clear()

    if report is not None:
        report.update({
            "forms": len(forms), "implicit_operands": n_impl,
            "suppressed_operands": n_supp, "measurements": n_meas,
            "exact_dup_rows": n_dup,
        })
    return forms, timings, xml_date, unknown
