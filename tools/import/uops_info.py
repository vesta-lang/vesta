#!/usr/bin/env python3
"""Importador de uops.info (instructions.xml) -> IR comun de Vesta.

Frontend hacia el IR neutral de @ref ir (no el modelo interno del compilador):
lee el XML de uops.info en streaming (~140 MB) y produce @ref ir.InstrForm +
@ref ir.MicroArchInstr.  NO serializa nada (eso es @c build_database.py) ni el
compilador ve jamas este XML.

Convencion del proyecto: los scripts de automatizacion van en Python (no .sh).
"""
import os
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ir  # noqa: E402


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
        # uops.info no tiene atributo 'implicit': un operando SUPPRESSED (no
        # aparece en el texto del ensamblador) ES un registro implicito fijo
        # (EAX/EBX/ECX/EDX de cpuid, rax:rdx de mul...).
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

        # Solo los operandos EXPLiCITOS de registro/memoria contribuyen a las
        # mascaras; los implicitos/suppressed (rax:rdx de mul, flags) NO (el
        # compilador los ve como efecto implicito, no como operando del texto).
        if not implicit and not suppressed and kind in ("reg", "mem"):
            bit = 1 << idx0
            if reads:
                read_mask |= bit
            if writes:
                write_mask |= bit

    return (operands, read_mask, write_mask, has_mem, has_imm, wr_flags, rd_flags)


def _parse_timing(arch_el):
    """@ref ir.MicroArchInstr de un bloque <architecture>, o None."""
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
    # Latencias: TODAS las aristas start_op -> target_op (no se colapsan).  Un
    # solo <latency> puede dar VARIAS aristas: cycles (reg->reg), cycles_mem
    # (load-use), cycles_addr / cycles_addr_index (generacion de direccion).
    # Cada una lleva su 'kind' y su marca de cota superior.
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
            lats.append(ir.LatencyEdge(so, to, c, kind, ub))
    # Puertos: parseados a estructura (no cadena).  "1*p0156+2*p23".
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
    return ir.MicroArchInstr(lats, recip_tp, uops, microcoded, macro_fusible,
                             micro_fusible, div_cycles, ports)


def _width_sig(operands):
    """Firma de anchos de los operandos EXPLiCITOS de registro/mem/imm."""
    return tuple(o.width for o in operands
                 if not o.suppressed and o.kind in ("reg", "mem", "imm"))


def _kinds(operands):
    """Firma de tipos de los operandos EXPLiCITOS (para detectar divergencias
    REALES: dos filas con misma uid deberian tener los mismos tipos)."""
    return tuple(o.kind for o in operands
                 if not o.suppressed and o.kind in ("reg", "mem", "imm"))


def parse(xml_path, specs, report=None):
    """Parsea el XML en streaming.

    @param specs   lista de @ref ir.MicroArchSpec a extraer.
    @param report  dict opcional donde acumular contadores para el informe.
    @return (forms, timings, xml_date) con forms=list[InstrForm] en orden del
            XML, timings={xml_name: {iform: MicroArchInstr}}.
    """
    forms = []
    by_uid = {}
    timings = {s.xml_name: {} for s in specs}
    wanted = set(timings)
    xml_date = ""
    n_meas = n_impl = n_supp = n_dup_div = 0
    unknown_meas_attrs = set()
    # Atributos de <measurement> que conocemos (usados o deliberadamente
    # ignorados de momento: los *_indexed / *_same_reg son mediciones alternas
    # -- direccionamiento indexado, mismo registro -- que hoy no modelamos; el
    # informe solo debe alertar de campos GENUINAMENTE nuevos).
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
            form = ir.InstrForm(iform, el.get("asm", ""), el.get("opcode", ""),
                                el.get("extension", ""), wsig, uid, ops, rm, wm,
                                mem, imm, wf, rf)
            prev = by_uid.get(uid)
            if prev is None:
                by_uid[uid] = form
                forms.append(form)
                for o in ops:
                    if o.implicit:
                        n_impl += 1
                    if o.suppressed:
                        n_supp += 1
            elif _kinds(prev.operands) != _kinds(ops):
                # misma uid pero TIPOS distintos -> divergencia real (cambio del
                # XML).  Los opcodes alternos (misma uid, mismos tipos) NO cuentan.
                n_dup_div += 1
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
                    if t is not None and uid not in timings[name]:
                        timings[name][uid] = t
        el.clear()

    if report is not None:
        report.update({
            "forms": len(forms), "implicit_operands": n_impl,
            "suppressed_operands": n_supp, "measurements": n_meas,
            "dup_iform_divergent": n_dup_div,
            "unknown_measurement_fields": sorted(unknown_meas_attrs),
        })
    return forms, timings, xml_date
