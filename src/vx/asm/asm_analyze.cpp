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
#include <set>

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

/**
 * @brief Registro BASE de un operando de memoria, en forma canonica.
 *
 * Es el primer identificador dentro de los corchetes, y eso vale igual en las
 * dos sintaxis: `[rdi]`, `[rbx + rcx*8]` en x86 y `[x0]`, `[x0, #8]`,
 * `[x0, x1, lsl #3]` en arm64.  La canonicalizacion la hace
 * @c asm_canonical_reg, que ya despacha por la arquitectura del OBJETIVO.
 *
 * @param operando Texto del operando, con sus corchetes.
 * @param arch     Arquitectura del cuerpo que se analiza.  Se pasa EXPLICITA:
 *                 el cuerpo puede ser de una arquitectura distinta de la que se
 *                 este compilando (una variante por `@Target`), y entonces
 *                 canonicalizar con los registros del objetivo activo daria una
 *                 base vacia o de otro banco.
 * @return El registro canonico, o cadena vacia si no se pudo determinar
 *         (direccion absoluta, simbolo, expresion no reconocida).
 */
std::string base_de_memoria(const std::string &operando,
                            const std::string &arch) {
    const size_t a = operando.find('[');
    if (a == std::string::npos) return std::string();
    size_t i = a + 1;
    while (i < operando.size() && std::isspace((unsigned char)operando[i])) ++i;
    /* Marcador `$N`: el registro aun no esta elegido -- lo elige el asignador
     * despues --, pero el marcador YA identifica de que operando se trata, que
     * es justo lo que hace falta.  Se devuelve tal cual.
     *
     * Es ademas el camino MEJOR de los dos: no depende de nombres de registro,
     * asi que vale igual en cualquier arquitectura, y no puede confundirse con
     * otra variable que use el mismo registro.  Y es la forma que usa la
     * stdlib, o sea que sin esto la parte que mas importa quedaba fuera. */
    if (i < operando.size() && operando[i] == '$') {
        std::string ph = "$";
        for (++i; i < operando.size() && std::isdigit((unsigned char)operando[i]);
             ++i)
            ph.push_back(operando[i]);
        return ph.size() > 1 ? ph : std::string();
    }
    std::string ident;
    for (; i < operando.size(); ++i) {
        const char c = operando[i];
        if (std::isalnum((unsigned char)c) || c == '_') ident.push_back(c);
        else break;
    }
    if (ident.empty()) return std::string();
    return asm_canonical_reg(ident, arch);
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
    std::set<std::string> escritos; // registros que el bloque reescribe
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
        if (eff.port_io) res.has_port_io = true;
        if (eff.barrier) res.has_atomic = true; // ordena: misma consecuencia

        /* Leer y escribir memoria no es lo mismo, y la tabla ya sabe cual de
         * las dos: @c operand_write_mask dice QUE operandos escribe la
         * instruccion, asi que basta con mirar en que posicion esta el de
         * memoria.  Se apunta lo justo, y ante cualquier duda las dos cosas --
         * decir de menos aqui seria dejar reordenar algo que no se puede. */
        if (lock_prefix || line_has_mem || eff.touches_mem) {
            bool lee = true, escribe = true; // por defecto, lo conservador
            if (!lock_prefix && line_has_mem) {
                /* Memoria EXPLICITA (`[...]` en un operando) y sin prefijo
                 * atomico: se puede precisar si se identifica cual es.
                 *
                 * La condicion es tener CORCHETES, no que la instruccion toque
                 * memoria "implicitamente": en arm64 toda carga o almacen la
                 * toca por definicion y aun asi lleva su `[x0]` a la vista.
                 * Exigir lo segundo dejaba fuera a arm64 entero.  Lo que si
                 * queda fuera es la memoria SIN corchetes -- `push`, `call`,
                 * las de cadena --, que no se puede atribuir a nada. */
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
                    /* Por que registro se llega.  Si no se sabe, el bloque toca
                     * memoria que no se puede nombrar y hay que decirlo. */
                    const std::string base =
                        base_de_memoria(ops[static_cast<size_t>(idx_mem)], arch);
                    if (base.empty()) res.accesos_incompletos = true;
                    else res.accesos.push_back({base, escribe});
                    /* El registro base tiene que seguir valiendo lo que la
                     * ligadura dice.  Si el propio bloque lo reescribe antes
                     * -- `mov rdi, rsi` y luego `[rdi]` --, el acceso ya no va
                     * a donde apuntaba la variable, y atribuirselo seria
                     * mentir.  Se anota que registros escribe el bloque y al
                     * final se descartan los accesos que dependan de ellos. */
                } else {
                    res.accesos_incompletos = true;
                }
            } else if (!eff.implicit_mem_read.empty() ||
                       !eff.implicit_mem_write.empty()) {
                /* Memoria IMPLICITA pero CONOCIDA: la instruccion no escribe
                 * los corchetes, pero la arquitectura dice por que registro
                 * accede (`movsb` va de `rsi` a `rdi`).  Se apunta igual que un
                 * acceso escrito a mano -- saberlo y no decirlo seria dejar el
                 * analisis a medias. */
                lee = !eff.implicit_mem_read.empty();
                escribe = !eff.implicit_mem_write.empty();
                for (const std::string &r : eff.implicit_mem_read)
                    res.accesos.push_back({r, false});
                for (const std::string &w : eff.implicit_mem_write)
                    res.accesos.push_back({w, true});
            } else {
                // Atomica o mnemonico cuya memoria no esta acotada: no se puede
                // atribuir a un registro base.
                res.accesos_incompletos = true;
            }
            if (lee) res.reads_mem = true;
            if (escribe) res.writes_mem = true;
        }

        /* Registros que el bloque ESCRIBE: los que la tabla marca como
         * implicitos y los operandos que su mascara senala.  Se acumulan para
         * invalidar despues los accesos cuya base pise el propio bloque. */
        for (const std::string &w : eff.implicit_write) escritos.insert(w);
        {
            const std::vector<std::string> ops = operandos_de(line, mnem);
            for (size_t k = 0; k < ops.size() && k < 8; ++k) {
                if (((eff.operand_write_mask >> k) & 1u) == 0u) continue;
                if (ops[k].find('[') != std::string::npos) continue;
                // Un marcador se anota tal cual: `mov $0, $1` pisa la base $0
                // igual que `mov rdi, rsi` pisa rdi.
                if (!ops[k].empty() && ops[k][0] == '$') {
                    escritos.insert(ops[k]);
                    continue;
                }
                const std::string c = asm_canonical_reg(ops[k], arch);
                if (!c.empty()) escritos.insert(c);
            }
        }
    }

    /* Un acceso cuya base la reescribe el propio bloque ya no apunta a donde
     * decia la ligadura: se descarta la lista entera (no se sabe cuanto de lo
     * que toca queda sin describir). */
    for (const AsmBlockEffects::Acceso &a : res.accesos)
        if (escritos.count(a.base)) {
            res.accesos_incompletos = true;
            break;
        }
    res.escritos.assign(escritos.begin(), escritos.end());

    res.explicit_stack_bytes = max_frame;
    return res;
}

} // namespace vx
