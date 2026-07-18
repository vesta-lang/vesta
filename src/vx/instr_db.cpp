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
#include <initializer_list>

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

namespace {

/// Ancho (bits) de un registro segun la ISA (0 = desconocido/no restringe).
/// Espeja @c regWidth del analizador (x86 exacto; ARM/RISC-V el ancho real,
/// pero las formas ARM/RISC-V llevan width 0 = "cualquiera", asi que el score
/// no lo restringe).
uint16_t reg_width(Isa isa, const std::string &r) {
    auto pref = [&](const char *p) {
        size_t n = std::strlen(p);
        return r.size() > n && r.compare(0, n, p) == 0 &&
               std::isdigit((unsigned char)r[n]);
    };
    if (isa == Isa::ARM64 || isa == Isa::ARM32) {
        if (r == "sp" || r == "xzr" || r == "lr") return 64;
        if (r == "wzr" || r == "wsp" || r == "pc") return 32;
        if (!r.empty() && (r[0] == 'x') && std::isdigit((unsigned char)r[1]))
            return 64;
        if (!r.empty() && (r[0] == 'w') && std::isdigit((unsigned char)r[1]))
            return 32;
        if (!r.empty() && (r[0] == 'r') && std::isdigit((unsigned char)r[1]))
            return 32; // A32
        if (pref("q") || pref("v")) return 128;
        if (pref("d")) return 64;
        if (pref("s")) return 32;
        if (pref("h")) return 16;
        if (pref("b")) return 8;
        return 0;
    }
    if (isa == Isa::RISCV) {
        static const char *abi[] = {
            "zero", "ra", "sp", "gp", "tp", "fp", "t0", "t1", "t2", "t3", "t4",
            "t5", "t6", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8",
            "s9", "s10", "s11", "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7"};
        for (const char *a : abi)
            if (r == a) return 64;
        if ((r[0] == 'x' || r[0] == 'f') && r.size() > 1 &&
            std::isdigit((unsigned char)r[1]))
            return 64;
        if (r.size() >= 2 && (r.compare(0, 2, "ft") == 0 ||
                              r.compare(0, 2, "fs") == 0 ||
                              r.compare(0, 2, "fa") == 0))
            return 64;
        return 0;
    }
    // x86: anchos exactos.
    auto is = [&](std::initializer_list<const char *> l) {
        for (auto s : l)
            if (r == s) return true;
        return false;
    };
    if (is({"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp"}) ||
        (pref("r") && (r.back() != 'd' && r.back() != 'w' && r.back() != 'b')))
        return 64;
    if (is({"eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp"}) ||
        (pref("r") && r.back() == 'd'))
        return 32;
    if (is({"ax", "bx", "cx", "dx", "si", "di", "bp", "sp"}) ||
        (pref("r") && r.back() == 'w'))
        return 16;
    if (is({"al", "ah", "bl", "bh", "cl", "ch", "dl", "dh", "sil", "dil",
            "bpl", "spl"}) ||
        (pref("r") && r.back() == 'b'))
        return 8;
    if (pref("xmm")) return 128;
    if (pref("ymm")) return 256;
    if (pref("zmm")) return 512;
    if (pref("k")) return 64;
    return 0;
}

} // namespace

ParsedOp parse_operand(Isa isa, const std::string &token) {
    std::string t = token;
    // trim.
    while (!t.empty() && std::isspace((unsigned char)t.front())) t.erase(0, 1);
    while (!t.empty() && std::isspace((unsigned char)t.back())) t.pop_back();
    std::string low = t;
    for (char &c : low) c = static_cast<char>(std::tolower((unsigned char)c));

    // RISC-V: memoria como desplazamiento(reg): 0(a0), -4(sp).
    if (isa == Isa::RISCV) {
        size_t op = low.find('(');
        if (op != std::string::npos && low.back() == ')')
            return ParsedOp{OP_MEM, 0};
    }
    // x86/ARM: memoria [...].
    if (low.find('[') != std::string::npos) return ParsedOp{OP_MEM, 0};
    // inmediato ARM (#imm) o numero.
    std::string num = low;
    if (!num.empty() && num[0] == '#') num.erase(0, 1);
    if (!num.empty() &&
        (std::isdigit((unsigned char)num[0]) ||
         ((num[0] == '-' || num[0] == '+') && num.size() > 1)))
        return ParsedOp{OP_IMM, 0};
    // registro.
    uint16_t w = reg_width(isa, low);
    if (w || (!low.empty() && std::isalpha((unsigned char)low[0])))
        return ParsedOp{OP_REG, w};
    return ParsedOp{OP_IMM, 0};
}

int32_t match_asm_line(Isa isa, const std::string &line) {
    // quita comentarios (; // #) y espacios.
    std::string s = line;
    size_t cm = s.find_first_of(";");
    if (cm != std::string::npos) s.resize(cm);
    size_t sl = s.find("//");
    if (sl != std::string::npos) s.resize(sl);
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(0, 1);
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    if (s.empty() || s.back() == ':') return -1;   // vacia o label
    // mnemonico = primer token.
    size_t sp = s.find_first_of(" \t");
    std::string mnem = sp == std::string::npos ? s : s.substr(0, sp);
    std::string rest = sp == std::string::npos ? "" : s.substr(sp + 1);
    // trocea operandos por comas respetando [...] y (...).
    std::vector<ParsedOp> ops;
    std::string cur;
    int depth = 0;
    auto flush = [&]() {
        // trim.
        std::string c = cur;
        while (!c.empty() && std::isspace((unsigned char)c.front())) c.erase(0, 1);
        while (!c.empty() && std::isspace((unsigned char)c.back())) c.pop_back();
        if (!c.empty()) ops.push_back(parse_operand(isa, c));
        cur.clear();
    };
    for (char c : rest) {
        if (c == '[' || c == '(')
            ++depth;
        else if (c == ']' || c == ')')
            --depth;
        if (c == ',' && depth == 0)
            flush();
        else
            cur += c;
    }
    flush();
    return match(isa, mnem, ops);
}

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

// -------------------------------------------------------------------------
// Capa de coste: latencia + puertos por microarquitectura.
// -------------------------------------------------------------------------

namespace {
CostData cost_for(Isa isa) {
    switch (isa) {
    case Isa::X86:
        return cost_x86();
    case Isa::ARM64:
        return cost_arm64();
    case Isa::ARM32:
        return cost_arm32();
    case Isa::RISCV:
        return cost_riscv();
    }
    return {};
}
} // namespace

uint32_t microarch_count(Isa isa) { return cost_for(isa).count; }

const char *microarch_name(Isa isa, uint32_t ua_id) {
    const CostData c = cost_for(isa);
    return ua_id < c.count ? c.uarchs[ua_id].name : "";
}

int32_t microarch_by_name(Isa isa, const std::string &name) {
    const CostData c = cost_for(isa);
    for (uint32_t i = 0; i < c.count; ++i)
        if (name == c.uarchs[i].name) return static_cast<int32_t>(i);
    return -1;
}

AsmCost cost(Isa isa, int32_t form_id, uint32_t ua_id) {
    AsmCost r;
    const CostData c = cost_for(isa);
    if (ua_id >= c.count || form_id < 0) return r;
    const MicroarchData &m = c.uarchs[ua_id];
    if (static_cast<uint32_t>(form_id) >= m.form_count) return r;
    int16_t cid = m.form_class[form_id];
    if (cid < 0) return r;                       // no cronometrada en esta uarch
    const AsmClass &cl = m.classes[cid];
    r.found = true;
    r.recip_tp = cl.recip_tp;
    r.latency = cl.latency;
    r.div_cycles = cl.div_cycles;
    r.uops = cl.uops;
    r.microcoded = (cl.flags & 0x1) != 0;
    r.macro_fusible = (cl.flags & 0x2) != 0;
    r.ports = m.slots + cl.ports_off;
    r.ports_count = cl.ports_count;
    r.port_names = m.port_names;
    return r;
}

// -------------------------------------------------------------------------
// Capa de features por CPU (que extensiones de ISA admite cada core).
// -------------------------------------------------------------------------

namespace {
FeatData feat_for(Isa isa) {
    switch (isa) {
    case Isa::X86:
        return feat_x86();
    case Isa::ARM64:
    case Isa::ARM32:
        return feat_arm();          // A64 y A32/T32 comparten features.
    case Isa::RISCV:
        return feat_riscv();
    }
    return {};
}
} // namespace

uint32_t cpu_count(Isa isa) { return feat_for(isa).cpu_count; }

const char *cpu_name(Isa isa, uint32_t cpu_id) {
    const FeatData f = feat_for(isa);
    return cpu_id < f.cpu_count ? f.cpus[cpu_id].name : "";
}

int32_t cpu_by_name(Isa isa, const std::string &name) {
    const FeatData f = feat_for(isa);
    for (uint32_t i = 0; i < f.cpu_count; ++i)
        if (name == f.cpus[i].name) return static_cast<int32_t>(i);
    return -1;
}

bool cpu_has_feature(Isa isa, uint32_t cpu_id, const std::string &feature) {
    const FeatData f = feat_for(isa);
    if (cpu_id >= f.cpu_count) return false;
    const CpuFeatures &c = f.cpus[cpu_id];
    for (uint16_t i = 0; i < c.feat_count; ++i)
        if (feature == f.feat_names[c.feats[i]]) return true;
    return false;
}

AsmBlockCost analyze_asm_cost(Isa isa, const std::string &body,
                              uint32_t ua_id) {
    AsmBlockCost out;
    // presion por puerto acumulada por NOMBRE (dos instrucciones distintas
    // pueden compartir grupo de puertos).
    std::vector<std::pair<std::string, float>> pressure;
    auto add_port = [&](const std::string &name, float uops) {
        for (auto &p : pressure)
            if (p.first == name) {
                p.second += uops;
                return;
            }
        pressure.emplace_back(name, uops);
    };
    float tp_sum = 0.0f;

    size_t i = 0;
    while (i <= body.size()) {
        size_t nl = body.find('\n', i);
        std::string line =
            body.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
        i = (nl == std::string::npos) ? body.size() + 1 : nl + 1;
        int32_t fid = match_asm_line(isa, line);
        if (fid < 0) {
            // ¿linea con contenido pero mnemonico desconocido?  cuenta como
            // instruccion no emparejada (para la completitud).
            std::string t = line;
            size_t c = t.find_first_of(";");
            if (c != std::string::npos) t.resize(c);
            size_t s2 = t.find("//");
            if (s2 != std::string::npos) t.resize(s2);
            while (!t.empty() && std::isspace((unsigned char)t.front()))
                t.erase(0, 1);
            while (!t.empty() && std::isspace((unsigned char)t.back()))
                t.pop_back();
            if (!t.empty() && t.back() != ':') ++out.instr_count;
            continue;
        }
        ++out.instr_count;
        ++out.matched;
        AsmCost c = cost(isa, fid, ua_id);
        if (!c.found) continue;
        ++out.costed;
        out.total_uops += c.uops;
        out.latency_sum += c.latency;
        tp_sum += c.recip_tp;
        for (uint8_t k = 0; k < c.ports_count; ++k) {
            const AsmPortSlot &ps = c.ports[k];
            add_port(c.port_names[ps.port], ps.uops);
        }
    }
    // throughput = max(puerto mas cargado, suma de recip_tp) -- cota inferior de
    // ciclos del bloque bien planificado (ejecucion paralela por puertos).
    float max_port = 0.0f;
    for (const auto &p : pressure) max_port = std::max(max_port, p.second);
    out.throughput = std::max(max_port, tp_sum);
    out.port_pressure = std::move(pressure);
    return out;
}

} // namespace instr_db
} // namespace vx
