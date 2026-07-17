#!/usr/bin/env python3
"""Construye la base de datos de instrucciones de Vesta desde el IR comun.

Toma el IR que producen los importadores (hoy `uops_info`; manana `arm_guides`,
`llvm_sched`) y serializa los ficheros propios de Vesta:

    x86.vxisa              sintaxis por-ISA (iform, opcode, operandos completos
                           -- tipo/ancho/impl/supp/regset --, mascaras, flags).
    intel-skylake.vxarch   base de conocimiento de UNA microarq (por SchedClass:
    intel-alderlake-p.vxarch   latencias por-arista/throughput/uops/puertos).
    amd-zen4.vxarch
    instr_form_ids.h       enum InstrFormID + kInstrFormCount (el compilador
                           trabaja con IDs numericos; lookup por array, sin hash).

Ideas clave (estilo MCSchedModel de LLVM):
  - Clave unica = ISA + iform (NUNCA el mnemonico: IMUL son varias iform).
  - IDs ESTABLES: orden lexicografico por iform -> anadir/quitar formas al XML
    no reordena las que ya existian.
  - SchedulerClass como indireccion: iform_id -> class_id; muchas iform comparten
    (latencias,tp,uops,puertos) y se deduplican.  Claves CUANTIZADAS (centesimas
    enteras) para que 0.5 y 0.5000001 no generen clases distintas.
  - Las latencias NO se colapsan: cada clase guarda TODAS las aristas
    fuente->destino; el compilador decide (peor caso / media) al consultar.
  - Puertos normalizados a indices por-microarq (topologia Skylake != Zen).
  - El overlay semantico manual se VALIDA contra el XML: iform inexistente o
    propiedad desconocida = ERROR (evita divergencia silenciosa).

Nota de formato: hoy la salida es TEXTO (depurable, verificable a ojo).  El
serializado BINARIO (fread en el runtime) es el paso siguiente, una vez validada
la extraccion sobre datos reales.  Una capa @c SemanticID entre InstrFormID y
SchedulerClass (dedup de analisis semantico, no solo de timing) queda planificada
para cuando haya un consumidor real (peephole/alias); no se anade especulando.

Uso:
    python tools/import/build_database.py <instructions.xml> <dir_salida> \
        [overlay_x86_semantics.def]

Convencion del proyecto: los scripts de automatizacion van en Python (no .sh).
"""
import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ir           # noqa: E402
import uops_info     # noqa: E402

# Version del FORMATO (no de los datos).  Subir al cambiar el layout -> el
# loader detecta incompatibilidades.
DB_FORMAT_VERSION = 1

# Microarquitecturas a extraer (nombre en uops.info -> nombre canonico + familia).
MICROARCHS = [
    ir.MicroArchSpec("SKL", "intel-skylake", "intel"),
    ir.MicroArchSpec("ADL-P", "intel-alderlake-p", "intel"),  # Golden Cove
    ir.MicroArchSpec("ZEN4", "amd-zen4", "amd"),
]

# Propiedades semanticas VaLIDAS en el overlay manual (lo que el XML no sabe).
# Un typo (serialising) o una propiedad no listada = error del importador.
VALID_OVERLAY_PROPS = {
    "serializing", "barrier", "atomic", "ll_sc", "branch", "call", "ret",
    "syscall", "stack_push", "stack_pop", "mem_acquire", "mem_release",
    "mem_seq_cst", "may_fault", "privileged", "no_reorder",
}


def _sha256(path):
    """SHA256 del fichero fuente (identidad exacta, no solo la fecha)."""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _sanitize_ident(s):
    """A-Z0-9_ ; el resto -> _  (para el enum C, nombres raros del XML)."""
    return "".join(c if (c.isalnum() or c == "_") else "_" for c in s).upper()


def load_overlay(path, valid_iforms):
    """Lee el overlay semantico y lo VALIDA (iform Y propiedad) contra el XML.

    Formato del .def (iform en su linea; propiedades indentadas):
        IFORM_NAME
            serializing
            barrier
    Devuelve { iform: set(propiedades) }.  Aborta si algo no cuadra.
    """
    overlay = {}
    if not path or not os.path.exists(path):
        return overlay
    cur = None
    errors = []
    with open(path, "r", encoding="ascii") as f:
        for ln, raw in enumerate(f, 1):
            line = raw.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            if not line[0].isspace():
                cur = line.strip()
                if cur not in valid_iforms:
                    errors.append("  L%d: iform inexistente en el XML: '%s'"
                                  % (ln, cur))
                overlay.setdefault(cur, set())
            elif cur is not None:
                prop = line.strip()
                if prop not in VALID_OVERLAY_PROPS:
                    errors.append("  L%d: propiedad desconocida: '%s' (iform %s)"
                                  % (ln, prop, cur))
                overlay[cur].add(prop)
    if errors:
        sys.stderr.write("[build_database] overlay invalido:\n")
        sys.stderr.write("\n".join(errors) + "\n")
        sys.exit(1)
    return overlay


def _op_field(operands):
    """Codifica los operandos completos en una columna de texto compacta.

    Cada operando: idx,kind,width,flags,regset  (flags = r|w<<1|impl<<2|supp<<3).
    Separador de operandos ';'.  El regset se sanea (los separadores ->'/')."""
    parts = []
    for o in operands:
        flags = (int(o.read) | (int(o.write) << 1) |
                 (int(o.implicit) << 2) | (int(o.suppressed) << 3))
        rs = o.register_set.replace(";", "/").replace("|", "/").replace(",", "/")
        rs = rs.replace(" ", "_") or "-"
        parts.append("%d,%s,%d,%d,%s" % (o.idx, o.kind or "-", o.width, flags, rs))
    return ";".join(parts) or "-"


def write_vxisa(path, forms, overlay, xml_date, xml_hash):
    """Serializa la sintaxis por-ISA (operandos COMPLETOS) + overlay validado.

    @param forms  lista YA ordenada (IDs estables); el indice i == InstrFormID.
    """
    with open(path, "w", encoding="ascii", newline="\n") as f:
        f.write("vxisa %d isa=x86 source=uops.info date=%s forms=%d "
                "xml_sha256=%s\n"
                % (DB_FORMAT_VERSION, xml_date, len(forms), xml_hash))
        f.write("# id|uid|iform|ext|opcode|rmask|wmask|mem|imm|wflags|rflags"
                "|operands|overlay\n")
        f.write("# operands = idx,kind,width,flags,regset;...  "
                "flags=r|w<<1|impl<<2|supp<<3\n")
        for i, fm in enumerate(forms):
            ov = ",".join(sorted(overlay.get(fm.uid, []))) or "-"
            f.write("%d|%s|%s|%s|%s|0x%x|0x%x|%d|%d|%d|%d|%s|%s\n" % (
                i, fm.uid, fm.iform, fm.extension or "-", fm.opcode or "-",
                fm.read_mask, fm.write_mask, int(fm.has_mem), int(fm.has_imm),
                int(fm.writes_flags), int(fm.reads_flags),
                _op_field(fm.operands), ov))


def write_ids_header(path, forms):
    """Genera instr_form_ids.h: enum InstrFormID + kInstrFormCount."""
    seen = {}
    with open(path, "w", encoding="ascii", newline="\n") as f:
        f.write("/* GENERADO por tools/import/build_database.py -- NO editar.\n"
                " * Formas de instruccion como clave numerica; el compilador\n"
                " * trabaja con IDs, nunca con cadenas. */\n")
        f.write("#ifndef VX_INSTR_FORM_IDS_H\n#define VX_INSTR_FORM_IDS_H\n")
        f.write("#include <cstdint>\n\n")
        f.write("enum InstrFormID : uint32_t {\n")
        for i, fm in enumerate(forms):
            name = _sanitize_ident(fm.uid)
            # Un saneado podria colisionar (dos uid -> mismo ident); desambigua.
            if name in seen:
                name = "%s__%d" % (name, i)
            seen[name] = i
            f.write("    IFORM_%s = %d,\n" % (name, i))
        f.write("};\n\n")
        f.write("constexpr uint32_t kInstrFormCount = %d;\n" % len(forms))
        f.write("#endif // VX_INSTR_FORM_IDS_H\n")


def _q(x):
    """Cuantiza a centesimas enteras (evita 0.5 != 0.5000001 en las claves)."""
    return int(round(x * 100))


def write_vxarch(path, spec, id_of, timing, xml_date, xml_hash):
    """Serializa la microarq con dedup por SchedulerClass (latencias sin colapsar).

    @param id_of   { iform: InstrFormID }.
    @param timing  { iform: MicroArchInstr } de esta microarq.
    """
    port_index, port_order = {}, []

    def grp_idx(g):
        if g not in port_index:
            port_index[g] = len(port_order)
            port_order.append(g)
        return port_index[g]

    class_of = {}    # clave cuantizada -> class_id
    classes = []     # class_id -> (edges, recip_tp, uops, mc, mf, div, ports)
    iform_class = []
    for uid, t in timing.items():
        if uid not in id_of:
            continue
        edges = tuple(sorted((e.start_operand, e.target_operand, e.kind,
                              _q(e.cycles), int(e.upper_bound))
                             for e in t.latencies))
        ports_norm = tuple(sorted((grp_idx(p.port_group), _q(p.uops))
                                  for p in t.ports))
        key = (edges, _q(t.recip_tp), t.uops, int(t.microcoded),
               int(t.macro_fusible), _q(t.div_cycles), ports_norm)
        cid = class_of.get(key)
        if cid is None:
            cid = len(classes)
            class_of[key] = cid
            classes.append((edges, t.recip_tp, t.uops, int(t.microcoded),
                            int(t.macro_fusible), t.div_cycles, ports_norm))
        iform_class.append((id_of[uid], cid))

    iform_class.sort()
    with open(path, "w", encoding="ascii", newline="\n") as f:
        f.write("vxarch %d name=%s family=%s isa=x86 uops_arch=%s date=%s "
                "xml_sha256=%s classes=%d mapped=%d\n"
                % (DB_FORMAT_VERSION, spec.canonical_name, spec.family, "x86",
                   xml_date, xml_hash, len(classes), len(iform_class)))
        f.write("ports: " + " ".join("%d=%s" % (i, g)
                                     for i, g in enumerate(port_order)) + "\n")
        f.write("# class|recip_tp|uops|microcoded|macro_fusible|div_cycles"
                "|latencies(so:to:kind:cyc[:ub],...)|ports(idx*uops,...)\n")
        for cid, (edges, tp, u, mc, mf, div, pn) in enumerate(classes):
            lat = ",".join("%d:%d:%s:%.2f%s" % (so, to, kind, c / 100.0,
                                                ":ub" if ub else "")
                           for so, to, kind, c, ub in edges) or "-"
            ports = ",".join("%d*%.2f" % (gi, w / 100.0) for gi, w in pn) or "-"
            f.write("class %d|%.2f|%d|%d|%d|%.2f|%s|%s\n"
                    % (cid, tp, u, mc, mf, div, lat, ports))
        f.write("# iform_id|class_id\n")
        for fid, cid in iform_class:
            f.write("%d|%d\n" % (fid, cid))


def main():
    if len(sys.argv) < 3:
        sys.exit("uso: python build_database.py <instructions.xml> <dir_salida> "
                 "[overlay.def]")
    xml_path, out_dir = sys.argv[1], sys.argv[2]
    overlay_path = sys.argv[3] if len(sys.argv) > 3 else None
    os.makedirs(out_dir, exist_ok=True)

    print("[build_database] hash SHA256 del XML...")
    xml_hash = _sha256(xml_path)
    print("[build_database] parseando %s (streaming)..." % xml_path)
    report = {}
    forms, timings, xml_date = uops_info.parse(xml_path, MICROARCHS, report)

    # IDs ESTABLES: orden lexicografico por uid (no orden de aparicion).
    forms.sort(key=lambda fm: fm.uid)
    id_of = {fm.uid: i for i, fm in enumerate(forms)}
    valid = set(id_of)

    overlay = load_overlay(overlay_path, valid)

    write_vxisa(os.path.join(out_dir, "x86.vxisa"), forms, overlay, xml_date,
                xml_hash)
    write_ids_header(os.path.join(out_dir, "instr_form_ids.h"), forms)
    arch_stats = []
    for spec in MICROARCHS:
        p = os.path.join(out_dir, spec.canonical_name + ".vxarch")
        tmap = timings.get(spec.xml_name, {})
        write_vxarch(p, spec, id_of, tmap, xml_date, xml_hash)
        arch_stats.append((spec.canonical_name, len(tmap)))

    # ---- informe final ----
    print("\n==== informe de importacion ====")
    print("XML fecha        : %s" % xml_date)
    print("XML sha256       : %s" % xml_hash[:16] + "...")
    print("formas (iform)   : %d" % report.get("forms", 0))
    print("  op. implicitos : %d" % report.get("implicit_operands", 0))
    print("  op. suppressed : %d" % report.get("suppressed_operands", 0))
    print("mediciones       : %d" % report.get("measurements", 0))
    print("iform dup. diverg: %d" % report.get("dup_iform_divergent", 0))
    if overlay_path:
        print("overlay          : %d iform (validado iform+propiedad)"
              % len(overlay))
    for name, n in arch_stats:
        print("timing %-20s: %d iform" % (name, n))
    unk = report.get("unknown_measurement_fields", [])
    if unk:
        print("CAMPOS XML NO MANEJADOS (revisar): %s" % ", ".join(unk))
    print("salida           : %s" % out_dir)


if __name__ == "__main__":
    main()
