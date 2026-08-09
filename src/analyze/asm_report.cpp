/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analyze/asm_report.cpp
 * @brief Implementacion de @ref analyze/asm_report.h.
 */

#include "analyze/asm_report.h"

#include "vx/asm/asm_analyze.h"
#include "vx/asm/asm_cfg.h"
#include "vx/asm/asm_effects.h"
#include "vx/asm/asm_phys_reg.h" // sustituir los $N por su registro
#include "vx/asm/instr_db.h"

#include <cstdio>
#include <iomanip>

namespace analyze {

namespace {

/**
 * @brief Si un registro es del banco VECTORIAL.
 *
 * Se decide por el nombre canonico porque es lo unico que da la base para un
 * operando, y el nombre basta: los bancos anchos se llaman igual en todas las
 * ISAs que se compilan (xmm/ymm/zmm y las mascaras en x86; v/q en arm64).
 * Ante la duda se cuenta como general, que es lo mas comun.
 */
bool es_vectorial(const std::string &reg) {
    if (reg.size() >= 3) {
        const std::string p3 = reg.substr(0, 3);
        if (p3 == "xmm" || p3 == "ymm" || p3 == "zmm") return true;
    }
    if (reg.size() >= 2 && (reg[0] == 'v' || reg[0] == 'q') &&
        std::isdigit(static_cast<unsigned char>(reg[1])))
        return true;
    // Mascaras de AVX-512 (k0..k7): son del mundo vectorial, no de proposito
    // general, y contarlas como generales falsearia el reparto.
    if (reg.size() == 2 && reg[0] == 'k' &&
        std::isdigit(static_cast<unsigned char>(reg[1])))
        return true;
    return false;
}

/// Describe UN bloque a partir de su texto.
AsmBlockReport describir(const std::string &cuerpo, const std::string &funcion,
                         uint32_t linea) {
    AsmBlockReport r;
    r.funcion = funcion;
    r.linea = linea;
    const vx::instr_db::Isa isa = vx::isa_actual();
    const vx::AsmCfg cfg = vx::build_asm_cfg(isa, cuerpo);

    for (const vx::AsmInsn &in : cfg.insns) {
        if (in.sintetica) continue; // no la escribio el usuario
        ++r.instrucciones;
        if (in.term == vx::AsmTerm::UncondJump ||
            in.term == vx::AsmTerm::CondBranch ||
            in.term == vx::AsmTerm::Indirect || in.term == vx::AsmTerm::Ret)
            ++r.control;

        const int32_t fid = vx::instr_db::match_asm_line(isa, in.text);
        if (fid < 0) {
            /* De una instruccion que la base no conoce no se sabe NADA: ni que
             * registros toca, ni si va a memoria.  Se cuenta aparte en vez de
             * sumar ceros, que haria creer que no hace nada. */
            ++r.desconocidas;
            r.por_juego["UNKNOWN"]++;
            continue;
        }
        ++r.conocidas;
        const std::string rasgo =
            vx::instr_db::nombre_de_rasgo(vx::instr_db::isa_set_of(isa, fid));
        r.por_juego[rasgo.empty() ? std::string("BASE") : rasgo]++;
        if (!rasgo.empty()) r.rasgos.insert(rasgo);

        const vx::instr_db::AsmInsnSem sem =
            vx::instr_db::asm_insn_sem(isa, in.text, 0);
        for (const std::string &g : sem.reads)
            (es_vectorial(g) ? r.lee_vec : r.lee_gpr)++;
        for (const std::string &g : sem.writes)
            (es_vectorial(g) ? r.escribe_vec : r.escribe_gpr)++;
        if (sem.reads_mem) ++r.lee_mem;
        if (sem.writes_mem) ++r.escribe_mem;
        if (sem.writes_flags) ++r.escribe_flags;
        if (sem.barrier) r.barrera = true;
    }
    return r;
}

} // namespace

const char *nombre_banda(BandaCobertura b) {
    switch (b) {
    case BandaCobertura::Total: return "se entiende entero -> optimizacion normal";
    case BandaCobertura::Conservadora:
        return "casi entero -> optimizacion conservadora";
    case BandaCobertura::Restringida:
        return "a medias -> restricciones adicionales";
    default: return "se entiende poco -> tratarlo como caja negra";
    }
}

BandaCobertura banda_de(double c) {
    if (c >= 1.0) return BandaCobertura::Total;
    if (c >= 0.95) return BandaCobertura::Conservadora;
    if (c >= 0.50) return BandaCobertura::Restringida;
    return BandaCobertura::Opaca;
}

std::vector<AsmBlockReport> analizar_bloques_asm(const ir::IrModule &mod) {
    std::vector<AsmBlockReport> out;
    uint32_t n = 0;
    for (const ir::IrFunction &fn : mod.functions) {
        for (const ir::IrBlock &b : fn.blocks) {
            for (const ir::IrInstr &in : b.instrs) {
                /* El cuerpo no vive siempre en el mismo sitio: uno con
                 * operandos ligados lo lleva en @c func_name y uno opaco en la
                 * tabla de micros.  Mirar solo el primero deja fuera justo los
                 * que mas falta hace describir. */
                std::string cuerpo;
                if (in.op == ir::IrOp::INLINE_ASM) {
                    cuerpo = in.func_name;
                } else if (in.op == ir::IrOp::ASM_MICRO &&
                           in.imm < fn.asm_micros.size()) {
                    cuerpo = fn.asm_micros[in.imm].tmpl;
                } else {
                    continue;
                }
                if (cuerpo.empty()) continue;
                /* Los operandos que elige el compilador se escriben `$N` en el
                 * cuerpo, y con `$N` la base no puede decir de que banco es el
                 * registro ni si el acceso va a memoria: contaba dieciseis
                 * escrituras a registros generales en un bloque que solo mueve
                 * vectores.  Se sustituyen por el registro que les toca antes
                 * de mirar nada -- unos numeros equivocados son peores que no
                 * darlos. */
                cuerpo = vx::asm_body_subst_greedy(cuerpo, fn.asm_reg_bindings);
                AsmBlockReport r = describir(cuerpo, fn.name, in.source_line);
                /* El nivel viaja en los bits 6-7 del bitfield de calificadores:
                 * 0 = analizable, 1 = volatile, 2 = raw. */
                if (in.op == ir::IrOp::INLINE_ASM)
                    r.opacidad_pedida = ((in.imm >> 6) & 3ull) != 0;
                r.indice = ++n;
                out.push_back(std::move(r));
            }
        }
    }
    return out;
}

void print_asm_report(std::ostream &os,
                      const std::vector<AsmBlockReport> &bloques) {
    if (bloques.empty()) return;
    os << "\n=== ASA: bloques de ensamblador ===\n";
    os << "Cada bloque, con lo que la base de instrucciones sabe de el.  La\n"
          "COBERTURA es lo que decide cuanto se puede optimizar a su alrededor.\n";
    char buf[128];
    for (const AsmBlockReport &r : bloques) {
        os << "\nBLOQUE #" << r.indice << "\n";
        os << "  en        : " << r.funcion;
        if (r.linea != 0) os << " (linea " << r.linea << ")";
        os << "\n";
        os << "  instrucciones: " << r.instrucciones << "\n";

        os << "  juegos    :";
        if (r.por_juego.empty()) {
            os << " -";
        } else {
            os << "\n";
            for (const auto &kv : r.por_juego) {
                std::snprintf(buf, sizeof(buf), "      %-16s %4u\n",
                              kv.first.c_str(), kv.second);
                os << buf;
            }
        }
        if (r.por_juego.empty()) os << "\n";

        const double c = r.cobertura();
        std::snprintf(buf, sizeof(buf),
                      "  cobertura : %u/%u conocidas (%.2f%%)  --  %s\n",
                      r.conocidas, r.instrucciones, c * 100.0,
                      nombre_banda(banda_de(c)));
        os << buf;

        std::snprintf(buf, sizeof(buf),
                      "  registros : generales  lee %u  escribe %u   |   "
                      "vectoriales  lee %u  escribe %u\n",
                      r.lee_gpr, r.escribe_gpr, r.lee_vec, r.escribe_vec);
        os << buf;
        std::snprintf(buf, sizeof(buf),
                      "  memoria   : lee %u  escribe %u\n", r.lee_mem,
                      r.escribe_mem);
        os << buf;
        std::snprintf(buf, sizeof(buf),
                      "  control   : %u salto(s)   flags: %u   barrera: %s\n",
                      r.control, r.escribe_flags, r.barrera ? "si" : "no");
        os << buf;
        if (r.opacidad_pedida)
            os << "  optimizar : NO -- se pidio `volatile`/`raw`.  Todo lo de "
                  "arriba se sabe igual; lo que se respeta es no tocarlo\n";

        os << "  exige     : ";
        if (r.rasgos.empty()) {
            os << "nada fuera del conjunto base";
        } else {
            bool primero = true;
            for (const std::string &g : r.rasgos) {
                if (!primero) os << ", ";
                os << g;
                primero = false;
            }
        }
        os << "\n";

        if (r.desconocidas != 0) {
            /* Lo unico que hay que arreglar EN LA BASE.  Va aparte porque no es
             * una propiedad del programa: es una laguna del compilador, y
             * mezclarla con lo demas la hace parecer culpa de quien escribio el
             * asm. */
            std::snprintf(buf, sizeof(buf),
                          "  pendiente : %u instruccion(es) que la base no "
                          "conoce -- de esas no se sabe ni lo que hacen\n",
                          r.desconocidas);
            os << buf;
        }
    }
    os << "\n";
}

} // namespace analyze
