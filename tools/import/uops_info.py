#!/usr/bin/env python3
"""Importador de uops.info (instructions.xml) -> IR comun de Vesta.

Frontend hacia el IR neutral de @ref ir (no el modelo interno del compilador):
lee el XML de uops.info en streaming (~140 MB) y produce @ref ir.InstrForm +
@ref ir.MicroArchInstr-equivalente (aqui la medicion cruda por microarq).  NO
deduplica (eso es @c optimize) ni serializa (eso es @c build_database).

Convencion del proyecto: los scripts de automatizacion van en Python (no .sh).
"""
import os
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ir  # noqa: E402

# Atributos de <instruction> relevantes para la IDENTIDAD (encoding).  Anadir
# uno aqui lo mete en la clave semantica automaticamente.
ENC_ATTRS = ("isa-set", "eosz", "evex", "vex", "mask", "bcast", "roundc",
             "sae", "nf", "zeroing", "incomplete_opcode")


def _flags_rw(attrib):
    """(lee_flags, escribe_flags) de un operando type=flags."""
    r = w = False
    for k, v in attrib.items():
        if k.startswith("flag_"):
            if "r" in v:
                r = True
            if "w" in v:
                w = True
    return r, w


def _parse_operands(el):
    """Extrae operandos (incluidos los implicitos) + mascaras + flags."""
    operands = []
    read_mask = write_mask = 0
    has_mem = has_imm = wr_flags = rd_flags = False
    for op in el.findall("operand"):
        a = op.attrib
        kind = a.get("type", "")
        idx0 = int(a.get("idx", "1")) - 1  # base 0
        reads = a.get("r", "0") == "1"
        writes = a.get("w", "0") == "1"
        suppressed = a.get("suppressed", "0") == "1"
        # uops.info no tiene atributo 'implicit': un operando suppressed (no
        # aparece en el texto) ES un registro implicito fijo.
        implicit = suppressed or a.get("implicit", "0") == "1"
        width = int(a.get("width", "0") or "0")
        reg_set = (op.text or "").strip()

        if kind == "flags":
            fr, fw = _flags_rw(a)
            rd_flags = rd_flags or fr
            wr_flags = wr_flags or fw
            reads = reads or fr
            writes = writes or fw

        operands.append(ir.Operand(idx0, kind, width, reads, writes,
                                   implicit, suppressed, reg_set))
        if kind == "mem":
            has_mem = True
        elif kind == "imm":
            has_imm = True
        # Mascaras: solo operandos EXPLICITOS de registro/memoria (los implicitos
        # los ve el compilador como efecto, no como operando del texto).
        if not implicit and not suppressed and kind in ("reg", "mem"):
            bit = 1 << idx0
            if reads:
                read_mask |= bit
            if writes:
                write_mask |= bit
    return (operands, read_mask, write_mask, has_mem, has_imm, wr_flags, rd_flags)


def _parse_timing(arch_el):
    """Medicion cruda de un bloque <architecture>, o None."""
    m = arch_el.find("measurement")
    if m is None:
        return None
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
    microcoded = a.get("uops_MS", "0") not in ("0", "")
    macro_fusible = "macro_fusible" in a
    micro_fusible = "micro_fusible" in a
    try:
        div_cycles = float(a["div_cycles"]) if "div_cycles" in a else -1.0
    except ValueError:
        div_cycles = -1.0
    # Latencias: TODAS las aristas; un solo <latency> da varias (cycles=reg,
    # cycles_mem=load-use, cycles_addr/index=agen), cada una con su kind (int).
    lats = []
    for lel in m.findall("latency"):
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
            lats.append(ir.LatencyEdge(so, to, c, ir.LATENCY_KIND_ID[kind], ub))
    ports = []
    for term in a.get("ports", "").split("+"):
        term = term.strip()
        if "*" not in term:
            continue
        w, grp = term.split("*", 1)
        try:
            ports.append(ir.PortUse(grp.strip(), float(w)))
        except ValueError:
            pass
    return ir.SchedulerClass(lats, recip_tp, uops, microcoded, macro_fusible,
                             div_cycles, ports)


def _width_sig(operands):
    """Firma de anchos de los operandos EXPLiCITOS de registro/mem/imm."""
    return tuple(o.width for o in operands
                 if not o.suppressed and o.kind in ("reg", "mem", "imm"))


def parse(xml_path, specs, report=None):
    """Parsea el XML en streaming.

    @param specs   lista de @ref ir.MicroArchSpec a extraer.
    @param report  dict opcional donde acumular contadores para el informe.
    @return (forms, timings, xml_date) con forms=list[InstrForm] (una por
            SemanticID unico) y timings={xml_name: {semantic_id: SchedulerClass}}.
    """
    forms = []
    by_sid = {}          # semantic_id -> semantic_key (deteccion de colision)
    timings = {s.xml_name: {} for s in specs}
    wanted = set(timings)
    xml_date = ""
    n_meas = n_impl = n_supp = n_dup = n_collision = 0
    unknown_meas_attrs = set()
    _base = {"TP_loop", "TP_unrolled", "TP_ports", "uops", "uops_MS",
             "uops_MITE", "uops_retire_slots", "ports", "macro_fusible",
             "micro_fusible", "complex_decoder", "available_simple_decoders",
             "div_cycles"}
    known_meas = set(_base)
    for b in list(_base):
        known_meas.add(b + "_indexed")
        known_meas.add(b + "_same_reg")

    for ev, el in ET.iterparse(xml_path, events=("start", "end")):
        if ev == "start":
            if el.tag == "root":
                xml_date = el.get("date", "")
            continue
        if el.tag != "instruction":
            continue
        iform = el.get("iform", "")
        if iform:
            (ops, rm, wm, mem, imm, wf, rf) = _parse_operands(el)
            wsig = _width_sig(ops)
            uid = iform if not wsig else iform + "/" + "x".join(str(w)
                                                               for w in wsig)
            enc = {k: el.get(k) for k in ENC_ATTRS if el.get(k) is not None}
            form = ir.InstrForm(iform, el.get("iclass", ""), el.get("asm", ""),
                                el.get("opcode", ""), el.get("extension", ""),
                                enc, wsig, uid, ops, rm, wm, mem, imm, wf, rf)
            sid = ir.semantic_id(form)
            skey = ir.semantic_key(form)
            if sid not in by_sid:
                by_sid[sid] = skey
                forms.append(form)
                for o in ops:
                    if o.implicit:
                        n_impl += 1
                    if o.suppressed:
                        n_supp += 1
            elif by_sid[sid] != skey:
                n_collision += 1   # colision de hash de 64 bits (deberia ser 0)
            else:
                n_dup += 1         # fila duplicada exacta
            for arch in el.findall("architecture"):
                name = arch.get("name", "")
                if name in wanted:
                    m = arch.find("measurement")
                    if m is not None:
                        n_meas += 1
                        for k in m.attrib:
                            if k not in known_meas:
                                unknown_meas_attrs.add(k)
                    t = _parse_timing(arch)
                    if t is not None and sid not in timings[name]:
                        timings[name][sid] = t
        el.clear()

    if report is not None:
        report.update({
            "forms": len(forms), "implicit_operands": n_impl,
            "suppressed_operands": n_supp, "measurements": n_meas,
            "exact_dup_rows": n_dup, "hash_collisions": n_collision,
            "unknown_measurement_fields": sorted(unknown_meas_attrs),
        })
    return forms, timings, xml_date
