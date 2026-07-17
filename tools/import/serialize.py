#!/usr/bin/env python3
"""Etapa SERIALIZE: escribe los ficheros de la base de datos desde el IR.

Fase PURAMENTE de escritura -- no valida, no normaliza, no deduplica (eso ya lo
hizo @c optimize).  Genera:
    x86.vxisa            sintaxis por-ISA (FormID + operandos + encoding + overlay).
    <microarq>.vxarch    modelo de scheduling (clases + form_class).
    instr_form_ids.h     enum InstrFormID + kInstrFormCount + kInstrChecksum[].
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ir  # noqa: E402

DB_FORMAT_VERSION = 1
_FLAG_R, _FLAG_W, _FLAG_I, _FLAG_S = 1, 2, 4, 8


def _uid(form):
    """Nombre humano DERIVADO (iform + anchos explicitos) -- solo doc/enum."""
    ws = [str(o.width) for o in form.operands
          if not o.suppressed and o.kind in ("reg", "mem", "imm")]
    return form.iform if not ws else form.iform + "/" + "x".join(ws)


def _sanitize_ident(s):
    return "".join(c if (c.isalnum() or c == "_") else "_" for c in s).upper()


def _op_field(operands):
    parts = []
    for o in operands:
        flags = (int(o.read) | (int(o.write) << 1) |
                 (int(o.implicit) << 2) | (int(o.suppressed) << 3))
        rs = o.register_set.replace(";", "/").replace("|", "/").replace(",", "/")
        rs = rs.replace(" ", "_") or "-"
        parts.append("%d,%s,%d,%d,%s" % (o.idx, o.kind or "-", o.width, flags, rs))
    return ";".join(parts) or "-"


def write_vxisa(path, forms, overlay, xml_date, xml_hash):
    with open(path, "w", encoding="ascii", newline="\n") as f:
        f.write("vxisa %d schema=%d isa=x86 source=uops.info date=%s forms=%d "
                "xml_sha256=%s\n" % (DB_FORMAT_VERSION, ir.SEMANTIC_SCHEMA,
                                     xml_date, len(forms), xml_hash))
        f.write("# id|checksum|uid|iclass|ext|opcode|enc|rmask|wmask|mem|imm"
                "|wflags|rflags|operands|overlay\n")
        f.write("# id = FormID denso (arrays); checksum = FNV1a64 de la clave "
                "(verificacion); uid/iclass = doc.  operands: idx,kind,width,"
                "flags,regset  (flags=r|w<<1|impl<<2|supp<<3)\n")
        for i, fm in enumerate(forms):
            ov = ",".join(sorted(overlay.get(fm.iform, []))) or "-"
            enc = ",".join("%s=%s" % (k, v) for k, v in fm.enc.nonempty()) or "-"
            f.write("%d|%016x|%s|%s|%s|%s|%s|0x%x|0x%x|%d|%d|%d|%d|%s|%s\n" % (
                i, ir.checksum(fm), _uid(fm), fm.iclass or "-",
                fm.extension or "-", fm.opcode or "-", enc,
                fm.read_mask, fm.write_mask, int(fm.has_mem), int(fm.has_imm),
                int(fm.writes_flags), int(fm.reads_flags),
                _op_field(fm.operands), ov))


def write_ids_header(path, forms):
    seen = {}
    with open(path, "w", encoding="ascii", newline="\n") as f:
        f.write("/* GENERADO por tools/import/build_database.py -- NO editar.\n"
                " * InstrFormID = indice denso (arrays); kInstrChecksum[] = FNV1a64\n"
                " * de la clave estructural (verificacion, no identidad). */\n")
        f.write("#ifndef VX_INSTR_FORM_IDS_H\n#define VX_INSTR_FORM_IDS_H\n")
        f.write("#include <cstdint>\n\n")
        f.write("constexpr uint32_t kInstrSemanticSchema = %d;\n\n"
                % ir.SEMANTIC_SCHEMA)
        f.write("enum InstrFormID : uint32_t {\n")
        for i, fm in enumerate(forms):
            name = _sanitize_ident(_uid(fm))
            if name in seen:
                name = "%s__%d" % (name, i)
            seen[name] = i
            f.write("    IFORM_%s = %d,\n" % (name, i))
        f.write("};\n\n")
        f.write("constexpr uint32_t kInstrFormCount = %d;\n\n" % len(forms))
        f.write("constexpr uint64_t kInstrChecksum[kInstrFormCount] = {\n")
        for fm in forms:
            f.write("    0x%016xULL,\n" % ir.checksum(fm))
        f.write("};\n")
        f.write("#endif // VX_INSTR_FORM_IDS_H\n")


def write_vxarch(path, sched, xml_date, xml_hash):
    spec = sched.spec
    mapped = sum(1 for c in sched.form_class if c >= 0)
    with open(path, "w", encoding="ascii", newline="\n") as f:
        f.write("vxarch %d name=%s family=%s isa=x86 uops_arch=%s date=%s "
                "xml_sha256=%s classes=%d mapped=%d\n"
                % (DB_FORMAT_VERSION, spec.canonical_name, spec.family,
                   spec.xml_name, xml_date, xml_hash, len(sched.classes),
                   mapped))
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
        for fid, cid in enumerate(sched.form_class):
            if cid >= 0:
                f.write("%d|%d\n" % (fid, cid))
