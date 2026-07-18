#!/usr/bin/env python3
"""Serializa la DB de instrucciones a CODIGO C++ (tablas estaticas) para que el
compilador la lleve DENTRO -- autocontenida, sin archivos externos, legible y
rapida (indexado directo, el compilador la optimiza a .rodata).

La consumen el analisis de asm (ASA), el LSP (hover) y el optimizer JIT/AOT
(scheduling consciente de latencias + puertos superescalares).

Este primer paso emite las FORMAS de una ISA (sintaxis: mnemonico + operandos +
overlay + mascaras r/w), indexadas por FormID, con un pool de strings interned y
un pool de operandos.  El coste por microarquitectura se anyade despues.

    python tools/import/gen_cpp_db.py <arch-data-dir> <isa> <salida.inc>
    #   isa = x86 (por ahora)
"""
import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import database  # noqa: E402

# clave de ISA -> (.vxisa rel, subdir de .vxarch, filtro a32).  El filtro a32:
# None = todas; False = excluir *-a32; True = solo *-a32 (ARM comparte carpeta).
_ISA = {
    "x86": ("x86/x86.vxisa", "x86", None),
    "arm64": ("arm/arm.vxisa", "arm", False),
    "arm32": ("arm/arm32.vxisa", "arm", True),
    "riscv": ("riscv/riscv.vxisa", "riscv", None),
}

# kind de operando -> entero estable (debe casar con el enum C++ DbOpKind).
_KIND = {"reg": 0, "mem": 1, "imm": 2, "agen": 3, "relbr": 4, "absbr": 5,
         "flags": 6}
# etiquetas de overlay -> bit (debe casar con el enum C++ DbOverlay).
_OVL = {"barrier": 0, "serializing": 1, "atomic": 2, "ll_sc": 3,
        "mem_acquire": 4, "mem_release": 5, "mem_seq_cst": 6, "no_reorder": 7,
        "branch": 8, "call": 9, "ret": 10, "syscall": 11}


class Interner:
    """Pool de strings deduplicado; devuelve indices estables."""
    def __init__(self):
        self.items = []
        self.idx = {}

    def get(self, s):
        i = self.idx.get(s)
        if i is None:
            i = len(self.items)
            self.idx[s] = i
            self.items.append(s)
        return i


def _cesc(s):
    """Escapa una string para un literal C."""
    return s.replace("\\", "\\\\").replace('"', '\\"')


def _f(x):
    """Literal float C++ valido (garantiza punto o exponente antes de la 'f';
    `1f` no es valido, `1.0f` si).  Sanea inf/nan."""
    s = repr(float(x))
    if s in ("inf", "-inf", "nan"):
        s = "0.0"
    if "." not in s and "e" not in s and "E" not in s:
        s += ".0"
    return s + "f"


def _max_lat(lat):
    """Latencia maxima (proxy del camino critico del nodo) del campo latencias
    `so:to:kind:cyc[:ub],...`."""
    if lat in ("", "-"):
        return 0.0
    mx = 0.0
    for e in lat.split(","):
        p = e.split(":")
        if len(p) >= 4:
            try:
                mx = max(mx, float(p[3]))
            except ValueError:
                pass
    return mx


def gen_cost(root, isa, out):
    """Emite el .cpp de COSTE por microarquitectura (latencia + puertos)."""
    vxisa_rel, subdir, a32 = _ISA[isa]
    forms = database.load_vxisa(os.path.join(root, vxisa_rel))
    nforms = (max(forms) + 1) if forms else 0

    uarchs = []              # (name, port_names[], classes[], slots[], form_class[])
    for p in sorted(glob.glob(os.path.join(root, subdir, "*.vxarch"))):
        is_a32 = "-a32" in os.path.basename(p)
        if a32 is not None and bool(is_a32) != bool(a32):
            continue
        name, legend, classes, form_class = database.load_vxarch(p)
        slots = []           # AsmPortSlot pool de esta microarq
        cls_rows = []        # AsmClass
        for cid in range(len(classes)):
            c = classes[cid]
            po = len(slots)
            pc = 0
            if c["ports"] not in ("", "-"):
                for tok in c["ports"].split(","):
                    gi, uo = tok.split("*")
                    slots.append((int(gi), float(uo)))
                    pc += 1
            flags = (int(c["microcoded"]) | (int(c["macro_fusible"]) << 1))
            cls_rows.append((float(c["recip_tp"]), _max_lat(c["latencies"]),
                             float(c["div_cycles"]), int(c["uops"]) & 0xFFFF,
                             flags, po, pc))
        fc = [-1] * nforms
        for fid, cid in form_class.items():
            if 0 <= fid < nforms:
                fc[fid] = cid
        uarchs.append((name, list(legend), cls_rows, slots, fc))

    low = isa.lower().replace("-", "_")
    with open(out, "w", encoding="ascii", newline="\n") as f:
        f.write("// GENERADO por tools/import/gen_cpp_db.py -- NO editar a mano.\n")
        f.write("// Coste por microarquitectura %s (latencia + puertos).\n" % isa)
        f.write('#include "vx/instr_db.h"\n\n')
        f.write("namespace vx { namespace instr_db { namespace {\n\n")
        for i, (name, legend, cls_rows, slots, fc) in enumerate(uarchs):
            # legado de puertos (placeholder si vacio: C++ no admite array [0]).
            f.write("const char *const kPn%d[] = {" % i)
            f.write(",".join('"%s"' % _cesc(x) for x in legend) if legend
                    else '""')
            f.write("};\n")
            # pool de slots
            f.write("const AsmPortSlot kSl%d[] = {" % i)
            f.write(",".join("{%d,%s}" % (g, _f(u)) for g, u in slots) if slots
                    else "{0,0.0f}")
            f.write("};\n")
            # clases
            f.write("const AsmClass kCl%d[] = {\n" % i)
            for rt, la, dv, uo, fl, po, pc in cls_rows:
                f.write("  {%s,%s,%s,%d,%d,%d,%d},\n"
                        % (_f(rt), _f(la), _f(dv), uo, fl, po, pc))
            if not cls_rows:
                f.write("  {0.0f,0.0f,-1.0f,0,0,0,0},\n")
            f.write("};\n")
            # form_class (empaquetado, 32 por linea)
            f.write("const int16_t kFc%d[] = {\n" % i)
            for j in range(0, len(fc), 32):
                f.write("  " + ",".join(str(v) for v in fc[j:j + 32]) + ",\n")
            f.write("};\n\n")
        # tabla de microarquitecturas
        f.write("const MicroarchData kUarch[] = {\n")
        for i, (name, legend, cls_rows, slots, fc) in enumerate(uarchs):
            f.write('  {"%s", kPn%d, %d, kCl%d, %d, kSl%d, kFc%d, %d},\n'
                    % (_cesc(name), i, len(legend), i, len(cls_rows), i, i,
                       len(fc)))
        f.write("};\n\n} // namespace anonimo\n\n")
        f.write("const CostData &cost_%s() {\n" % low)
        f.write("  static const CostData d = {kUarch, %d};\n  return d;\n}\n\n"
                % len(uarchs))
        f.write("}} // namespace vx::instr_db\n")

    tot_fc = sum(len(u[4]) for u in uarchs)
    print("[gen_cpp_db] %s COSTE: %d microarq, %d form_class -> %s (%.1f MB)"
          % (isa, len(uarchs), tot_fc, out, os.path.getsize(out) / 1e6))


def main():
    if len(sys.argv) < 4:
        sys.exit("uso: python gen_cpp_db.py <arch-data-dir> <isa> <salida.cpp> "
                 "[forms|cost]")
    root, isa, out = sys.argv[1:4]
    mode = sys.argv[4] if len(sys.argv) > 4 else "forms"
    if mode == "cost":
        gen_cost(root, isa, out)
        return
    vxisa_rel = _ISA.get(isa, (os.path.join(isa, isa + ".vxisa"),))[0]
    forms = database.load_vxisa(os.path.join(root, vxisa_rel))
    nforms = (max(forms) + 1) if forms else 0

    strs = Interner()
    ops_pool = []            # (kind, width, flags) aplanado
    form_rows = []           # por FormID: campos empaquetados
    # indice iclass -> (primer_fid, count).  Las formas ya vienen ordenadas por
    # clave (iclass primero) -> mismas iclass contiguas.
    iclass_index = []        # (iclass_str_idx, first_fid, count)
    cur_ic = None
    cur_first = 0
    cur_n = 0

    for fid in range(nforms):
        fm = forms.get(fid)
        if fm is None:
            form_rows.append(None)
            continue
        ic = fm["iclass"]
        ic_idx = strs.get(ic)
        ext_idx = strs.get(fm["ext"])
        # overlay -> bitmask
        ovl = 0
        for t in fm["overlay"].split(","):
            if t in _OVL:
                ovl |= (1 << _OVL[t])
        # operandos -> pool
        ops_off = len(ops_pool)
        ops_cnt = 0
        if fm["operands"] != "-":
            for o in fm["operands"].split(";"):
                if not o:
                    continue
                _idx, kind, width, flags, _rs = o.split(",", 4)
                ops_pool.append((_KIND.get(kind, 7), int(width) & 0xFFFF,
                                 int(flags) & 0xFF))
                ops_cnt += 1
        memflags = (int(fm["mem"]) | (int(fm["imm"]) << 1) |
                    (int(fm["wflags"]) << 2) | (int(fm["rflags"]) << 3))
        form_rows.append((ic_idx, ext_idx, ovl,
                          int(fm["rmask"], 16) & 0xFF,
                          int(fm["wmask"], 16) & 0xFF, memflags,
                          ops_off, ops_cnt, strs.get(fm["opcode"])))
        # indice por iclass
        if ic != cur_ic:
            if cur_ic is not None:
                iclass_index.append((strs.get(cur_ic), cur_first, cur_n))
            cur_ic, cur_first, cur_n = ic, fid, 0
        cur_n += 1
    if cur_ic is not None:
        iclass_index.append((strs.get(cur_ic), cur_first, cur_n))
    iclass_index.sort(key=lambda r: strs.items[r[0]])   # binaria por nombre

    up = isa.upper().replace("-", "_")
    low = isa.lower().replace("-", "_")
    with open(out, "w", encoding="ascii", newline="\n") as f:
        f.write("// GENERADO por tools/import/gen_cpp_db.py -- NO editar a mano.\n")
        f.write("// DB de instrucciones %s embebida (formas: sintaxis+overlay).\n"
                % isa)
        f.write('#include "vx/instr_db.h"\n\n')
        f.write("namespace vx { namespace instr_db { namespace {\n\n")
        # pool de strings
        f.write("const char *const kStr[] = {\n")
        for s in strs.items:
            f.write('  "%s",\n' % _cesc(s))
        f.write("};\n\n")
        # pool de operandos
        f.write("const DbOperand kOps[] = {\n")
        for k, w, fl in ops_pool:
            f.write("  {%d,%d,%d},\n" % (k, w, fl))
        f.write("};\n\n")
        # formas por FormID
        f.write("const DbForm kForms[] = {\n")
        for r in form_rows:
            if r is None:
                f.write("  {0,0,0,0,0,0,0,0,0},\n")  # hueco (FormID sin forma)
            else:
                f.write("  {%d,%d,%d,%d,%d,%d,%d,%d,%d},\n" % r)
        f.write("};\n\n")
        # indice por iclass (ordenado)
        f.write("const DbIclassRange kIclassIndex[] = {\n")
        for ic_idx, first, n in iclass_index:
            f.write("  {%d,%d,%d},\n" % (ic_idx, first, n))
        f.write("};\n\n")
        f.write("} // namespace anonimo\n\n")
        # accesor: rellena IsaData con las tablas de arriba.
        f.write("const IsaData &db_%s() {\n" % low)
        f.write("  static const IsaData d = {\n")
        f.write("    kStr, %d, kOps, %d, kForms, %d, kIclassIndex, %d};\n"
                % (len(strs.items), len(ops_pool), len(form_rows),
                   len(iclass_index)))
        f.write("  return d;\n}\n\n")
        f.write("}} // namespace vx::instr_db\n")

    print("[gen_cpp_db] %s: %d formas, %d strings, %d operandos, %d iclass -> %s (%.1f MB)"
          % (isa, len(form_rows), len(strs.items), len(ops_pool),
             len(iclass_index), out, os.path.getsize(out) / 1e6))


if __name__ == "__main__":
    main()
