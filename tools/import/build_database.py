#!/usr/bin/env python3
"""Orquestador del pipeline de la base de datos de instrucciones de Vesta.

    XML (uops.info) --uops_info--> IR --optimize--> IR optimizado --serialize--> ficheros

Este modulo SOLO orquesta: hashea el XML, invoca al importador, ABORTA si el
esquema del XML cambio (atributos desconocidos), asigna IDs + deduplica via
@c optimize, carga/valida el overlay semantico, y llama al @c serialize.  Ni
parsea XML, ni normaliza puertos, ni deduplica, ni escribe formato.

Salida: x86.vxisa, <microarq>.vxarch, instr_form_ids.h.  Nota: hoy TEXTO
(depurable); el binario (fread) y el split PortClass/LatencyClass son el paso
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
import serialize     # noqa: E402
import uops_info     # noqa: E402

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
INCOMPATIBLE_OVERLAY = [
    ("call", "ret"), ("stack_push", "stack_pop"), ("atomic", "ll_sc"),
    ("mem_acquire", "mem_release"),
]


def _sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def load_overlay(path, valid_iforms):
    """Overlay semantico VALIDADO (iform existe, propiedad valida, sin pares
    incompatibles).  Aborta si algo no cuadra."""
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


def main():
    if len(sys.argv) < 3:
        sys.exit("uso: python build_database.py <instructions.xml> <dir_salida> "
                 "[overlay.def]")
    xml_path, out_dir = sys.argv[1], sys.argv[2]
    overlay_path = sys.argv[3] if len(sys.argv) > 3 else None
    os.makedirs(out_dir, exist_ok=True)

    print("[build_database] hash SHA256 del XML...")
    xml_hash = _sha256(xml_path)
    print("[build_database] importando %s (streaming)..." % xml_path)
    report = {}
    forms, timings, xml_date, unknown = uops_info.parse(xml_path, MICROARCHS,
                                                        report)
    if unknown:
        sys.stderr.write("[build_database] el esquema del XML cambio -- "
                         "atributos DESCONOCIDOS (anadir conscientemente):\n")
        for u in sorted(unknown):
            sys.stderr.write("  %s\n" % u)
        sys.exit(1)

    # optimize: identidad (FormID denso) + timings deduplicados por microarq.
    forms, key_to_id = optimize.assign_ids(forms)
    valid_iforms = {fm.iform for fm in forms}
    overlay = load_overlay(overlay_path, valid_iforms)

    serialize.write_vxisa(os.path.join(out_dir, "x86.vxisa"), forms, overlay,
                          xml_date, xml_hash)
    serialize.write_ids_header(os.path.join(out_dir, "instr_form_ids.h"), forms)
    arch_stats = []
    for spec in MICROARCHS:
        sched = optimize.build_arch_schedule(spec, key_to_id,
                                             timings.get(spec.xml_name, {}),
                                             len(forms))
        serialize.write_vxarch(os.path.join(out_dir,
                                            spec.canonical_name + ".vxarch"),
                               sched, xml_date, xml_hash)
        arch_stats.append((spec.canonical_name,
                           sum(1 for c in sched.form_class if c >= 0),
                           len(sched.classes)))

    print("\n==== informe de importacion ====")
    print("XML fecha        : %s" % xml_date)
    print("XML sha256       : %s..." % xml_hash[:16])
    print("esquema id       : %d" % ir.SEMANTIC_SCHEMA)
    print("formas unicas    : %d" % report.get("forms", 0))
    print("  op. implicitos : %d" % report.get("implicit_operands", 0))
    print("  op. suppressed : %d" % report.get("suppressed_operands", 0))
    print("filas dup. exact.: %d" % report.get("exact_dup_rows", 0))
    print("mediciones       : %d" % report.get("measurements", 0))
    if overlay_path:
        print("overlay          : %d iform (iform+prop+incompat validados)"
              % len(overlay))
    for name, mapped, ncl in arch_stats:
        print("sched %-20s: %d formas -> %d clases" % (name, mapped, ncl))
    print("salida           : %s" % out_dir)


if __name__ == "__main__":
    main()
