/**
 * @file instr_db.cpp
 * @brief Emparejador texto->FormID sobre la DB de instrucciones EMBEBIDA.
 *
 * Las tablas por-ISA las genera @c tools/import/gen_cpp_db.py (ficheros
 * @c gen/instr_db_<isa>_gen.inc, incluidos aqui como @c .rodata estatica).  El
 * @c match espeja el emparejador del analizador: rango del iclass por busqueda
 * binaria + puntuacion por clase/ancho de operando.
 */
#include "vx/instr_db.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace vx {
namespace instr_db {

namespace {

/// Tablas de una ISA (las rellena el accesor @c db_<isa>() del .cpp generado).
IsaData tables_for(Isa isa) {
    switch (isa) {
    case Isa::X86:
        return db_x86();
    case Isa::ARM64:
        return db_arm64();
    case Isa::ARM32:
        return db_arm32();
    case Isa::RISCV:
        return db_riscv();
    }
    return {};
}

/// Operandos EXPLICITOS de una forma para puntuar (descarta implicit/suppressed
/// y los operandos de flags, que el usuario no escribe).
void explicit_ops(const IsaData &t, const DbForm &f,
                  std::vector<const DbOperand *> &out) {
    out.clear();
    for (unsigned i = 0; i < f.ops_count; ++i) {
        const DbOperand &o = t.ops[f.ops_off + i];
        if (o.flags & 0x0C) continue;          // implicit(bit2) | suppressed(bit3)
        if (o.kind == OP_FLAGS) continue;
        out.push_back(&o);
    }
}

/// Puntua los operandos del usuario contra los de la forma; -1 si no casan.
int score_ops(const std::vector<ParsedOp> &user,
              const std::vector<const DbOperand *> &form) {
    if (user.size() != form.size()) return -1;
    int s = 0;
    for (size_t i = 0; i < form.size(); ++i) {
        const ParsedOp &u = user[i];
        const DbOperand &fo = *form[i];
        if (u.kind != fo.kind) return -1;
        if (u.width && fo.width) {
            if (u.width != fo.width) return -1;
            s += 2;
        } else {
            s += 1;
        }
    }
    return s;
}

/// Busca el rango de FormIDs de un mnemonico (binaria sobre kIclassIndex,
/// ordenado por nombre).  Devuelve nullptr si no existe.
const DbIclassRange *find_iclass(const IsaData &t, const std::string &up) {
    unsigned lo = 0, hi = t.iclass_count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        const DbIclassRange &r = t.iclass[mid];
        int c = std::strcmp(t.str[r.iclass], up.c_str());
        if (c == 0) return &r;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return nullptr;
}

} // namespace

int32_t match(Isa isa, const std::string &mnemonic,
              const std::vector<ParsedOp> &ops) {
    const IsaData t = tables_for(isa);
    if (!t.forms) return -1;
    std::string up = mnemonic;
    for (char &c : up) c = static_cast<char>(std::toupper((unsigned char)c));
    const DbIclassRange *r = find_iclass(t, up);
    if (!r) return -1;                          // mnemonico no existe
    int32_t best = -1;
    int best_s = -1;
    std::vector<const DbOperand *> fo;
    for (uint32_t fid = r->first_fid; fid < r->first_fid + r->count; ++fid) {
        explicit_ops(t, t.forms[fid], fo);
        int s = score_ops(ops, fo);
        if (s > best_s) {
            best_s = s;
            best = static_cast<int32_t>(fid);
        }
    }
    // El mnemonico existe: si nada caso por operandos, vale la primera del rango.
    return best >= 0 ? best : static_cast<int32_t>(r->first_fid);
}

const char *iclass_name(Isa isa, int32_t form_id) {
    const IsaData t = tables_for(isa);
    if (!t.forms || form_id < 0 ||
        static_cast<unsigned>(form_id) >= t.form_count)
        return "";
    return t.str[t.forms[form_id].iclass];
}

uint16_t overlay_of(Isa isa, int32_t form_id) {
    const IsaData t = tables_for(isa);
    if (!t.forms || form_id < 0 ||
        static_cast<unsigned>(form_id) >= t.form_count)
        return 0;
    return t.forms[form_id].overlay;
}

uint32_t form_count(Isa isa) { return tables_for(isa).form_count; }

} // namespace instr_db
} // namespace vx
