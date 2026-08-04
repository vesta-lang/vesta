/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file ir_pattern.cpp
 * @brief Implementacion del registro de patrones (ver ir_pattern.h).
 */

#include "ir/ir_pattern.h"

namespace ir {

bool ir_apply_patterns(IrFunction &fn, const std::vector<Pattern> &patterns) {
    if (fn.is_native || patterns.empty()) return false;
    bool changed = false;

    for (IrBlockId b = 0; b < fn.blocks.size(); ++b) {
        for (size_t i = 0; i < fn.blocks[b].instrs.size(); ++i) {
            // De todos los patrones que reclaman este sitio se queda el que
            // mas gana.  A igualdad se queda el primero, para que el resultado
            // no dependa de en que orden se recorra nada.
            const Pattern *elegido = nullptr;
            PatternMatch elegido_m{};
            int mejor = 0;

            for (const Pattern &p : patterns) {
                if (!p.match || !p.rewrite) continue;
                const PatternMatch m = p.match(fn, b, i);
                if (!m.valid) continue;
                if (p.legal && !p.legal(fn, m)) continue;
                int gana = 1; // sin estimacion, se asume que algo gana
                if (p.benefit) {
                    const PatternBenefit pb = p.benefit(fn, m);
                    if (!pb.worth_it()) continue;
                    gana = pb.instructions_saved;
                }
                if (gana > mejor) {
                    mejor = gana;
                    elegido = &p;
                    elegido_m = m;
                }
            }

            if (elegido && elegido->rewrite(fn, elegido_m)) changed = true;
        }
    }
    return changed;
}

} // namespace ir
