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
 * @file asm_analyze.cpp
 * @brief Implementacion del resumen de efectos de bloque de un inline asm
 *        (ver asm_analyze.h).
 */
#include "vx/asm/asm_analyze.h"

#include <cctype>
#include <cstdlib>

#include "vx/asm/asm_effects.h"

namespace vx {

namespace {

/// Pasa @p s a minusculas (ASCII).
std::string lower(const std::string &s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s)
        o.push_back(static_cast<char>(std::tolower((unsigned char)c)));
    return o;
}

/// Trocea una linea NASM en tokens: separadores whitespace y coma; los
/// corchetes/operandos se mantienen como tokens sueltos (basta para detectar
/// el mnemonico y los registros; el analisis fino de operandos es trabajo
/// posterior (reconstruccion de CFG + dataflow).
std::vector<std::string> tokenize_line(const std::string &line) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    };
    for (char c : line) {
        if (std::isspace((unsigned char)c) || c == ',') {
            flush();
        } else {
            cur.push_back(c);
        }
    }
    flush();
    return out;
}

/**
 * @brief Trocea los OPERANDOS de una linea, respetando los corchetes.
 *
 * Hace falta para saber QUE operando es el de memoria, y con ello si la
 * instruccion lo lee o lo escribe.  No vale trocear por comas a secas: la coma
 * de `[rbx + rcx*8]` pertenece al modo de direccionamiento, no separa
 * operandos.
 *
 * @param linea Linea completa, ya sin comentarios.
 * @param mnem  Mnemonico en minusculas, tal como se reconocio.
 * @return Los operandos en orden textual, sin espacios alrededor.
 */
std::vector<std::string> operandos_de(const std::string &linea,
                                      const std::string &mnem) {
    std::vector<std::string> out;
    // Situarse justo detras del mnemonico (comparando en minusculas).
    std::string baja;
    baja.reserve(linea.size());
    for (char c : linea) baja.push_back((char)std::tolower((unsigned char)c));
    const size_t p = baja.find(mnem);
    if (p == std::string::npos) return out;
    size_t i = p + mnem.size();

    std::string cur;
    int prof = 0; // profundidad de corchetes
    auto cerrar = [&] {
        size_t a = cur.find_first_not_of(" \t");
        size_t b = cur.find_last_not_of(" \t");
        if (a != std::string::npos) out.push_back(cur.substr(a, b - a + 1));
        cur.clear();
    };
    for (; i < linea.size(); ++i) {
        const char c = linea[i];
        if (c == '[') ++prof;
        if (c == ']' && prof > 0) --prof;
        if (c == ',' && prof == 0) {
            cerrar();
            continue;
        }
        cur.push_back(c);
    }
    cerrar();
    return out;
}

/// @c true si @p mnem es una rama/salto (para @c has_branch).  Cubre x86
/// (jmp/jCC/loop) y arm64 (b/b.CC/cbz/cbnz/tbz/tbnz).
bool es_rama(const std::string &mnem) {
    if (mnem.empty()) return false;
    if (mnem == "jmp" || mnem == "loop" || mnem == "loope" ||
        mnem == "loopne")
        return true;
    if (mnem[0] == 'j' && mnem.size() >= 2) return true; // jCC (je/jne/jg/...)
    // arm64: b, b.CC, bl, br, cbz, cbnz, tbz, tbnz.
    if (mnem == "b" || mnem == "bl" || mnem == "br" || mnem == "cbz" ||
        mnem == "cbnz" || mnem == "tbz" || mnem == "tbnz")
        return true;
    if (mnem.size() > 2 && mnem[0] == 'b' && mnem[1] == '.')
        return true; // b.ne / b.eq / ...
    return false;
}

/// @c true si @p mnem es una atomica de arm64 (LL/SC o CAS/RMW/swap): base para
/// @c has_atomic.  Se reconoce por prefijo para cubrir todas las variantes de
/// orden/ancho (ldaxr/ldaxrb/..., cas/casa/casal/..., ldadd*/ldset*/swp*).
bool es_atomica_arm(const std::string &mnem) {
    static const char *pref[] = {"ldaxr", "ldxr",  "ldar", "stlxr", "stxr",
                                 "stlr",  "ldaxp", "ldxp", "stlxp", "stxp",
                                 "cas",   "swp",   "ldadd", "ldset", "ldclr",
                                 "ldeor"};
    for (const char *p : pref)
        if (mnem.rfind(p, 0) == 0) return true;
    return false;
}

/// Ancho (bytes) del slot de pila de un @c push/@c pop segun el arch x86.
int stack_word_x86(const std::string &arch) {
    if (arch == "x86_16") return 2;
    if (arch == "x86") return 4; // x86-32
    return 8;                     // x86_64
}

/// Delta de pila EXPLICITO de una instruccion, en bytes (positivo = mas marco).
/// x86: @c push/@c pop (ancho por arch) y @c sub/@c add sobre @c rsp/@c esp/
/// @c sp; arm64: @c sub/@c add sobre @c sp.  Pone @p ok=false si mueve el
/// puntero de pila con un inmediato NO literal (conservador: no acotable).
int64_t stack_delta(const std::vector<std::string> &toks, size_t mi,
                    const std::string &arch, bool &ok) {
    ok = true;
    const std::string m = lower(toks[mi]);
    const bool x86 = (arch.rfind("x86", 0) == 0 || arch.empty());
    if (x86) {
        const int w = stack_word_x86(arch);
        if (m == "push") return w;
        if (m == "pop") return -w;
    }
    // sub/add <sp>, <imm> (x86: rsp/esp/sp; arm64: `sub sp, sp, #imm`).
    if ((m == "sub" || m == "add") && mi + 2 < toks.size()) {
        const std::string dst = lower(toks[mi + 1]);
        const bool is_sp =
            (dst == "rsp" || dst == "esp" || dst == "sp");
        if (is_sp) {
            // arm64: `sub sp, sp, #imm` -> el inmediato es el 3er operando y
            // lleva '#'.  x86: `sub rsp, imm` -> 2o operando.
            std::string imm = (!x86 && mi + 3 < toks.size())
                                   ? toks[mi + 3]
                                   : toks[mi + 2];
            if (!imm.empty() && imm[0] == '#') imm = imm.substr(1);
            char *end = nullptr;
            const long v = std::strtol(imm.c_str(), &end, 0);
            if (end != imm.c_str() && *end == '\0') {
                const int64_t d = static_cast<int64_t>(v);
                return (m == "sub") ? d : -d;
            }
            ok = false; // sp movido con algo no literal.
            return 0;
        }
    }
    return 0;
}

} // namespace

AsmBlockEffects asm_analyze_block(const std::string &nasm_body,
                                  const std::string &arch) {
    AsmBlockEffects res;
    // Marco de pila: seguimos el maximo alcanzado (peor caso), no el neto: un
    // `sub rsp,32; ...; add rsp,32` reserva 32 aunque acabe en 0.
    int64_t cur_frame = 0;
    int64_t max_frame = 0;

    size_t i = 0;
    const std::string &b = nasm_body;
    while (i <= b.size()) {
        size_t eol = b.find('\n', i);
        if (eol == std::string::npos) eol = b.size();
        std::string line = b.substr(i, eol - i);
        i = eol + 1;

        // Comentarios ';' y '//'.
        size_t cpos = line.find(';');
        if (cpos != std::string::npos) line = line.substr(0, cpos);
        size_t slpos = line.find("//");
        if (slpos != std::string::npos) line = line.substr(0, slpos);

        const bool line_has_mem = line.find('[') != std::string::npos;

        auto toks = tokenize_line(line);
        if (toks.empty()) continue;

        // Saltar labels ("name:") y prefijos; detectar el prefijo `lock`
        // (barrera atomica) por el camino.
        size_t ti = 0;
        bool lock_prefix = false;
        while (ti < toks.size()) {
            const std::string &t = toks[ti];
            if (!t.empty() && t.back() == ':') { // label
                ++ti;
                continue;
            }
            const std::string lt = lower(t);
            if (lt == "lock") {
                lock_prefix = true;
                ++ti;
                continue;
            }
            if (lt == "rep" || lt == "repe" || lt == "repz" || lt == "repne" ||
                lt == "repnz" || lt == "bnd") {
                ++ti;
                continue;
            }
            break;
        }
        if (ti >= toks.size()) continue; // linea solo-label/prefijo

        const std::string mnem = lower(toks[ti]);

        if (lock_prefix || line_has_mem) res.touches_mem = true;
        // Prefijo `lock` (x86) o una atomica de arm64 (LL/SC o CAS/RMW/swap) =
        // efecto atomico (barrera).
        if (lock_prefix || es_atomica_arm(mnem)) res.has_atomic = true;
        if (es_rama(mnem)) res.has_branch = true;

        // Marco de pila explicito.
        bool sd_ok = true;
        const int64_t d = stack_delta(toks, ti, arch, sd_ok);
        if (!sd_ok) {
            // El puntero de pila se movio con algo no literal: no acotable.
            res.unknown_mnemonics.push_back(mnem + " sp,<no-literal>");
        } else if (d != 0) {
            cur_frame += d;
            if (cur_frame > max_frame) max_frame = cur_frame;
        }

        // Efectos por-instruccion segun la tabla del arch.  Un mnemonico no
        // tabulado -> a la lista de desconocidos para que el caller de un error
        // claro (disciplina "crecer bajo demanda, error claro siempre").
        const AsmEffects eff = asm_effects_for(mnem, arch);
        if (!eff.known) {
            res.unknown_mnemonics.push_back(mnem);
            continue;
        }
        if (eff.touches_mem) res.touches_mem = true;
        if (eff.touches_flags) res.touches_flags = true;
        if (eff.is_call) res.is_call = true;

        /* Leer y escribir memoria no es lo mismo, y la tabla ya sabe cual de
         * las dos: @c operand_write_mask dice QUE operandos escribe la
         * instruccion, asi que basta con mirar en que posicion esta el de
         * memoria.  Se apunta lo justo, y ante cualquier duda las dos cosas --
         * decir de menos aqui seria dejar reordenar algo que no se puede. */
        if (lock_prefix || line_has_mem || eff.touches_mem) {
            bool lee = true, escribe = true; // por defecto, lo conservador
            if (!lock_prefix && !eff.touches_mem && line_has_mem) {
                // Memoria EXPLICITA (`[...]` en un operando) y sin prefijo
                // atomico: se puede precisar si se identifica cual es.
                const std::vector<std::string> ops = operandos_de(line, mnem);
                int idx_mem = -1;
                bool varios = false;
                for (size_t k = 0; k < ops.size(); ++k)
                    if (ops[k].find('[') != std::string::npos) {
                        if (idx_mem >= 0) varios = true;
                        else idx_mem = static_cast<int>(k);
                    }
                if (idx_mem >= 0 && !varios && idx_mem < 8) {
                    escribe = ((eff.operand_write_mask >> idx_mem) & 1u) != 0u;
                    lee = !escribe || (eff.operand_write_mask == 0u);
                    /* Un operando que se escribe puede ademas leerse (`add
                     * [rdi], rax` acumula).  Solo un destino PURO -- el `mov`
                     * -- no lee lo que pisa, y distinguirlo no compensa el
                     * riesgo: si escribe, se cuenta tambien como lectura. */
                    if (escribe) lee = true;
                }
            }
            if (lee) res.reads_mem = true;
            if (escribe) res.writes_mem = true;
        }
    }

    res.explicit_stack_bytes = max_frame;
    return res;
}

} // namespace vx
