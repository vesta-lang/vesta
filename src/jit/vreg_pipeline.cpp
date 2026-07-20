/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file jit/vreg_pipeline.cpp
 * @brief Implementacion del orquestador vreg ( D.7, commit 5c).
 *        Ver vreg_pipeline.h y doc/REGALLOC.md.
 */

#include "jit/vreg_pipeline.h"

#include "ir/ssa_ir.h"
#include "jit/auto_jit.h"
#include "jit/code_cache.h"
#include "jit/codegen_target.h"
#include "jit/interval.h"
#include "jit/jit_registry.h"
#include "jit/linear_scan.h"
#include "jit/machine_ir.h"
#include "jit/peephole.h"
#include "jit/regalloc_rewrite.h"
#include "jit/sched/machine_sched.h"
#include "jit/ssa_coalesce.h"
#include "jit/target_reginfo.h"
#include "jit/vreg_select.h"
#include "jit/x86_64/x86_target.h"
#include "jit/x86_encoder.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace jit {

namespace {
/**
 * @brief Scheduler machine-level (C2.15) sobre el MFunction fisico, tras el
 *        regalloc y antes de encodear.  Default ON; @c VESTA_SCHED=0 lo desactiva
 *        (A/B).  Reordena por camino critico/latencia respetando el DAG completo
 *        de dependencias -> oculta latencias y expone ILP al core superescalar.
 */
void maybe_schedule(MFunction &pf, sched::EffIsa isa, sched::SchedMode mode) {
    // Default ON: reordena por camino critico + RECURSOS (latencia + puertos +
    // issue-width + throughput reciproco) del modelo de coste.  VESTA_SCHED=0
    // lo desactiva (A/B).
    static const bool on = [] {
        const char *v = std::getenv("VESTA_SCHED");
        return !(v && v[0] == '0');
    }();
    if (!on) return;
    // Modelo de coste segun el modo: JIT auto-detecta la microarquitectura del
    // host (cpuid/MIDR) y usa los datos EXACTOS de la DB; AOT usa el generico
    // salvo --cpu explicito.  Construir es barato (cpuid una vez); sin estado
    // mutable compartido -> seguro con el compile paralelo (M8).
    const sched::SchedIsa sisa = (isa == sched::EffIsa::ARM64)
                                     ? sched::SchedIsa::ARM64
                                     : sched::SchedIsa::X86_64;
    std::unique_ptr<sched::SchedCostModel> cm =
        sched::make_cost_model(sisa, sched::sched_cpu(), mode);
    const int moved = sched::schedule_function(pf, *cm, isa);
    // Diagnostico opt-in (VESTA_SCHED_EFF=1): volcado del modelo de efectos por
    // instruccion (post-schedule) para cotejar 1:1 con Capstone.
    static const bool dump_eff = std::getenv("VESTA_SCHED_EFF") != nullptr;
    if (dump_eff) {
        auto rk = [](uint32_t k) -> std::string {
            if (k == UINT32_MAX) return "-";
            static const char *gp[16] = {"rax","rcx","rdx","rbx","rsp","rbp",
                "rsi","rdi","r8","r9","r10","r11","r12","r13","r14","r15"};
            if (k < 16) return gp[k];
            if (k < 32) return "xmm" + std::to_string(k - 16);
            return "k" + std::to_string(k);
        };
        std::fprintf(stderr, "[eff] === %s ===\n", pf.name.c_str());
        for (const MBlock &b : pf.blocks)
            for (const MInstr &mi : b.instrs) {
                sched::MEffects e = sched::machine_effects(mi, isa);
                const char *mn = sched::mop_mnemonic(mi.op, isa);
                std::string s = mn ? mn : ("op#" + std::to_string(
                                              static_cast<int>(mi.op)));
                s += " R[";
                for (uint32_t r : e.reads) s += rk(r) + " ";
                s += "] W[";
                for (uint32_t r : e.writes) s += rk(r) + " ";
                s += "]";
                if (e.reads_flags) s += " rF";
                if (e.writes_flags) s += " wF";
                if (e.reads_mem) s += " rMem";
                if (e.writes_mem) s += " wMem";
                if (e.is_barrier) s += " BARRIER";
                std::fprintf(stderr, "[eff] %s\n", s.c_str());
            }
    }
    // Diagnostico opt-in (VESTA_SCHED_STATS=1): cuanto reordena de verdad.
    static const bool stats = std::getenv("VESTA_SCHED_STATS") != nullptr;
    if (stats) {
        long instrs = 0;
        for (const MBlock &b : pf.blocks) instrs += (long)b.instrs.size();
        static long tot_moved = 0, tot_instr = 0, funcs = 0, touched = 0;
        tot_moved += moved;
        tot_instr += instrs;
        ++funcs;
        if (moved) ++touched;
        std::fprintf(stderr,
                     "[sched-stats] modelo=%s puertos=%d issue=%d | acum: "
                     "funcs=%ld tocadas=%ld moved=%ld instr=%ld (%.1f%% mov)\n",
                     cm->name(), cm->port_count(), cm->issue_width(), funcs,
                     touched, tot_moved, tot_instr,
                     tot_instr ? 100.0 * tot_moved / tot_instr : 0.0);
    }
}
} // namespace

uint8_t *vreg_compile(const ir::IrFunction &fn, CodeCache &cc,
                      const CallResolver &resolve_call, const VregEntries &ent,
                      const CallResolver &resolve_native,
                      const CallResolver &resolve_symbol) {
    /* 1. Seleccionar MachineIR de vregs (VM_ABI).  Si la funcion usa un
     *    op fuera del subset soportado, abortar -> fallback. */
    MFunction mf;
    if (!vreg_select(fn, mf, AbiKind::VM, resolve_call, ent, resolve_native,
                     resolve_symbol))
        return nullptr;

    const TargetRegInfo &tri = target_x86_64_vm_abi();

    /* 2. Intervalos + P1 coalescing + 3. asignacion (commit 6: el linear_scan
     *    FUERZA a slot los GC roots vivos a traves de un call).
     *    P1 (register coalescing): une vregs move-related no-interferentes
     *    para eliminar las copias de PHI/2-address.  Si coalesce algo,
     *    reconstruye los intervalos sobre la forma coalescida. */
    IntervalResult ivs = build_intervals(mf, tri);
    if (apply_ssa_coalesce(mf, fn)) ivs = build_intervals(mf, tri);
    RegAlloc ra = linear_scan(ivs, tri);

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
                    std::fprintf(
                        stderr,
                        "[vreg] GC root v%u vivo a traves de call no spilled "
                        "en '%s' -> fallback a slots\n",
                        v, fn.name.c_str());
#endif
                    return nullptr;
                }
            }
        }
    }

    /* 4. Rewrite a fisico (VM_ABI) + stackmaps de GC roots en cada CALL. */
    MFunction pf = rewrite_to_physical(mf, ra, tri, AbiKind::VM, &ivs);

    /* 4b. P1 peephole: borrar los self-moves (`mov rX, rX`) que el coalescing
     *     dejo al asignar el mismo fisico a los dos extremos de una copia. */
    peephole_physical(pf);
    maybe_schedule(pf, sched::EffIsa::X86, sched::SchedMode::JIT_AUTO);

    /* 3. Encode a bytes. */
    X86Encoder enc;
    std::vector<uint8_t> bytes;
    if (enc.encode(pf, bytes) == 0 || bytes.empty()) return nullptr;

    /* 4. Alojar en el code cache + commit (flush icache). */
    uint8_t *code = cc.alloc(bytes.size(), 16);
    if (!code) return nullptr;
    std::memcpy(code, bytes.data(), bytes.size());
    /* Jump table densa (SWITCH_DENSE): parchear cada entrada de 8 bytes con la
     * direccion nativa absoluta del brazo (base + label_offset).  POST-memcpy
     * (base = code) y PRE-commit (antes del flush icache). */
    for (const auto &f : pf.addr_table_fixups) {
        if (f.label < pf.label_offsets.size() &&
            pf.label_offsets[f.label] != UINT32_MAX) {
            const uint64_t target =
                reinterpret_cast<uint64_t>(code) + pf.label_offsets[f.label];
            std::memcpy(code + f.patch_at, &target, sizeof(uint64_t));
        }
    }
    cc.commit(code, bytes.size());

    /* Disasm opt-in (VESTA_JIT_DISASM=1) del codigo vreg generado. */
    static const bool dis = [] {
        const char *v = std::getenv("VESTA_JIT_DISASM");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    if (dis) debug_dump_jit_code(fn.name + " [vreg]", code, bytes.size());

    /* 5. Registrar en el JitRegistry con los stackmaps reales (commit 6):
     *    describen los GC roots vivos a traves de cada CALL (en slots),
     *    para que el GC los escanee del stack durante un sweep.
     *
     *    frame_size para el WALK POR TAMANO DE FRAME (scan_aot_frames, usado
     *    tambien por el scan preciso del GC en modo interp+JIT): RBP - RSP en
     *    un safepoint call = callee-saved pushes + spill_bytes.  Ese valor exacto
     *    ya lo lleva cada stackmap (frame_size_for_scan(), identico en todos);
     *    @c num_spill_slots por si solo NO incluye los callee-saved, asi que
     *    registrarlo daria un RBP mal reconstruido y el GC no veria los roots.
     *    Sin stackmaps (funcion sin calls) el valor no se consume en el walk. */
    const uint32_t scan_frame_size =
        pf.stackmaps.empty()
            ? static_cast<uint32_t>(8u * ra.num_spill_slots)
            : pf.stackmaps.front().frame_size;
    JitRegistry::instance().register_function(
        code, code + bytes.size(), pf.stackmaps, scan_frame_size, "vreg");

    return code;
}

uint8_t *vreg_compile_callback(const ir::IrFunction &fn, CodeCache &cc,
                               const VregCallbackOpts &cb,
                               const CallResolver &resolve_call,
                               const VregEntries &ent,
                               const CallResolver &resolve_native,
                               const CallResolver &resolve_symbol) {
    /* Identico a vreg_compile pero pasando el VregCallbackOpts al selector:
     * el prologo/epilogo del entry siguen la convencion de ABI C nativo. */
    MFunction mf;
    if (!vreg_select(fn, mf, AbiKind::VM, resolve_call, ent, resolve_native,
                     resolve_symbol, /*pic=*/true,
#if defined(_WIN32)
                     /*target_sysv=*/false,
#else
                     /*target_sysv=*/true,
#endif
                     /*mode32=*/false, FloatIsa::SSE2, /*emit_line_map=*/false,
                     cb))
        return nullptr;

    const TargetRegInfo &tri = target_x86_64_vm_abi();

    IntervalResult ivs = build_intervals(mf, tri);
    if (apply_ssa_coalesce(mf, fn)) ivs = build_intervals(mf, tri);
    RegAlloc ra = linear_scan(ivs, tri);

    /* Verificador adversarial de GC roots (igual que vreg_compile). */
    if (!ivs.call_positions.empty()) {
        for (uint32_t v = 0; v < mf.vreg_count; ++v) {
            const LiveInterval &lv = ivs.intervals[v];
            if (!lv.is_gc()) continue;
            for (uint32_t cp : ivs.call_positions) {
                if (lv.covers(cp) && !ra.spilled(v)) return nullptr;
            }
        }
    }

    MFunction pf = rewrite_to_physical(mf, ra, tri, AbiKind::VM, &ivs);
    peephole_physical(pf);
    maybe_schedule(pf, sched::EffIsa::X86, sched::SchedMode::JIT_AUTO);

    X86Encoder enc;
    std::vector<uint8_t> bytes;
    if (enc.encode(pf, bytes) == 0 || bytes.empty()) return nullptr;

    uint8_t *code = cc.alloc(bytes.size(), 16);
    if (!code) return nullptr;
    std::memcpy(code, bytes.data(), bytes.size());
    for (const auto &f : pf.addr_table_fixups) {
        if (f.label < pf.label_offsets.size() &&
            pf.label_offsets[f.label] != UINT32_MAX) {
            const uint64_t target =
                reinterpret_cast<uint64_t>(code) + pf.label_offsets[f.label];
            std::memcpy(code + f.patch_at, &target, sizeof(uint64_t));
        }
    }
    cc.commit(code, bytes.size());

    static const bool dis = [] {
        const char *v = std::getenv("VESTA_JIT_DISASM");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    if (dis) debug_dump_jit_code(fn.name + " [vreg-cb]", code, bytes.size());

    const uint32_t scan_frame_size =
        pf.stackmaps.empty()
            ? static_cast<uint32_t>(8u * ra.num_spill_slots)
            : pf.stackmaps.front().frame_size;
    JitRegistry::instance().register_function(
        code, code + bytes.size(), pf.stackmaps, scan_frame_size, "vreg-cb");

    return code;
}

/*
 * Orquestador AOT ARCH-NEUTRAL: pipeline completo (seleccion -> intervalos ->
 * regalloc -> rewrite -> scheduler -> encode -> traduccion de relocs) a traves
 * de un @c CodegenTarget cualquiera.  Lo comparten x86 y arm64.
 */
std::vector<uint8_t> vreg_compile_native_target(
    const ir::IrFunction &fn, const CodegenTarget &target,
    std::vector<NativeReloc> *relocs_out,
    std::vector<LineMapEntry> *line_map_out,
    std::vector<std::pair<uint32_t, std::string>> *asm_labels_out,
    std::vector<Stackmap> *stackmaps_out) {
    if (relocs_out) relocs_out->clear();
    if (line_map_out) line_map_out->clear();
    if (asm_labels_out) asm_labels_out->clear();
    if (stackmaps_out) stackmaps_out->clear();

    /* 1. SELECCION: IR (SSA) -> MachineIR de vregs (ABI HOST_LEAF). */
    MFunction mf;
    if (!target.select(fn, mf)) return {};
    const TargetRegInfo &tri = target.reg_info();

    /* 2. Intervalos + P1 coalescing + 3. asignacion linear-scan (COMUN a todo
     *    target: toman el TargetRegInfo). */
    IntervalResult ivs = build_intervals(mf, tri);
    if (apply_ssa_coalesce(mf, fn)) ivs = build_intervals(mf, tri);
    RegAlloc ra = linear_scan(ivs, tri);

    /* 4. REWRITE a fisico (prologo/epilogo + spills de la ABI del target). */
    MFunction pf = target.rewrite(mf, ra, ivs);
    target.peephole(pf);
    maybe_schedule(pf, target.sched_isa(), sched::SchedMode::AOT_GENERIC);

    /* 5. ENCODE a bytes maquina del target. */
    std::vector<uint8_t> bytes;
    if (target.encode(pf, bytes) == 0 || bytes.empty()) return {};

    /* Solo-LSP: el encoder ya poblo pf.line_map (si emit_line_map).  La
     * entregamos al caller para la vista correlada fuente <-> asm. */
    if (line_map_out) *line_map_out = std::move(pf.line_map);
    if (asm_labels_out) *asm_labels_out = std::move(pf.asm_labels);
    /*  AOT-GC (Inc 1): stackmaps de raices GC (pc_offset relativo a la
     * funcion + slots GcHandle), poblados por rewrite_to_physical en cada
     * safepoint/CALL.  El encoder ya fijo pc_offset al byte real del call. */
    if (stackmaps_out) *stackmaps_out = std::move(pf.stackmaps);

    /* AOT: traducir las MReloc del encoder (sym_idx -> reloc_symbols) a
     * NativeReloc con el NOMBRE del simbolo resuelto, para que el driver
     * las aplique sin depender de la tabla interna del MFunction. */
    if (relocs_out) {
        relocs_out->reserve(pf.relocs.size());
        for (const MReloc &r : pf.relocs) {
            NativeReloc nr;
            switch (r.kind) {
            case MRelocKind::CALL_REL32:
                nr.kind = NativeReloc::Kind::CALL_REL32;
                break;
            case MRelocKind::DATA_REL32:
                nr.kind = NativeReloc::Kind::DATA_REL32;
                break;
            case MRelocKind::ABS64: nr.kind = NativeReloc::Kind::ABS64; break;
            case MRelocKind::TPOFF32:
                nr.kind = NativeReloc::Kind::TPOFF32;
                break;
            case MRelocKind::SECREL32:
                nr.kind = NativeReloc::Kind::SECREL32;
                break;
            case MRelocKind::ARM64_CALL26:
                nr.kind = NativeReloc::Kind::ARM64_CALL26;
                break;
            }
            nr.offset = r.patch_at;
            nr.addend = r.addend;
            nr.symbol = (r.sym_idx < pf.reloc_symbols.size())
                            ? pf.reloc_symbols[r.sym_idx]
                            : std::string();
            relocs_out->push_back(std::move(nr));
        }
    }

    /* Disasm opt-in (VESTA_JIT_DISASM=1) del codigo AOT generado. */
    static const bool dis = [] {
        const char *v = std::getenv("VESTA_JIT_DISASM");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    if (dis)
        debug_dump_jit_code(fn.name + " [aot-native]", bytes.data(),
                            bytes.size());

    return bytes;
}

std::vector<uint8_t>
vreg_compile_native(const ir::IrFunction &fn, const CallResolver &resolve_call,
                    const VregEntries &ent, const CallResolver &resolve_native,
                    const CallResolver &resolve_symbol,
                    std::vector<NativeReloc> *relocs_out, bool pic,
                    bool target_sysv, bool mode32, FloatIsa fisa,
                    bool emit_line_map,
                    std::vector<LineMapEntry> *line_map_out,
                    std::vector<std::pair<uint32_t, std::string>>
                        *asm_labels_out,
                    std::vector<Stackmap> *stackmaps_out) {
    /* Ruta AOT x86: construye el X86Target y delega en el orquestador comun. */
    const X86Target target(resolve_call, ent, resolve_native, resolve_symbol,
                           pic, target_sysv, mode32, fisa, emit_line_map);
    return vreg_compile_native_target(fn, target, relocs_out, line_map_out,
                                      asm_labels_out, stackmaps_out);
}

uint8_t *vreg_compile_osr(const ir::IrFunction &fn, CodeCache &cc,
                          const CallResolver &resolve_call,
                          const VregEntries &ent,
                          const CallResolver &resolve_native,
                          const CallResolver &resolve_symbol,
                          uint32_t header_block, uint8_t **osr_entry_out,
                          const std::vector<uint32_t> *required_captures) {
    if (osr_entry_out) *osr_entry_out = nullptr;

    /* 1-3: identico a vreg_compile (selector + intervals + regalloc +
     * verificador adversarial de GC roots). */
    MFunction mf;
    if (!vreg_select(fn, mf, AbiKind::VM, resolve_call, ent, resolve_native,
                     resolve_symbol))
        return nullptr;
    const TargetRegInfo &tri = target_x86_64_vm_abi();
    IntervalResult ivs = build_intervals(mf, tri);
    RegAlloc ra = linear_scan(ivs, tri);
    if (!ivs.call_positions.empty()) {
        for (uint32_t vv = 0; vv < mf.vreg_count; ++vv) {
            const LiveInterval &lv = ivs.intervals[vv];
            if (!lv.is_gc()) continue;
            for (uint32_t cp : ivs.call_positions) {
                if (lv.covers(cp) && !ra.spilled(vv)) return nullptr;
            }
        }
    }

    /* 4. Rewrite a fisico en modo OSR-ENTRY (suprime el trigger C1 y
     *    appendea el bloque OSR-entry para @p header_block). */
    OsrEmit osr;
    osr.mode = OsrEmit::C2_ENTRY;
    osr.header_block = static_cast<MBlockId>(header_block);
    osr.required_captures = required_captures; // red de seguridad live-in
    MFunction pf = rewrite_to_physical(mf, ra, tri, AbiKind::VM, &ivs, &osr);
    maybe_schedule(pf, sched::EffIsa::X86, sched::SchedMode::JIT_AUTO);
    if (!osr.osr_entry_valid) return nullptr; // no se pudo emitir el entry

    /* 5. Encode. */
    X86Encoder enc;
    std::vector<uint8_t> bytes;
    if (enc.encode(pf, bytes) == 0 || bytes.empty()) return nullptr;

    /* 6. Alojar + commit. */
    uint8_t *code = cc.alloc(bytes.size(), 16);
    if (!code) return nullptr;
    std::memcpy(code, bytes.data(), bytes.size());
    /* Jump table densa (SWITCH_DENSE): parchear entradas con la direccion
     * nativa del brazo (base + label_offset).  POST-memcpy, PRE-commit. */
    for (const auto &f : pf.addr_table_fixups) {
        if (f.label < pf.label_offsets.size() &&
            pf.label_offsets[f.label] != UINT32_MAX) {
            const uint64_t target =
                reinterpret_cast<uint64_t>(code) + pf.label_offsets[f.label];
            std::memcpy(code + f.patch_at, &target, sizeof(uint64_t));
        }
    }
    cc.commit(code, bytes.size());

    /* 7. Resolver la direccion absoluta del OSR-entry via el offset que el
     *    encoder dejo en label_offsets. */
    if (osr.osr_entry_label < pf.label_offsets.size() &&
        pf.label_offsets[osr.osr_entry_label] != UINT32_MAX) {
        if (osr_entry_out)
            *osr_entry_out = code + pf.label_offsets[osr.osr_entry_label];
    } else {
        return nullptr; // offset no resuelto
    }

    static const bool dis = [] {
        const char *v = std::getenv("VESTA_JIT_DISASM");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    if (dis) debug_dump_jit_code(fn.name + " [vreg-osr]", code, bytes.size());

    /* 8. Registrar el blob C2 en el JitRegistry (stackmaps de sus CALLs).
     *    frame_size = RBP - RSP en un safepoint (callee-saved + spill_bytes),
     *    tomado del stackmap: @c num_spill_slots por si solo no incluye los
     *    callee-saved -> el scan_aot_frames reconstruiria mal el RBP.  Ver el
     *    comentario equivalente en vreg_compile. */
    const uint32_t scan_frame_size =
        pf.stackmaps.empty()
            ? static_cast<uint32_t>(8u * ra.num_spill_slots)
            : pf.stackmaps.front().frame_size;
    JitRegistry::instance().register_function(
        code, code + bytes.size(), pf.stackmaps, scan_frame_size, "vreg-osr");

    return code;
}

} // namespace jit
