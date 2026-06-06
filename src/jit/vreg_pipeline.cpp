/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/vreg_pipeline.cpp
 * @brief Implementacion del orquestador vreg (Phase D.7, commit 5c).
 *        Ver vreg_pipeline.h y doc/REGALLOC.md.
 */

#include "jit/vreg_pipeline.h"

#include "ir/ssa_ir.h"
#include "jit/auto_jit.h"
#include "jit/code_cache.h"
#include "jit/interval.h"
#include "jit/jit_registry.h"
#include "jit/linear_scan.h"
#include "jit/machine_ir.h"
#include "jit/regalloc_rewrite.h"
#include "jit/target_reginfo.h"
#include "jit/vreg_select.h"
#include "jit/x86_encoder.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace jit {

    uint8_t *vreg_compile(const ir::IrFunction &fn, CodeCache &cc,
                          const CallResolver &resolve_call,
                          const VregEntries &ent,
                          const CallResolver &resolve_native) {
        /* 1. Seleccionar MachineIR de vregs (VM_ABI).  Si la funcion usa un
         *    op fuera del subset soportado, abortar -> fallback. */
        MFunction mf;
        if (!vreg_select(fn, mf, AbiKind::VM, resolve_call, ent, resolve_native))
            return nullptr;

        const TargetRegInfo &tri = target_x86_64_vm_abi();

        /* 2. Intervalos + 3. asignacion (commit 6: el linear_scan FUERZA a
         *    slot los GC roots vivos a traves de un call). */
        IntervalResult ivs = build_intervals(mf, tri);
        RegAlloc ra = linear_scan(ivs, tri);

        /* 3a. Valvula de seguridad DIVMOD_V (Phase D.7 perf, 2026-06-06):
         *     el pseudo DIVMOD_V se marca como call-position (clobber RAX/RDX),
         *     lo que SUBE la presion de registros.  Cuando esa presion provoca
         *     SPILLS, una interaccion del rewrite/encoder bajo spill genera un
         *     0xCC (INT3) en el codigo -> SIGTRAP en runtime (bug del path de
         *     spill aun sin root-cause).  Hasta endurecer ese path, si la
         *     funcion usa DIVMOD_V Y el allocator spillo, hacemos FALLBACK
         *     SEGURO a slots (que maneja DIV/MOD correctamente via su propio
         *     idiv).  Las funciones con DIV/MOD que NO spillean usan el vreg
         *     IDIV (verificado en test_vreg_vm).  Sin DIVMOD_V (gate OFF) esto
         *     no aplica. */
        if (ra.num_spill_slots > 0) {
            bool has_divmod = false;
            for (const auto &blk : mf.blocks) {
                for (const auto &mi : blk.instrs)
                    if (mi.op == MOp::DIVMOD_V) { has_divmod = true; break; }
                if (has_divmod) break;
            }
            if (has_divmod) return nullptr;  // fallback seguro a slots
        }

        /* 3b. Verificador adversarial (commit 6): TODO GC root vivo a traves
         *     de un call DEBE estar en un slot, para que su stackmap lo
         *     describa.  Si por algun bug del allocator quedara en un registro,
         *     el GC no lo veria -> corrupcion del heap.  En vez de arriesgarlo,
         *     hacemos FALLBACK seguro al path de slots.  Coste O(NV*calls),
         *     despreciable frente a encode/commit. */
        if (!ivs.call_positions.empty()) {
            for (uint32_t v = 0; v < mf.vreg_count; ++v) {
                const LiveInterval &lv = ivs.intervals[v];
                if (!lv.is_gc()) continue;
                for (uint32_t cp : ivs.call_positions) {
                    if (lv.covers(cp) && !ra.spilled(v)) {
#ifndef NDEBUG
                        std::fprintf(stderr,
                            "[vreg] GC root v%u vivo a traves de call no spilled "
                            "en '%s' -> fallback a slots\n", v, fn.name.c_str());
#endif
                        return nullptr;
                    }
                }
            }
        }

        /* 4. Rewrite a fisico (VM_ABI) + stackmaps de GC roots en cada CALL. */
        MFunction pf = rewrite_to_physical(mf, ra, tri, AbiKind::VM, &ivs);

        /* 3. Encode a bytes. */
        X86Encoder enc;
        std::vector<uint8_t> bytes;
        if (enc.encode(pf, bytes) == 0 || bytes.empty()) return nullptr;

        /* 4. Alojar en el code cache + commit (flush icache). */
        uint8_t *code = cc.alloc(bytes.size(), 16);
        if (!code) return nullptr;
        std::memcpy(code, bytes.data(), bytes.size());
        cc.commit(code, bytes.size());

        /* Disasm opt-in (VESTA_JIT_DISASM=1) del codigo vreg generado. */
        static const bool dis = []{
            const char *v = std::getenv("VESTA_JIT_DISASM");
            return v && v[0] != '\0' && v[0] != '0';
        }();
        if (dis) debug_dump_jit_code(fn.name + " [vreg]", code, bytes.size());

        /* 5. Registrar en el JitRegistry con los stackmaps reales (commit 6):
         *    describen los GC roots vivos a traves de cada CALL (en slots),
         *    para que el GC los escanee del stack durante un sweep. */
        JitRegistry::instance().register_function(
            code, code + bytes.size(), pf.stackmaps,
            static_cast<uint32_t>(8u * ra.num_spill_slots), "vreg");

        return code;
    }

} // namespace jit
