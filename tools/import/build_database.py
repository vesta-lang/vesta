#!/usr/bin/env python3
"""Construye la base de datos de instrucciones de Vesta desde el IR comun.

Orquesta el pipeline Importer -> IR -> optimize -> Serializer y escribe los
ficheros propios de Vesta:

    x86.vxisa              sintaxis por-ISA (semantic_id + operandos completos +
                           overlay semantico).
    intel-skylake.vxarch   modelo de scheduling por microarq (SchedulerClasses
    intel-alderlake-p.vxarch   deduplicadas + mapeo forma->clase).
    amd-zen4.vxarch
    instr_form_ids.h       enum InstrFormID + kInstrFormCount (indice denso para
                           arrays) + su semantic_id de 64 bits (identidad estable).

Identidad: cada forma tiene un @ref ir.semantic_id (hash de la firma estructural
COMPLETA -- iclass, opcode, extension, encoding, todos los operandos) que NO
depende del nombre iform (la fuente lo renombra) ni se rompe si la fuente anade
un campo.  El indice denso (0..N, orden por semantic_id) es para arrays; el
semantic_id es la referencia estable entre versiones.

Nota de formato: hoy la salida es TEXTO (depurable).  El binario (fread en el
runtime) y el split PortClass/LatencyClass de las SchedulerClasses son el paso
siguiente, ya validada la extraccion.

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
import optimize      # noqa: E402
import uops_info     # noqa: E402

DB_FORMAT_VERSION = 1

MICROARCHS = [
    ir.MicroArchSpec("SKL", "intel-skylake", "intel"),
    ir.MicroArchSpec("ADL-P", "intel-alderlake-p", "intel"),  # Golden Cove
    ir.MicroArchSpec("ZEN4", "amd-zen4", "amd"),
]

VALID_OVERLAY_PROPS = {
    "serializing", "barrier", "atomic", "ll_sc", "branch", "call", "ret",
    "syscall", "stack_push", "stack_pop", "mem_acquire", "mem_release",
    "mem_seq_cst", "may_fault", "privileged", "no_reorder",
}

# Propiedades de overlay MUTUAMENTE EXCLUYENTES (una entrada no puede llevar las
# dos: call+ret, atomic+ll_sc en x86, push+pop, acquire+release=usa seq_cst).
INCOMPATIBLE_OVERLAY = [
    ("call", "ret"), ("stack_push", "stack_pop"), ("atomic", "ll_sc"),
    ("mem_acquire", "mem_release"),
]


def _sha256(path):
    """SHA256 del fichero fuente (identidad exacta, no solo la fecha)."""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _sanitize_ident(s):
    """A-Z0-9_ ; el resto -> _  (para el enum C)."""
    return "".join(c if (c.isalnum() or c == "_") else "_" for c in s).upper()


def load_overlay(path, valid_iforms):
    """Lee el overlay semantico y lo VALIDA (iform, propiedad, incompatibilidad).

    Formato: la iform en su linea; las propiedades indentadas.  Devuelve
    { iform: set(propiedades) }.  Aborta si algo no cuadra."""
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
    for iform, props in overlay.items():
        for a, b in INCOMPATIBLE_OVERLAY:
            if a in props and b in props:
                errors.append("  iform %s: propiedades incompatibles '%s' + '%s'"
                              % (iform, a, b))
    if errors:
        sys.stderr.write("[build_database] overlay invalido:\n")
        sys.stderr.write("\n".join(errors) + "\n")
        sys.exit(1)
    return overlay


def _op_field(operands):
    """Operandos completos en una columna: idx,kind,width,flags,regset;...
    flags = r|w<<1|impl<<2|supp<<3.  El regset se sanea (separadores ->'/')."""
    parts = []
    for o in operands:
        flags = (int(o.read) | (int(o.write) << 1) |
                 (int(o.implicit) << 2) | (int(o.suppressed) << 3))
        rs = o.register_set.replace(";", "/").replace("|", "/").replace(",", "/")
        rs = rs.replace(" ", "_") or "-"
        parts.append("%d,%s,%d,%d,%s" % (o.idx, o.kind or "-", o.width, flags, rs))
    return ";".join(parts) or "-"


def write_vxisa(path, forms, sid_of, overlay, xml_date, xml_hash):
    """Serializa la sintaxis por-ISA + semantic_id + overlay validado.

    @param forms   lista YA ordenada (indice i == InstrFormID denso).
    @param sid_of  { form_id: semantic_id }.
    """
    with open(path, "w", encoding="ascii", newline="\n") as f:
        f.write("vxisa %d isa=x86 source=uops.info date=%s forms=%d "
                "xml_sha256=%s\n"
                % (DB_FORMAT_VERSION, xml_date, len(forms), xml_hash))
        f.write("# id|semantic_id|uid|iclass|ext|opcode|enc|rmask|wmask|mem|imm"
                "|wflags|rflags|operands|overlay\n")
        f.write("# id = indice denso (arrays); semantic_id = identidad estable; "
                "uid/iclass = doc.  operands: idx,kind,width,flags,regset\n")
        for i, fm in enumerate(forms):
            ov = ",".join(sorted(overlay.get(fm.iform, []))) or "-"
            enc = ",".join("%s=%s" % (k, fm.enc[k]) for k in sorted(fm.enc)) or "-"
            f.write("%d|%016x|%s|%s|%s|%s|%s|0x%x|0x%x|%d|%d|%d|%d|%s|%s\n" % (
                i, sid_of[i], fm.uid, fm.iclass or "-", fm.extension or "-",
                fm.opcode or "-", enc, fm.read_mask, fm.write_mask,
                int(fm.has_mem), int(fm.has_imm),
                int(fm.writes_flags), int(fm.reads_flags),
                _op_field(fm.operands), ov))


def write_ids_header(path, forms, sid_of):
    """Genera instr_form_ids.h: enum InstrFormID (indice denso) + kInstrFormCount
    + la tabla de semantic_id (identidad estable de 64 bits)."""
    seen = {}
    with open(path, "w", encoding="ascii", newline="\n") as f:
        f.write("/* GENERADO por tools/import/build_database.py -- NO editar.\n"
                " * InstrFormID = indice denso (arrays); kInstrSemanticId[] = su\n"
                " * identidad estable de 64 bits (independiente del nombre). */\n")
        f.write("#ifndef VX_INSTR_FORM_IDS_H\n#define VX_INSTR_FORM_IDS_H\n")
        f.write("#include <cstdint>\n\n")
        f.write("enum InstrFormID : uint32_t {\n")
        for i, fm in enumerate(forms):
            name = _sanitize_ident(fm.uid)
            if name in seen:
                name = "%s__%d" % (name, i)
            seen[name] = i
            f.write("    IFORM_%s = %d,\n" % (name, i))
        f.write("};\n\n")
        f.write("constexpr uint32_t kInstrFormCount = %d;\n\n" % len(forms))
        f.write("constexpr uint64_t kInstrSemanticId[kInstrFormCount] = {\n")
        for i in range(len(forms)):
            f.write("    0x%016xULL,\n" % sid_of[i])
        f.write("};\n")
        f.write("#endif // VX_INSTR_FORM_IDS_H\n")


def write_vxarch(path, sched, xml_date, xml_hash):
    """Serializa una @ref ir.ArchSchedule ya deduplicada por @c optimize."""
    spec = sched.spec
    with open(path, "w", encoding="ascii", newline="\n") as f:
        f.write("vxarch %d name=%s family=%s isa=x86 uops_arch=%s date=%s "
                "xml_sha256=%s classes=%d mapped=%d\n"
                % (DB_FORMAT_VERSION, spec.canonical_name, spec.family,
                   spec.xml_name, xml_date, xml_hash, len(sched.classes),
                   len(sched.form_class)))
        f.write("ports: " + " ".join("%d=%s" % (i, g)
                                     for i, g in enumerate(sched.port_names))
                + "\n")
        f.write("# class|recip_tp|uops|microcoded|macro_fusible|div_cycles"
                "|latencies(so:to:kind:cyc[:ub],...)|ports(idx*uops,...)\n")
        for cid, c in enumerate(sched.classes):
            lat = ",".join("%d:%d:%d:%.2f%s" % (e.start_operand, e.target_operand,
                                                e.kind, e.cycles,
                                                ":ub" if e.upper_bound else "")
                           for e in c.latencies) or "-"
            ports = ",".join("%d*%.2f" % (p.group, p.uops) for p in c.ports) or "-"
            f.write("class %d|%.2f|%d|%d|%d|%.2f|%s|%s\n"
                    % (cid, c.recip_tp, c.uops, int(c.microcoded),
                       int(c.macro_fusible), c.div_cycles, lat, ports))
        f.write("# form_id|class_id\n")
        for fid, cid in sched.form_class:
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

    # Indice denso ESTABLE: orden por semantic_id (contenido, no el nombre).
    forms.sort(key=ir.semantic_id)
    sid_of = {i: ir.semantic_id(fm) for i, fm in enumerate(forms)}
    id_of = {sid_of[i]: i for i in range(len(forms))}  # semantic_id -> form_id
    valid_iforms = {fm.iform for fm in forms}

    overlay = load_overlay(overlay_path, valid_iforms)

    write_vxisa(os.path.join(out_dir, "x86.vxisa"), forms, sid_of, overlay,
                xml_date, xml_hash)
    write_ids_header(os.path.join(out_dir, "instr_form_ids.h"), forms, sid_of)
    arch_stats = []
    for spec in MICROARCHS:
        sched = optimize.build_arch_schedule(spec, id_of,
                                             timings.get(spec.xml_name, {}))
        write_vxarch(os.path.join(out_dir, spec.canonical_name + ".vxarch"),
                     sched, xml_date, xml_hash)
        arch_stats.append((spec.canonical_name, len(sched.form_class),
                           len(sched.classes)))

    print("\n==== informe de importacion ====")
    print("XML fecha        : %s" % xml_date)
    print("XML sha256       : %s..." % xml_hash[:16])
    print("formas unicas    : %d" % report.get("forms", 0))
    print("  op. implicitos : %d" % report.get("implicit_operands", 0))
    print("  op. suppressed : %d" % report.get("suppressed_operands", 0))
    print("filas dup. exact.: %d" % report.get("exact_dup_rows", 0))
    print("colisiones hash  : %d" % report.get("hash_collisions", 0))
    print("mediciones       : %d" % report.get("measurements", 0))
    if overlay_path:
        print("overlay          : %d iform (iform+prop+incompat validados)"
              % len(overlay))
    for name, mapped, ncl in arch_stats:
        print("sched %-20s: %d formas -> %d clases" % (name, mapped, ncl))
    unk = report.get("unknown_measurement_fields", [])
    if unk:
        print("CAMPOS XML NO MANEJADOS (revisar): %s" % ", ".join(unk))
    print("salida           : %s" % out_dir)


if __name__ == "__main__":
    main()
