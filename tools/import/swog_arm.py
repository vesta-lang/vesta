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
        # AArch32 load/store-multiple: el SWOG usa el sufijo de direccionamiento
        # (LDMIA/LDMDB...) pero nuestras formas usan el mnemonico base (LDM).
        mm = re.match(r'^(V?LD|V?ST|SRS|RFE)(M?)(IA|IB|DA|DB|FD|FA|ED|EA)$', tok)
        if mm:
            out.append(mm.group(1) + mm.group(2))     # LDMIA -> LDM
    return out, ref


# Titulo de seccion SWOG -> categoria de coste.  Una forma solo recibe el coste
# de una seccion COMPATIBLE con su extension (evita dar coste escalar a una forma
# SVE, o coste entero a una ASIMD).
_SECTION_CAT = [
    (re.compile(r'crypto', re.I), 'CRYPTO'),
    (re.compile(r'advanced\s*simd|asimd|\bsimd\b|neon', re.I), 'ASIMD'),
    (re.compile(r'floating', re.I), 'FP'),
    (re.compile(r'\bsve\b|scalable', re.I), 'SVE'),
    (re.compile(r'branch', re.I), 'BRANCH'),
    (re.compile(r'load|store|memory', re.I), 'LOADSTORE'),
    (re.compile(r'system', re.I), 'SYSTEM'),
    (re.compile(r'arithmetic|logical|move|shift|multiply|divide|bitfield|'
                r'conditional|pointer\s*auth|pc-relative|crc|count|miscell|'
                r'\bdata\b|integer|address', re.I), 'INT'),
]
# CATEGORY_MAP explicito: la extension de la forma -> UNICO bucket de coste
# donde se busca su timing.  Nunca se cruza entre buckets (una forma SVE jamas
# recibe coste GENERAL; una FLOAT jamas ASIMD).  El bucket GENERAL agrupa las
# secciones escalares de la SWOG (arithmetic/branch/load-store/system), que son
# el mismo dominio.  SVE/SVE2/SME/SME2 solo casan su propia seccion: en un core
# sin SVE (A76/N1) quedan sin coste, como debe ser.
_EXT_CATS = {
    # CRYPTO en GENERAL por CRC32 (instr entera que el SWOG pone en cripto).
    'GENERAL': ('INT', 'BRANCH', 'LOADSTORE', 'SYSTEM', 'CRYPTO'),
    # ADVSIMD y FLOAT comparten el dominio vector/FP: la SWOG de ARM las agrupa
    # en una sola seccion "Advanced SIMD and Floating-Point" (mismas pipelines
    # V0/V1).  NO es cruzar entero<->vector; es la realidad del hardware.
    # FP/ASIMD PRIMERO (categoria correcta); LOADSTORE/INT como ultimo recurso
    # para instrucciones V*/F* que el SWOG lista en la seccion de load/store o
    # que la deteccion de seccion metio en INT (VLDR, VDIV, VSQRT...).  Es SEGURO:
    # los mnemonicos vectoriales llevan prefijo V/F y no colisionan con enteros.
    'ADVSIMD': ('ASIMD', 'FP', 'CRYPTO', 'LOADSTORE', 'INT'),
    'FLOAT': ('FP', 'ASIMD', 'CRYPTO', 'LOADSTORE', 'INT'),
    'FPSIMD': ('FP', 'ASIMD', 'CRYPTO', 'LOADSTORE', 'INT'),
    # SVE/SVE2/SME solo su propia seccion; en un core sin SVE quedan sin coste.
    'SVE': ('SVE',), 'SVE2': ('SVE2', 'SVE'),
    'SME': ('SME',), 'SME2': ('SME2', 'SME'),
    'SYSTEM': ('SYSTEM',),
}


def _section_cat(title):
    for rx, cat in _SECTION_CAT:
        if rx.search(title or ''):
            return cat
    return 'INT'


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


# Encabezado de seccion: "3.4 Titulo" (Neoverse N1) o "3.4. Titulo" (Cortex-A76,
# con punto final tras el numero) -> el punto es opcional.
_HEADING = re.compile(r'^\s*\d+(?:\.\d+)+\.?\s+([A-Z][A-Za-z0-9 /,\-]{3,50})', re.M)


def _parse_rows(pdf, isa_hdr='AArch64'):
    """list[(cat, group, mnem_cell, latency, tp, pipes[])] de las tablas de la
    ISA pedida (@c isa_hdr = 'AArch64' o 'AArch32').

    @c cat = categoria de la SECCION (INT/ASIMD/FP/SVE/...) para poder emparejar
    cada forma solo con el coste de una seccion compatible con su extension."""
    rows = []
    cat = 'INT'
    for page in pdf.pages:
        heads = _HEADING.findall(page.extract_text() or '')
        if heads:
            cat = _section_cat(heads[-1])         # ultima seccion vista en la pagina
        for t in page.extract_tables():
            if not t or not t[0]:
                continue
            hdr = " ".join((c or "") for c in t[0])
            # la 2a columna es "AArch64"/"AArch32" o "SVE Instruction" (las
            # tablas SVE no repiten AArch64: SVE es extension de A64).
            ok_isa = isa_hdr in hdr or (isa_hdr == 'AArch64' and 'SVE' in hdr)
            if not ok_isa or ('Latency' not in hdr and 'Exec' not in hdr):
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
                rows.append((cat, group, mnem_cell, lat, tp, pipes))
    return rows


def build_cost_map(pdf, isa_hdr='AArch64'):
    """((mnemonico, categoria) -> (lat_edges, recip_tp, pslots), port_names[])."""
    rows = _parse_rows(pdf, isa_hdr)
    # grupo -> instrucciones (para resolver '(same as X)')
    group_mnems = {}
    for cat, group, cell, lat, tp, pipes in rows:
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

    for cat, group, cell, lat, tp, pipes in rows:
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
            cost.setdefault((mn.upper(), cat), (lat_edges, recip, pslots))
    return cost, port_names


def main():
    if len(sys.argv) < 6:
        sys.exit("uso: python swog_arm.py <swog.pdf> <core> <family> "
                 "<arm.vxisa> <dir_salida>")
    pdf_path, core, family, vxisa, out = sys.argv[1:6]
    # ISA opcional (aarch64 por defecto; aarch32 lee las tablas AArch32 del PDF).
    isa = sys.argv[6].lower() if len(sys.argv) > 6 else 'aarch64'
    isa_hdr = 'AArch32' if isa == 'aarch32' else 'AArch64'
    os.makedirs(out, exist_ok=True)
    with pdfplumber.open(pdf_path) as pdf:
        cost, port_names = build_cost_map(pdf, isa_hdr)
    forms = database.load_vxisa(vxisa)

    # deduplicar clases de coste; mapear forma->clase por mnemonico (iclass).
    classes = []
    class_key = {}
    form_class = [-1] * (max(forms) + 1)
    mapped = 0

    def _bases(mn):
        """Nombres candidatos: el exacto + el base cuando el SWOG usa el generico
        (CRC32B->CRC32, CRC32CW->CRC32C, PKHBT->PKH)."""
        names = [mn]
        m = re.match(r'^(CRC32C?)[BHWX]$', mn)
        if m:
            names.append(m.group(1))
        if re.match(r'^PKH(BT|TB)$', mn):
            names.append('PKH')
        return names

    def lookup(mn, ext):
        """Coste de un mnemonico solo desde una seccion COMPATIBLE con su ext."""
        for name in _bases(mn):
            for cat in _EXT_CATS.get(ext, ('INT',)):
                c = cost.get((name, cat))
                if c is not None:
                    return c
        return None

    for fid in sorted(forms):
        fm = forms[fid]
        ext = fm["ext"]
        # forma -> categoria -> (resolver alias) -> timing. Un alias busca por su
        # BASE (CINC -> CSINC); jamas cruza a otra categoria.
        mn = fm["iclass"].upper()
        if fm["category"].startswith("alias:"):
            base = fm["category"][6:].split("_")[0].upper()
            c = lookup(base, ext) or lookup(mn, ext)
        else:
            c = lookup(mn, ext)
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
    serialize.write_vxarch(path, sched, "swog", "-",
                           isa=("arm32" if isa == "aarch32" else "arm"),
                           source="arm-swog")
    print("[swog_arm] %s: %d mnemonicos con coste, %d/%d formas mapeadas, "
          "%d clases, pipelines=%s"
          % (core, len(cost), mapped, len(forms), len(classes),
             ",".join(port_names)))


if __name__ == "__main__":
    main()
