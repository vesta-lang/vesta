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
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import database  # noqa: E402

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


def main():
    if len(sys.argv) < 4:
        sys.exit("uso: python gen_cpp_db.py <arch-data-dir> <isa> <salida.inc>")
    root, isa, out = sys.argv[1:4]
    forms = database.load_vxisa(os.path.join(root, isa, isa + ".vxisa"))
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
