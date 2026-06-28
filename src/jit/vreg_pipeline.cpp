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

    /* 2. Intervalos + 3. asignacion (commit 6: el linear_scan FUERZA a
     *    slot los GC roots vivos a traves de un call). */
    IntervalResult ivs = build_intervals(mf, tri);
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
     *    para que el GC los escanee del stack durante un sweep. */
    JitRegistry::instance().register_function(
        code, code + bytes.size(), pf.stackmaps,
        static_cast<uint32_t>(8u * ra.num_spill_slots), "vreg");

    return code;
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
    if (relocs_out) relocs_out->clear();
    if (line_map_out) line_map_out->clear();
    if (asm_labels_out) asm_labels_out->clear();
    if (stackmaps_out) stackmaps_out->clear();
    /* 1. Seleccionar MachineIR de vregs en ABI HOST_LEAF (args en arg_regs,
     *    retorno en RAX, sin ProcessVM* ni runtime entries).  Si la funcion
     *    usa un op fuera del subset, abortar -> vector vacio (fallback). */
    MFunction mf;
    if (!vreg_select(fn, mf, AbiKind::HOST_LEAF, resolve_call, ent,
                     resolve_native, resolve_symbol, pic, target_sysv, mode32,
                     fisa, emit_line_map))
        return {};

    /* Descriptor del target.  x86-64: ABI del TARGET (no del host) -- SysV
     * para ELF, Win64 para PE -> cross-target + boundary externo correcto;
     * RBX reservado (conservador).  x86-32 (mode32): 8 GP eax-edi, regparm(3),
     * pointer_size=4 -> el regalloc solo usa registros que existen en 32-bit.
     */
    const TargetRegInfo &tri =
        mode32 ? target_x86_32() : target_x86_64_abi(target_sysv);

    /* 2. Intervalos + 3. asignacion linear-scan. */
    IntervalResult ivs = build_intervals(mf, tri);
    RegAlloc ra = linear_scan(ivs, tri);

    /* 4. Rewrite a fisico con prologue/epilogue HOST_LEAF + carga de params
     *    desde los arg_regs (parallel-move).  Pasamos &ivs (el rewrite lo
     *    usa para two-address legalization y, si hubiera CALLs, stackmaps);
     *    en BARE no hay GC que consuma esos stackmaps, pero construirlos es
     *    inocuo. */
    MFunction pf = rewrite_to_physical(mf, ra, tri, AbiKind::HOST_LEAF, &ivs);

    /* 5. Encode a bytes nativos.  mode32 -> x86-32 (sin REX, operando 32-bit).
     */
    X86Encoder enc;
    enc.set_mode32(mode32);
    // avx+: MOVES escalares float en VEX (no mezclar con las ops VEX).
    enc.set_vex_scalar(fisa == FloatIsa::AVX || fisa == FloatIsa::AVX512F);
    std::vector<uint8_t> bytes;
    if (enc.encode(pf, bytes) == 0 || bytes.empty()) return {};

    /* Solo-LSP: el encoder ya poblo pf.line_map (si emit_line_map).  La
     * entregamos al caller para la vista correlada fuente <-> asm. */
    if (line_map_out) *line_map_out = std::move(pf.line_map);
    if (asm_labels_out) *asm_labels_out = std::move(pf.asm_labels);
    /* Phase AOT-GC (Inc 1): stackmaps de raices GC (pc_offset relativo a la
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

    /* 8. Registrar el blob C2 en el JitRegistry (stackmaps de sus CALLs). */
    JitRegistry::instance().register_function(
        code, code + bytes.size(), pf.stackmaps,
        static_cast<uint32_t>(8u * ra.num_spill_slots), "vreg-osr");

    return code;
}

} // namespace jit
