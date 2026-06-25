/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/linear_scan.cpp
 * @brief Implementacion del register allocator linear-scan (Phase D.7).
 *        Ver linear_scan.h y doc/REGALLOC.md.
 */

#include "jit/linear_scan.h"

#include <algorithm>

namespace jit {

RegAlloc linear_scan(const IntervalResult &ivs, const TargetRegInfo &tri) {
    RegAlloc out;
    const uint32_t NV = static_cast<uint32_t>(ivs.intervals.size());
    out.assign.resize(NV);
    if (NV == 0) return out;

    /* Dedup de callee-saved usados, COMPARTIDO entre clases (los ids GP
     * y FP no se solapan: GP 0..15, XMM 16..31). */
    bool callee_used_flag[64] = {false};

    /* crossesCall: el intervalo esta vivo en alguna posicion de CALL ->
     * solo puede usar callee-saved (los caller-saved se destruyen). */
    auto crosses_call = [&](const LiveInterval &iv) -> bool {
        for (uint32_t p : ivs.call_positions)
            if (iv.covers(p)) return true;
        return false;
    };

    /* Phase AS inc.5e: @p r esta CLOBBERED para @p iv si el intervalo cubre
     * la posicion de algun INLINE_ASM_RAW cuya lista de clobbers incluye
     * @p r.  Cubre los clobbers de callee-saved (r12-r15) que el
     * call-position no protege (este solo empuja los caller-saved
     * live-across a callee-saved).  Los precoloreados (bindings) NO pasan
     * por aqui: toman su fixed_reg antes, incondicionalmente. */
    auto clobbered_for = [&](const LiveInterval &iv, uint8_t r) -> bool {
        for (const auto &site : ivs.asm_clobbers) {
            if (!iv.covers(site.pos)) continue;
            for (uint8_t cr : site.regs)
                if (cr == r) return true;
        }
        return false;
    };

    /* Procesar cada clase de registro de forma independiente. */
    for (size_t ci = 0; ci < TargetRegInfo::NCLASS; ++ci) {
        const RegClass cls = static_cast<RegClass>(ci);
        const std::vector<uint8_t> &allocatable = tri.allocatable[ci];
        if (allocatable.empty()) continue; // clase no asignable en v1 (FP)

        auto is_callee = [&](uint8_t r) -> bool {
            return tri.is_callee_saved(cls, r);
        };

        /* Intervalos de esta clase, no vacios, ordenados por inicio. */
        std::vector<uint32_t> order;
        order.reserve(NV);
        for (uint32_t v = 0; v < NV; ++v) {
            const LiveInterval &iv = ivs.intervals[v];
            if (!iv.empty() && iv.cls == cls) order.push_back(v);
        }
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
            return ivs.intervals[a].start() < ivs.intervals[b].start();
        });

        /* Estado del scan (fresco por clase). */
        bool occupied[64] = {false};  ///< reg id -> ocupado ahora
        std::vector<uint32_t> active; ///< vids con reg, se expiran por end

        for (uint32_t vid : order) {
            const LiveInterval &iv = ivs.intervals[vid];
            const uint32_t istart = iv.start();

            /* ---- Expirar activos terminados (liberan su reg) ---- */
            {
                size_t k = 0;
                for (size_t a = 0; a < active.size(); ++a) {
                    const uint32_t av = active[a];
                    if (ivs.intervals[av].end() <= istart) {
                        occupied[out.assign[av].reg] = false;
                    } else {
                        active[k++] = active[a];
                    }
                }
                active.resize(k);
            }

            /* ---- Phase AS inc.5: intervalo PRECOLOREADO (register-bound
             * de un inline-asm) ----
             * Toma su @c fixed_reg de forma INCONDICIONAL (override del
             * cross-call y del GC-spill: el inline-asm necesita el valor en
             * ESE registro fisico, aunque sea caller-saved).  Si el reg esta
             * ocupado por un activo, se desaloja a ese activo (spill).  El
             * selector (5d) garantiza que @c fixed_reg es un registro usable
             * (no rbx/rsp/rbp en VM_ABI) y que dos bindings que solapan no
             * comparten registro (validado en el type checker, inc.2). */
            if (iv.fixed_reg >= 0) {
                const uint8_t fr = static_cast<uint8_t>(iv.fixed_reg);
                if (occupied[fr]) {
                    for (size_t a = 0; a < active.size(); ++a) {
                        if (out.assign[active[a]].reg == fr) {
                            const uint32_t av = active[a];
                            out.assign[av].loc = RegAlloc::Loc::SPILL;
                            out.assign[av].slot = out.num_spill_slots++;
                            active.erase(active.begin() + a);
                            break;
                        }
                    }
                    occupied[fr] = false;
                }
                out.assign[vid].loc = RegAlloc::Loc::REG;
                out.assign[vid].reg = fr;
                occupied[fr] = true;
                active.push_back(vid);
                if (is_callee(fr) && !callee_used_flag[fr]) {
                    callee_used_flag[fr] = true;
                    out.callee_saved_used.push_back(fr);
                }
                continue;
            }

            const bool cc = crosses_call(iv);

            /* ---- force_spill: memory-resident obligatorio ----
             * Vregs live-in a un sucesor abnormal (handler de excepcion):
             * deben vivir en un slot para sobrevivir al edge del throw (el
             * catch recarga del slot tras el unwind).  Mismo trato que un GC
             * root cross-call. */
            if (vid < ivs.force_spill.size() && ivs.force_spill[vid]) {
                out.assign[vid].loc = RegAlloc::Loc::SPILL;
                out.assign[vid].slot = out.num_spill_slots++;
                continue;
            }

            /* ---- GC root vivo a traves de un call: SPILL forzado ----
             * (Phase D.7 commit 6, enfoque A).  Un valor GC en un registro
             * a traves de un call seria invisible al GC (que escanea el
             * stack).  Lo ponemos en un slot SIEMPRE; el stackmap del call
             * lo describe ahi.  Los GC roots que NO cruzan calls siguen en
             * registros (sin coste). */
            if (iv.is_gc() && cc) {
                out.assign[vid].loc = RegAlloc::Loc::SPILL;
                out.assign[vid].slot = out.num_spill_slots++;
                continue;
            }

            /* ---- Buscar un fisico libre y usable ---- */
            int chosen = -1;
            for (uint8_t r : allocatable) {
                if (occupied[r]) continue;
                if (cc && !is_callee(r)) continue; // cross-call: solo callee
                if (clobbered_for(iv, r))
                    continue; // inc.5e: clobbered por un asm
                chosen = static_cast<int>(r);
                break;
            }

            if (chosen >= 0) {
                const uint8_t r = static_cast<uint8_t>(chosen);
                out.assign[vid].loc = RegAlloc::Loc::REG;
                out.assign[vid].reg = r;
                occupied[r] = true;
                active.push_back(vid);
                if (is_callee(r) && !callee_used_flag[r]) {
                    callee_used_flag[r] = true;
                    out.callee_saved_used.push_back(r);
                }
                continue;
            }

            /* ---- Sin reg libre: SPILL (Poletto, fin mas lejano) ---- */
            int victim_idx = -1;
            uint32_t victim_end = 0;
            for (size_t a = 0; a < active.size(); ++a) {
                const uint32_t av = active[a];
                const uint8_t r = out.assign[av].reg;
                /* Para un vreg cross-call solo robamos regs callee-saved
                 * (los unicos que podria usar). */
                if (cc && !is_callee(r)) continue;
                /* inc.5e: no robar un reg clobbered por un asm que iv cruza
                 * (iv no podria vivir ahi). */
                if (clobbered_for(iv, r)) continue;
                const uint32_t e = ivs.intervals[av].end();
                if (victim_idx < 0 || e > victim_end) {
                    victim_idx = static_cast<int>(a);
                    victim_end = e;
                }
            }

            if (victim_idx < 0 || iv.end() >= victim_end) {
                /* Spillear el PROPIO intervalo (su fin es el mas lejano,
                 * o no hay activo robable). */
                out.assign[vid].loc = RegAlloc::Loc::SPILL;
                out.assign[vid].slot = out.num_spill_slots++;
            } else {
                /* Robar el reg de la victima: la victima va a stack para
                 * toda su vida; este intervalo toma su reg. */
                const uint32_t jv = active[static_cast<size_t>(victim_idx)];
                const uint8_t r = out.assign[jv].reg;
                out.assign[jv].loc = RegAlloc::Loc::SPILL;
                out.assign[jv].slot = out.num_spill_slots++;
                active.erase(active.begin() + victim_idx);

                out.assign[vid].loc = RegAlloc::Loc::REG;
                out.assign[vid].reg = r; // occupied[r] sigue true
                active.push_back(vid);
                if (is_callee(r) && !callee_used_flag[r]) {
                    callee_used_flag[r] = true;
                    out.callee_saved_used.push_back(r);
                }
            }
        }
    }

    return out;
}

} // namespace jit
