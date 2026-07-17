#!/usr/bin/env python3
"""Importador de COSTE ARM desde una Software Optimization Guide (SWOG) de ARM.

Fuente: el PDF oficial de una SWOG (p.ej. Cortex-A76, Neoverse N1).  EXTRACCION
UNICA -> nuestro formato serializado (<core>.vxarch); el compilador/sitio solo
consumen eso.  Reutiliza ir.py/serialize.py (mismo pipeline que x86/uops.info).

La SWOG da el coste por GRUPO DE INSTRUCCION (no por encoding): grupo ->
(mnemonicos, Exec Latency, Execution Throughput [IPC], Utilized Pipelines).  Se
mapea a nuestras formas por MNEMONICO (iclass): cada forma cuya iclass este en
el grupo recibe su clase de coste.  Granularidad inherente de la SWOG: un
mnemonico en varios grupos (ADD basico vs ADD extend) -> se toma el primero
(el mas comun); refinar por forma-de-operandos es trabajo posterior.

  recip_tp = 1 / throughput_IPC     (la SWOG da instrucciones-por-ciclo)
  latency  = Exec Latency (una arista result representativa)
  ports    = las pipelines utilizadas (B/S/I/M/D/L/V... segun la leyenda)

    python tools/import/swog_arm.py <swog.pdf> <core-canonico> <family> <arm.vxisa> <dir_salida>
    python tools/import/swog_arm.py A76.pdf arm-cortex-a76 arm arm.vxisa out/
"""
import os
import re
import sys

import pdfplumber

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ir          # noqa: E402
import serialize   # noqa: E402
import database    # noqa: E402


def _num(s):
    """Primer numero de una celda ('2', '1-4', '3 (see note)') o None."""
    if not s:
        return None
    m = re.search(r'\d+(?:\.\d+)?', s)
    return float(m.group(0)) if m else None


def _expand_mnems(cell):
    """Expande la columna de instrucciones a mnemonicos concretos.

    'ADD{S}, SUB{S}' -> ADD, ADDS, SUB, SUBS.  Ignora '(same as ...)' (lo
    resuelve el llamador) y notas entre parentesis."""
    if not cell:
        return [], None
    txt = cell.replace('\n', ' ').strip()
    ref = None
    m = re.search(r'\(same as ([^)]+)\)', txt, re.I)
    if m:
        ref = m.group(1).strip().lower()
        txt = txt[:m.start()] + txt[m.end():]
    txt = re.sub(r'\([^)]*\)', ' ', txt)           # quita notas entre parentesis
    out = []
    for tok in re.split(r'[,\s]+', txt):
        tok = tok.strip().strip('.')
        if not tok or not re.match(r'^[A-Za-z][A-Za-z0-9]*(\{S\})?[A-Za-z0-9]*$', tok):
            continue
        if '{S}' in tok:
            base = tok.replace('{S}', '')
            out.append(base)
            out.append(base + 'S')
        else:
            out.append(tok)
    return out, ref


def _pipeline_legend(pdf):
    """{simbolo: nombre} de la tabla 'Pipeline name | Symbol'."""
    leg = {}
    for page in pdf.pages:
        for t in page.extract_tables():
            hdr = " ".join((c or "") for c in (t[0] if t else []))
            if 'Pipeline name' in hdr and 'Symbol' in hdr:
                for row in t[1:]:
                    if len(row) >= 2 and row[0] and row[1]:
                        sym = row[1].strip().split()[0]
                        leg[sym] = row[0].strip().replace('\n', ' ')
        if leg:
            break
    return leg


def _parse_rows(pdf):
    """list[(group, mnem_cell, latency, tp, pipes[])] de las tablas AArch64."""
    rows = []
    for page in pdf.pages:
        for t in page.extract_tables():
            if not t or not t[0]:
                continue
            hdr = " ".join((c or "") for c in t[0])
            if 'AArch64' not in hdr or 'Latency' not in hdr and 'Exec' not in hdr:
                continue
            for row in t[1:]:
                cells = [(c or "").strip() for c in row]
                if len(cells) < 5:
                    continue
                group = cells[0].replace('\n', ' ').strip()
                mnem_cell = cells[1]
                lat = _num(cells[2])
                tp = _num(cells[3])
                pipes = [p.strip() for p in re.split(r'[,\n]', cells[4])
                         if p.strip() and re.match(r'^[A-Z][A-Z0-9]?$', p.strip())]
                if not group and not mnem_cell:
                    continue
                rows.append((group, mnem_cell, lat, tp, pipes))
    return rows


def build_cost_map(pdf):
    """(mnemonico -> (latency, recip_tp, pipes[]), port_names[]) de la SWOG."""
    rows = _parse_rows(pdf)
    # grupo -> instrucciones (para resolver '(same as X)')
    group_mnems = {}
    for group, cell, lat, tp, pipes in rows:
        mnems, _ = _expand_mnems(cell)
        if group:
            group_mnems.setdefault(group.lower(), []).extend(mnems)
    port_index = {}
    port_names = []
    cost = {}

    def port_id(sym):
        if sym not in port_index:
            port_index[sym] = len(port_names)
            port_names.append(sym)
        return port_index[sym]

    for group, cell, lat, tp, pipes in rows:
        if lat is None and tp is None:
            continue
        mnems, ref = _expand_mnems(cell)
        if ref and ref in group_mnems:
            mnems = mnems + group_mnems[ref]
        recip = (1.0 / tp) if tp else -1.0
        pslots = [ir.PortSlot(group=port_id(p), uops=1.0) for p in pipes]
        lat_edges = ([ir.LatencyEdge(0, 0, float(lat), ir.LATENCY_RESULT)]
                     if lat is not None else [])
        for mn in mnems:
            cost.setdefault(mn.upper(), (lat_edges, recip, pslots))
    return cost, port_names


def main():
    if len(sys.argv) < 6:
        sys.exit("uso: python swog_arm.py <swog.pdf> <core> <family> "
                 "<arm.vxisa> <dir_salida>")
    pdf_path, core, family, vxisa, out = sys.argv[1:6]
    os.makedirs(out, exist_ok=True)
    with pdfplumber.open(pdf_path) as pdf:
        cost, port_names = build_cost_map(pdf)
    forms = database.load_vxisa(vxisa)

    # deduplicar clases de coste; mapear forma->clase por mnemonico (iclass).
    classes = []
    class_key = {}
    form_class = [-1] * (max(forms) + 1)
    mapped = 0
    for fid in sorted(forms):
        mn = forms[fid]["iclass"].upper()
        c = cost.get(mn)
        if c is None:
            continue
        lat_edges, recip, pslots = c
        key = (round(recip, 4), tuple((p.group, p.uops) for p in pslots),
               tuple((e.cycles, e.kind) for e in lat_edges))
        cid = class_key.get(key)
        if cid is None:
            cid = len(classes)
            class_key[key] = cid
            classes.append(ir.SchedulerClass(
                latencies=lat_edges, recip_tp=recip, uops=1,
                microcoded=False, macro_fusible=False, div_cycles=-1.0,
                ports=pslots))
        form_class[fid] = cid
        mapped += 1

    sched = ir.ArchSchedule(
        spec=ir.MicroArchSpec(xml_name=core, canonical_name=core, family=family),
        port_names=port_names, classes=classes, form_class=form_class)
    path = os.path.join(out, core + ".vxarch")
    serialize.write_vxarch(path, sched, "swog", "-", isa="arm", source="arm-swog")
    print("[swog_arm] %s: %d mnemonicos con coste, %d/%d formas mapeadas, "
          "%d clases, pipelines=%s"
          % (core, len(cost), mapped, len(forms), len(classes),
             ",".join(port_names)))


if __name__ == "__main__":
    main()
