/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file bounds.cpp
 * @brief Implementacion del comprobador de limites de region (ver bounds.h).
 */
#include "analysis/effects/bounds.h"

#include "analysis/effects/effect_analysis.h"
#include "ir/ssa_ir.h"

#include <cstdlib>

namespace analysis {
namespace effects {

namespace {
/// Nombre legible de una region, para la prueba del diagnostico.
std::string nombre_region(AbstractLoc::Kind k, uint32_t id) {
    const char *base = "?";
    switch (k) {
    case AbstractLoc::Kind::Stack: base = "stack"; break;
    case AbstractLoc::Kind::Heap: base = "heap"; break;
    case AbstractLoc::Kind::Global: base = "global"; break;
    default: break;
    }
    return std::string(base) + "#" + std::to_string(id);
}
} // namespace

std::vector<BoundsViolation> check_region_bounds(const ir::IrModule &mod) {
    std::vector<BoundsViolation> out;
    /* Interruptor de escape.  La comprobacion esta MEDIDA a cero falsos sobre
     * los 454 programas del corpus, pero un veredicto que rompe una compilacion
     * tiene que poder desactivarse sin tocar el compilador. */
    if (const char *v = std::getenv("VESTA_ASA_BOUNDS"))
        if (v[0] == '0') return out;

    EffectAnalysis ea;
    ea.module_summary(mod); // deja el motor con sus tablas listas
    for (const ir::IrFunction &fn : mod.functions) {
        if (fn.blocks.empty()) continue;
        const analysis::PointsTo &pt = ea.points_to_publico(fn);
        for (const ir::IrBlock &b : fn.blocks) {
            for (const ir::IrInstr &in : b.instrs) {
                const EffectAnalysisResult r = ea.local(fn, in);
                auto revisa = [&](const LocSet &ls, bool escribe) {
                    if (ls.is_top) return;
                    for (const AbstractLoc &l : ls.locs) {
                        if (l.width <= 0 || !l.concrete()) continue;
                        /* La extension se indexa por VALUE-ID, y solo Stack y
                         * Heap tienen ahi su raiz: en `ArgDerived` el
                         * identificador es el INDICE DEL PARAMETRO, asi que
                         * consultarlo devolveria la extension de un valor sin
                         * relacion.  De un parametro no se sabe el tamano -- lo
                         * sabe quien llama --, asi que no se afirma nada. */
                        if (l.kind != AbstractLoc::Kind::Stack &&
                            l.kind != AbstractLoc::Kind::Heap)
                            continue;
                        const analysis::RegionExtent &ex = pt.extent_of(l.id);
                        if (!ex.constante()) continue;
                        const int64_t tope = ex.limite();
                        const int64_t fin = l.off + l.width;
                        if (l.off >= 0 && fin <= tope) continue;
                        BoundsViolation v;
                        v.function = fn.name;
                        v.line = in.source_line;
                        v.write = escribe;
                        v.width = l.width;
                        v.off = l.off;
                        v.limite = tope;
                        v.objeto = ex.bytes;
                        v.region = nombre_region(l.kind, l.id);
                        out.push_back(std::move(v));
                    }
                };
                revisa(r.effects.mem.writes, true);
                revisa(r.effects.mem.reads, false);
            }
        }
    }
    return out;
}

} // namespace effects
} // namespace analysis
