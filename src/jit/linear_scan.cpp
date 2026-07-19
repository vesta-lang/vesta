/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
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
            /* Coalescing 2-address: preferir el fisico de src1 (hint) si esta
             * LIBRE.  Libre <=> src1 ya expiro = murio -> reusar su reg para
             * dst es seguro y elide el `mov dst, src1` del legalizado.  Si no
             * esta libre/usable, cae al greedy normal. */
            if (vid < ivs.coalesce_hint.size() && ivs.coalesce_hint[vid] >= 0) {
                const uint32_t partner =
                    static_cast<uint32_t>(ivs.coalesce_hint[vid]);
                /* CRITICO: el reg de partner solo conserva su valor si partner
                 * MUERE exactamente en el def de dst (su ultimo uso = la ranura
                 * use del op 2-address, posicion 2*gi; el def de dst = 2*gi+1).
                 * Asi no hay hueco donde otro vreg reuse el reg.  Posiciones: 2
                 * por instr (use=2*gi, def=2*gi+1) -> partner.end()+1 ==
                 * dst.start().  Para dst loop-carried (start = header, mucho
                 * antes) no se cumple -> no coalesce (seguro).  Sin este guard,
                 * occupied[hr]==false (libre) NO garantiza que hr aun tenga el
                 * valor de partner (pudo morir antes y reusarse el reg). */
                if (partner < out.assign.size() &&
                    out.assign[partner].loc == RegAlloc::Loc::REG &&
                    partner < ivs.intervals.size() &&
                    ivs.intervals[partner].end() + 1u == istart) {
                    const uint8_t hr = out.assign[partner].reg;
                    /* hr debe ser de esta clase (mismo banco), estar libre y
                     * pasar los filtros cross-call / clobber. */
                    bool in_class = false;
                    for (uint8_t r : allocatable)
                        if (r == hr) { in_class = true; break; }
                    if (in_class && !occupied[hr] && !(cc && !is_callee(hr)) &&
                        !clobbered_for(iv, hr))
                        chosen = static_cast<int>(hr);
                }
            }
            if (chosen < 0)
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

            /* ---- Sin reg libre: SPILL por NEXT-USE (no por fin mas lejano) ----
             * Mejor heuristica que Poletto: mantener en reg el intervalo que se
             * usa ANTES; spillear el que se usa MAS TARDE (su reg no se necesita
             * en mas tiempo).  next_use = primer uso >= istart (lista uses
             * ascendente); si no quedan usos -> end() (vivo pero sin usos, buen
             * candidato a spill).  Solo heuristica -> no afecta correctitud. */
            auto next_use = [&](uint32_t av) -> uint32_t {
                const auto &u = ivs.intervals[av].uses;
                auto it = std::lower_bound(u.begin(), u.end(), istart);
                if (it != u.end()) return *it;
                return ivs.intervals[av].end();
            };
            int victim_idx = -1;
            uint32_t victim_nu = 0;
            for (size_t a = 0; a < active.size(); ++a) {
                const uint32_t av = active[a];
                const uint8_t r = out.assign[av].reg;
                /* Para un vreg cross-call solo robamos regs callee-saved
                 * (los unicos que podria usar). */
                if (cc && !is_callee(r)) continue;
                /* inc.5e: no robar un reg clobbered por un asm que iv cruza
                 * (iv no podria vivir ahi). */
                if (clobbered_for(iv, r)) continue;
                const uint32_t nu = next_use(av);
                if (victim_idx < 0 || nu > victim_nu) {
                    victim_idx = static_cast<int>(a);
                    victim_nu = nu;
                }
            }

            /* inc2b.2: un intervalo REGISTER-REQUIRED (operando `reg` de un asm
             * que lo referencia por $N durante todo el bloque) NO puede
             * derramarse: si hay una victima robable, se roba SIEMPRE su reg
             * (aunque el propio next-use sea mas lejano); solo si NO hay victima
             * cae al self-spill (rarisimo; el rewrite lo detecta y hace fallback
             * al pick greedy). */
            const bool must_reg = iv.reg_required && victim_idx >= 0;
            if (!must_reg && (victim_idx < 0 || next_use(vid) >= victim_nu)) {
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
