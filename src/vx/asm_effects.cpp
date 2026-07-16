/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file asm_effects.cpp
 * @brief Phase AS inc.4: implementacion de la inferencia PROPIA de clobbers.
 *
 * Sin dependencias de Keystone/Capstone.  Tabla plana mnemonic->efectos +
 * tokenizador ligero del cuerpo NASM Intel.
 */

#include "vx/asm_effects.h"
#include "vx/parser.h" // get_aot_condcomp_target: el arch del TARGET, no del host

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace vx {

std::string asm_normalize_numbers(const std::string &body) {
    std::string out;
    out.reserve(body.size());
    const size_t n = body.size();
    auto is_id = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '_';
    };
    size_t i = 0;
    while (i < n) {
        const char c = body[i];
        /* Identificador (incl. registros r8/r15/xmm0): copiar el token
         * entero -> sus digitos NO son un literal numerico. */
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_') {
            size_t j = i;
            while (j < n && is_id(body[j]))
                ++j;
            out.append(body, i, j - i);
            i = j;
            continue;
        }
        /* Literal numerico: empieza por digito (char previo no-identificador).
         */
        if (c >= '0' && c <= '9') {
            int base = 10;
            size_t j = i;
            if (c == '0' && i + 1 < n) {
                const char p = body[i + 1];
                if (p == 'x' || p == 'X') {
                    base = 16;
                    j = i + 2;
                } else if (p == 'b' || p == 'B') {
                    base = 2;
                    j = i + 2;
                } else if (p == 'o' || p == 'O') {
                    base = 8;
                    j = i + 2;
                }
            }
            unsigned long long val = 0;
            bool any = false;
            while (j < n) {
                const char d = body[j];
                int dv;
                if (d == '_') {
                    ++j;
                    continue;
                } // separador de digitos
                if (d >= '0' && d <= '9')
                    dv = d - '0';
                else if (d >= 'a' && d <= 'f')
                    dv = d - 'a' + 10;
                else if (d >= 'A' && d <= 'F')
                    dv = d - 'A' + 10;
                else
                    break;
                if (dv >= base) break;
                val = val * static_cast<unsigned long long>(base) +
                      static_cast<unsigned long long>(dv);
                any = true;
                ++j;
            }
            if (!any) {
                out.push_back(c);
                ++i;
                continue;
            } // "0" suelto
            char buf[32];
            std::snprintf(buf, sizeof(buf), "0x%llx", val);
            out += buf;
            i = j;
            continue;
        }
        out.push_back(c);
        ++i;
    }
    return out;
}

// ---------------------------------------------------------------------------
//  Canonicalizacion de registros: UNA por arquitectura.
//
//  Cada arquitectura tiene sus nombres y sus reglas de alias, y no se parecen
//  en nada: mezclarlas en una sola funcion hace que anadir la tercera sea un
//  suplicio y que los nombres de una se cuelen en otra (`x0` no existe en x86,
//  `rax` no existe en ARM, y aceptar el ajeno es un error que hay que dar, no
//  tragarse).  Aqui cada una tiene su funcion, y @ref kArchRegs las asocia con
//  el valor de `arch:` del target.  Anadir RISC-V es anadir su funcion y su
//  fila; no se toca nada de lo demas.
//
//  El criterio comun -- lo unico que comparten -- es que los alias de ANCHO
//  colapsan al registro FISICO: `eax`->`rax` y `w3`->`x3` son la misma idea.
// ---------------------------------------------------------------------------

namespace {

/// Registros de x86-64.  Canonico: el nombre de 64 bits (`rax`, `r10`) y `vN`
/// para el banco vectorial (xmm/ymm/zmm N son la misma pista).
std::string canon_x86_64(const std::string &r) {
    // Registros de proposito general: cada entrada lista todos los alias
    // de ancho que mapean al mismo fisico de 64 bits.
    struct GpEntry {
        const char *canon;
        const char *aliases[6];
    };
    static const GpEntry gp[] = {
        {"rax", {"rax", "eax", "ax", "al", "ah", nullptr}},
        {"rbx", {"rbx", "ebx", "bx", "bl", "bh", nullptr}},
        {"rcx", {"rcx", "ecx", "cx", "cl", "ch", nullptr}},
        {"rdx", {"rdx", "edx", "dx", "dl", "dh", nullptr}},
        {"rsi", {"rsi", "esi", "si", "sil", nullptr}},
        {"rdi", {"rdi", "edi", "di", "dil", nullptr}},
        {"rbp", {"rbp", "ebp", "bp", "bpl", nullptr}},
        {"rsp", {"rsp", "esp", "sp", "spl", nullptr}},
    };
    for (const auto &e : gp) {
        for (const char *const *a = e.aliases; *a; ++a) {
            if (r == *a) return e.canon;
        }
    }
    // r8..r15 con sufijos d/w/b (32/16/8 bits) -> canonico rN.
    if (r.size() >= 2 && r[0] == 'r' && std::isdigit((unsigned char)r[1])) {
        std::string num;
        size_t i = 1;
        while (i < r.size() && std::isdigit((unsigned char)r[i]))
            num.push_back(r[i++]);
        std::string suf = r.substr(i);
        if (!num.empty() &&
            (suf.empty() || suf == "d" || suf == "w" || suf == "b")) {
            int n = std::atoi(num.c_str());
            if (n >= 8 && n <= 15) return "r" + num;
        }
    }
    // Vectoriales xmm/ymm/zmm N (0..31) -> canonico vN (mismo banco).
    for (const char *pfx : {"xmm", "ymm", "zmm"}) {
        const size_t plen = std::strlen(pfx);
        if (r.size() > plen && r.compare(0, plen, pfx) == 0) {
            std::string num = r.substr(plen);
            bool all_dig = !num.empty();
            for (char c : num) {
                if (!std::isdigit((unsigned char)c)) {
                    all_dig = false;
                    break;
                }
            }
            if (all_dig) {
                int n = std::atoi(num.c_str());
                if (n >= 0 && n <= 31) return "v" + num;
            }
        }
    }
    return std::string();
}

/// Todos los digitos y no vacio.
bool solo_digitos(const std::string &s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!std::isdigit((unsigned char)c)) return false;
    return true;
}

/// Registros de AArch64.  Canonico: el nombre de 64 bits (`x3`) y `vN` para el
/// banco SIMD/FP.  Nada que ver con los de x86: aqui `w3` es la mitad baja de
/// `x3` (como `eax` de `rax`) y `b0`/`h0`/`s0`/`d0`/`q0`/`v0` son la misma
/// pista SIMD vista con distinto ancho.
std::string canon_arm64(const std::string &r) {
    if (r == "xzr" || r == "wzr") return "xzr";              // registro cero
    if (r == "sp" || r == "wsp") return "sp";                // stack pointer
    if (r == "lr" || r == "x30" || r == "w30") return "x30"; // link register
    if (r == "fp" || r == "x29" || r == "w29") return "x29"; // frame pointer

    // x0..x30 / w0..w30 -> xN.
    if (r.size() >= 2 && (r[0] == 'x' || r[0] == 'w')) {
        const std::string num = r.substr(1);
        if (solo_digitos(num)) {
            const int n = std::atoi(num.c_str());
            if (n >= 0 && n <= 30) return "x" + num;
        }
    }
    // Banco SIMD/FP: b/h/s/d/q/v N (0..31) -> vN.  Se admite la forma con lane
    // (`v0.4s`), que nombra al mismo fisico.
    if (r.size() >= 2 && (r[0] == 'b' || r[0] == 'h' || r[0] == 's' ||
                          r[0] == 'd' || r[0] == 'q' || r[0] == 'v')) {
        std::string num = r.substr(1);
        const size_t punto = num.find('.');
        if (punto != std::string::npos) num = num.substr(0, punto);
        if (solo_digitos(num)) {
            const int n = std::atoi(num.c_str());
            if (n >= 0 && n <= 31) return "v" + num;
        }
    }
    return std::string();
}

/// Que canonicalizador usa cada valor de `arch:`.  Anadir una arquitectura es
/// anadir su funcion y su fila aqui.
struct ArchRegs {
    const char *arch;
    std::string (*canon)(const std::string &);
};
const ArchRegs kArchRegs[] = {
    {"x86_64", &canon_x86_64},
    {"x86", &canon_x86_64}, // mismo banco (los de 64 bits sobran, y el
                            // ensamblador ya los rechaza por su cuenta)
    {"arm64", &canon_arm64},
};

/// La arquitectura del host, para cuando no hay override de target.
const char *arch_host() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "x86_64";
#endif
}

} // namespace

std::string asm_canonical_reg(const std::string &raw) {
    std::string r;
    r.reserve(raw.size());
    for (char c : raw)
        r.push_back((char)std::tolower((unsigned char)c));

    // Se despacha por el TARGET, no por el host: una variante
    // @Target("arch:arm64") se compila con los registros de ARM aunque el build
    // corra en x86.  Sin esto, `register("x0")` era "registro no reconocido" y
    // las variantes arm64 no habian compilado nunca.
    std::string os, arch;
    get_aot_condcomp_target(os, arch);
    if (arch.empty()) arch = arch_host();
    for (const auto &a : kArchRegs)
        if (arch == a.arch) return a.canon(r);
    return std::string();
}

// -----------------------------------------------------------------------
// Tabla plana mnemonic -> AsmEffects.  Subset comun, extensible.  Los
// registros van en forma canonica.  Construida una vez (lazy) en un
// unordered_map; el lookup es compile-time (no hot path runtime).
// -----------------------------------------------------------------------
AsmEffects asm_effects_for(const std::string &mnemonic) {
    static const std::unordered_map<std::string, AsmEffects> table = [] {
        std::unordered_map<std::string, AsmEffects> t;
        auto add = [&t](const char *m, AsmEffects e) {
            e.known = true;
            t[m] = std::move(e);
        };
        // Helpers de construccion.
        auto E = [](std::initializer_list<const char *> wr, bool wfo, bool mem,
                    bool flags, bool call = false) {
            AsmEffects e;
            for (const char *w : wr)
                e.implicit_write.emplace_back(w);
            e.writes_first_operand = wfo;
            e.touches_mem = mem;
            e.touches_flags = flags;
            e.is_call = call;
            return e;
        };
        // --- Instrucciones con efectos IMPLICITOS sobre registros ---
        add("rdtsc", E({"rax", "rdx"}, false, false, false));
        add("rdtscp", E({"rax", "rdx", "rcx"}, false, false, false));
        add("cpuid", E({"rax", "rbx", "rcx", "rdx"}, false, false, false));
        add("rdpmc", E({"rax", "rdx"}, false, false, false));
        add("rdrand", E({}, true, false, true)); // escribe dest + CF
        add("rdseed", E({}, true, false, true));
        // mul/imul 1-operando + div/idiv: rdx:rax.
        add("mul", E({"rax", "rdx"}, false, false, true));
        add("imul",
            E({"rax", "rdx"}, false, false, true)); // forma 1-op (conservador)
        add("div", E({"rax", "rdx"}, false, false, true));
        add("idiv", E({"rax", "rdx"}, false, false, true));
        add("cqo", E({"rdx"}, false, false, false));
        add("cdq", E({"rdx"}, false, false, false));
        // syscall (Linux): clobber rcx, r11 + caller-saved via is_call.
        add("syscall", E({"rcx", "r11"}, false, true, true, /*call=*/true));
        add("sysenter", E({}, false, true, true, true));
        // call/ret: call clobbera caller-saved (is_call).
        add("call", E({}, false, true, true, /*call=*/true));
        // --- Aritmetica/bitwise que escribe el 1er operando + flags ---
        add("add", E({}, true, false, true));
        add("sub", E({}, true, false, true));
        add("adc", E({}, true, false, true));
        add("sbb", E({}, true, false, true));
        add("and", E({}, true, false, true));
        add("or", E({}, true, false, true));
        add("xor", E({}, true, false, true));
        add("neg", E({}, true, false, true));
        add("inc", E({}, true, false, true)); // no CF, pero conservador
        add("dec", E({}, true, false, true));
        add("shl", E({}, true, false, true));
        add("shr", E({}, true, false, true));
        add("sar", E({}, true, false, true));
        add("rol", E({}, true, false, true));
        add("ror", E({}, true, false, true));
        add("rcl", E({}, true, false, true));
        add("rcr", E({}, true, false, true));
        add("imul2", E({}, true, false, true)); // alias interno; no real
        add("popcnt", E({}, true, false, true));
        add("lzcnt", E({}, true, false, true));
        add("tzcnt", E({}, true, false, true));
        add("bsf", E({}, true, false, true));
        add("bsr", E({}, true, false, true));
        add("bswap", E({}, true, false, false));
        // --- Movimientos / direcciones (escriben 1er operando, sin flags) ---
        add("mov", E({}, true, false, false));
        add("movzx", E({}, true, false, false));
        add("movsx", E({}, true, false, false));
        add("movsxd", E({}, true, false, false));
        add("lea", E({}, true, false, false));
        add("xchg",
            E({}, true, false, false)); // escribe ambos (conservador: 1er op)
        add("cmov", E({}, true, false, false));
        add("not", E({}, true, false, false)); // not no toca flags
        add("set", E({}, true, false, false)); // setcc: escribe 1er op (byte)
        // --- Comparaciones / test: solo flags ---
        add("cmp", E({}, false, false, true));
        add("test", E({}, false, false, true));
        // --- Pila: tocan memoria + rsp ---
        add("push", E({"rsp"}, false, true, false));
        add("pop", E({"rsp"}, true, true, false));
        // --- Control de flujo / no-ops: sin clobbers ---
        add("nop", E({}, false, false, false));
        add("ret", E({}, false, true, false));
        add("jmp", E({}, false, false, false));
        add("leave", E({"rsp", "rbp"}, false, true, false));
        // saltos condicionales: solo leen flags.
        for (const char *j : {"je",  "jne", "jz",  "jnz", "jg",  "jge", "jl",
                              "jle", "ja",  "jae", "jb",  "jbe", "jo",  "jno",
                              "js",  "jns", "jc",  "jnc", "jp",  "jnp"}) {
            add(j, E({}, false, false, false));
        }
        return t;
    }();

    std::string m;
    m.reserve(mnemonic.size());
    for (char c : mnemonic)
        m.push_back((char)std::tolower((unsigned char)c));
    auto it = table.find(m);
    if (it != table.end()) return it->second;
    // setcc (sete/setne/...) y cmovcc se reconocen por prefijo.
    if (m.rfind("set", 0) == 0 && m.size() > 3) {
        AsmEffects e;
        e.writes_first_operand = true;
        e.known = true;
        return e;
    }
    if (m.rfind("cmov", 0) == 0 && m.size() > 4) {
        AsmEffects e;
        e.writes_first_operand = true;
        e.known = true;
        return e;
    }
    AsmEffects unknown; // known=false
    return unknown;
}

namespace {
// Convierte un canonico (rax.., vN) al nombre que GCC espera en la
// clobber-list.  GP: tal cual (rax..r15).  Vectorial vN -> xmmN.
std::string canon_to_gcc(const std::string &canon) {
    if (!canon.empty() && canon[0] == 'v' && canon.size() > 1 &&
        std::isdigit((unsigned char)canon[1])) {
        return "xmm" + canon.substr(1);
    }
    return canon;
}

// Trocea una linea en tokens separados por espacios/comas/tabs.  Los
// corchetes y su contenido se conservan como parte del token (para
// detectar [mem]); aqui solo necesitamos el 1er token (mnemonico) y
// saber si el resto contiene un registro como primer operando.
std::vector<std::string> tokenize_line(const std::string &line) {
    std::vector<std::string> toks;
    std::string cur;
    for (char c : line) {
        if (c == ' ' || c == '\t' || c == ',') {
            if (!cur.empty()) {
                toks.push_back(cur);
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) toks.push_back(cur);
    return toks;
}
} // namespace

AsmInferResult asm_infer_clobbers(const std::string &nasm_body,
                                  const std::vector<std::string> &bound_canon) {
    AsmInferResult res;
    // Set de regs ligados (canonicos) a EXCLUIR de los clobbers.
    std::unordered_set<std::string> bound(bound_canon.begin(),
                                          bound_canon.end());
    // Acumulador canonico de clobbers (despues -> GCC names + filtrado).
    std::unordered_set<std::string> clob;

    // Caller-saved conservador (union SysV + Win64) para call/syscall.
    static const char *caller_saved[] = {"rax", "rcx", "rdx", "rsi", "rdi",
                                         "r8",  "r9",  "r10", "r11"};

    // Procesar linea a linea.
    size_t i = 0;
    const std::string &b = nasm_body;
    while (i <= b.size()) {
        // Extraer una linea [start, eol).
        size_t eol = b.find('\n', i);
        if (eol == std::string::npos) eol = b.size();
        std::string line = b.substr(i, eol - i);
        i = eol + 1;

        // Quitar comentarios ';' y '//'.
        size_t cpos = line.find(';');
        if (cpos != std::string::npos) line = line.substr(0, cpos);
        size_t slpos = line.find("//");
        if (slpos != std::string::npos) line = line.substr(0, slpos);

        // Detectar memoria: cualquier '[' en operandos.
        const bool line_has_mem = line.find('[') != std::string::npos;

        auto toks = tokenize_line(line);
        if (toks.empty()) continue;

        // Saltar labels iniciales ("name:" como token aislado) y prefijos.
        size_t ti = 0;
        while (ti < toks.size()) {
            const std::string &t = toks[ti];
            // Label: token que termina en ':'.
            if (!t.empty() && t.back() == ':') {
                ++ti;
                continue;
            }
            // Prefijos de instruccion.
            std::string lt;
            for (char c : t)
                lt.push_back((char)std::tolower((unsigned char)c));
            if (lt == "lock" || lt == "rep" || lt == "repe" || lt == "repz" ||
                lt == "repne" || lt == "repnz" || lt == "bnd") {
                ++ti;
                continue;
            }
            break;
        }
        if (ti >= toks.size()) continue; // linea solo-label/prefijo

        const std::string mnem = toks[ti];
        const AsmEffects eff = asm_effects_for(mnem);

        if (!eff.known) {
            res.unknown_mnemonics.push_back(mnem);
            // Conservador: marcar memoria + flags para no perder un efecto.
            res.clobber_memory = true;
            res.clobber_flags = true;
            continue;
        }

        // Registros escritos implicitamente.
        for (const auto &w : eff.implicit_write)
            clob.insert(w);

        // Primer operando escrito (si es un registro).
        if (eff.writes_first_operand && ti + 1 < toks.size()) {
            const std::string canon = asm_canonical_reg(toks[ti + 1]);
            if (!canon.empty()) clob.insert(canon);
        }

        // Memoria: implicita del mnemonico o '[' en la linea.
        if (eff.touches_mem || line_has_mem) res.clobber_memory = true;
        if (eff.touches_flags) res.clobber_flags = true;

        // call/syscall: clobber conservador de TODO el caller-saved.
        if (eff.is_call) {
            for (const char *cs : caller_saved)
                clob.insert(cs);
        }
    }

    // Excluir los regs ligados por register() (son operandos) + rsp/rbp
    // (nunca se pueden clobrear en GCC) y convertir a nombres GCC.
    for (const auto &c : clob) {
        if (bound.count(c)) continue;
        if (c == "rsp" || c == "rbp") continue;
        res.clobber_regs.push_back(canon_to_gcc(c));
    }
    std::sort(res.clobber_regs.begin(), res.clobber_regs.end());
    return res;
}

} // namespace vx
