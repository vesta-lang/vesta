/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file jit/stack_scan.cpp
 * @brief Implementacion del walker preciso de JIT frames.
 *
 * Algoritmo:
 *
 *   rbp = rbp_top
 *   while rbp in [stack_low, stack_high) and rbp != 0:
 *       caller_rbp = [rbp]        (8 bytes)
 *       caller_rip = [rbp + 8]    (8 bytes, return address)
 *       info = JitRegistry::lookup(caller_rip)
 *       if info:
 *           # JIT frame entre [rbp - frame_size, rbp)
 *           sm = JitRegistry::lookup_stackmap(caller_rip)
 *           if sm:
 *               for slot in sm.slots:
 *                   value = read_u64(rbp + slot.rbp_offset)
 *                   cb(ctx, value, slot.gc_kind, rbp + slot.rbp_offset)
 *       rbp = caller_rbp
 *
 * Limite de seguridad: max 256 frames para evitar bucles infinitos
 * por stack corruption.  En la practica un stack tipico tiene <50
 * frames; 256 es muy generoso.
 */

#include "jit/stack_scan.h"
#include "jit/jit_registry.h"

namespace jit {

namespace {
constexpr uint32_t MAX_FRAMES = 256;
}

JitScanStats scan_jit_frames(JitRootCallback cb, void *cb_ctx,
                             const uint8_t *rbp_top, const uint8_t *stack_low,
                             const uint8_t *stack_high) noexcept {
    JitScanStats stats{};
    if (!cb || !rbp_top) return stats;

    const uint8_t *rbp = rbp_top;
    JitRegistry &reg = JitRegistry::instance();

    for (uint32_t i = 0; i < MAX_FRAMES; ++i) {
        /* Validar que rbp este en el rango del stack permitido. */
        if (stack_low && rbp < stack_low) break;
        if (stack_high && rbp >= stack_high) break;
        if (rbp == nullptr) break;

        /* rbp debe estar 8-byte aligned para que esto sea valido. */
        if (reinterpret_cast<uintptr_t>(rbp) & 7) break;

        ++stats.frames_walked;

        /* Leer caller saved RBP (en [rbp]) y caller RIP (en [rbp+8]).
         * Defensive: verificar que [rbp..rbp+16] cabe en el stack. */
        if (stack_high && rbp + 16 > stack_high) break;

        const uint64_t saved_rbp = read_u64(rbp);
        const uint64_t caller_rip = read_u64(rbp + 8);
        const uint8_t *rip_ptr = reinterpret_cast<const uint8_t *>(caller_rip);

        /* Lookup en el registro JIT. */
        const Stackmap *sm = reg.lookup_stackmap(rip_ptr);
        if (sm) {
            ++stats.jit_frames;
            /* Por cada slot del stackmap, leer y notificar. */
            for (const StackmapSlot &slot : sm->slots) {
                const uint8_t *slot_addr = rbp + slot.rbp_offset;
                /* Validar que el slot esta en el stack del proceso. */
                if (stack_low && slot_addr < stack_low) continue;
                if (stack_high && slot_addr + 8 > stack_high) continue;
                const uint64_t value = read_u64(slot_addr);
                cb(cb_ctx, value, slot.gc_kind, slot_addr);
                switch (slot.gc_kind) {
                case StackmapGcKind::HANDLE: ++stats.handles_marked; break;
                case StackmapGcKind::HOSTPTR:
                case StackmapGcKind::STRING: ++stats.hostptr_marked; break;
                }
            }
        } else {
            ++stats.non_jit_frames;
        }

        /* Avanzar al frame del caller. */
        const uint8_t *next_rbp = reinterpret_cast<const uint8_t *>(saved_rbp);
        /* Detectar ciclos o stop conditions. */
        if (next_rbp == rbp) break;
        if (next_rbp == nullptr) break;
        rbp = next_rbp;
    }

    return stats;
}

namespace {
/**
 * @brief Localiza el stackmap que cubre @p pc dentro de @p info: el ultimo con
 *        @c pc_offset <= (pc - code_start).  Los stackmaps estan ordenados
 *        ascendente por @c pc_offset (register_function los ordena), asi que un
 *        barrido con corte temprano es O(n_safepoints) con n tipicamente <10.
 *        Evita el doble lookup + doble mutex de @c lookup_stackmap.
 */
const Stackmap *find_stackmap_in(const JitFunctionInfo &info,
                                 uint64_t pc) noexcept {
    const uint64_t base = reinterpret_cast<uint64_t>(info.code_start);
    if (pc < base) return nullptr;
    const uint32_t off = static_cast<uint32_t>(pc - base);
    const Stackmap *best = nullptr;
    for (const Stackmap &s : info.stackmaps) {
        if (s.pc_offset <= off)
            best = &s;
        else
            break; // ordenados ascendente -> el resto tambien es > off.
    }
    return best;
}
} // namespace

JitScanStats scan_aot_frames(JitRootCallback cb, void *cb_ctx, uint64_t start_pc,
                             uint64_t start_sp) noexcept {
    JitScanStats stats{};
    if (!cb || start_pc == 0 || start_sp == 0) return stats;

    JitRegistry &reg = JitRegistry::instance();
    uint64_t pc = start_pc; // return address dentro del frame Vesta actual
    uint64_t sp = start_sp; // RSP del frame Vesta justo antes de su `call`

    for (uint32_t i = 0; i < MAX_FRAMES; ++i) {
        /* RSP siempre 8-alineado; un desalineo indica corrupcion o que se
         * salio de la cadena Vesta -> parar (defensa anti-crash). */
        if (sp & 7u) break;

        const JitFunctionInfo *info =
            reg.lookup(reinterpret_cast<const uint8_t *>(pc));
        /* PC fuera de toda funcion Vesta registrada (CRT / _start / thunk):
         * fin del walk.  Los frames Vesta intermedios SIEMPRE se registran (con
         * frame_size real), aunque no retengan roots, para poder atravesarlos. */
        if (!info) break;

        ++stats.frames_walked;
        ++stats.jit_frames;

        /* Reconstruir RBP SIN leer la cadena [rbp]: en un safepoint call de
         * este frame, RBP = RSP + frame_size.  Robusto ante -fomit-frame-pointer
         * de OTROS frames (nunca dependemos del saved-RBP ajeno). */
        const uint64_t rbp = sp + info->frame_size;

        const Stackmap *sm = find_stackmap_in(*info, pc);
        if (sm) {
            /* Slots GC del safepoint: RBP-relativos (los mismos que emite el
             * codegen), leidos con el RBP reconstruido. */
            for (const StackmapSlot &slot : sm->slots) {
                const uint8_t *slot_addr =
                    reinterpret_cast<const uint8_t *>(rbp) + slot.rbp_offset;
                const uint64_t value = read_u64(slot_addr);
                cb(cb_ctx, value, slot.gc_kind, slot_addr);
                switch (slot.gc_kind) {
                case StackmapGcKind::HANDLE: ++stats.handles_marked; break;
                case StackmapGcKind::HOSTPTR:
                case StackmapGcKind::STRING: ++stats.hostptr_marked; break;
                }
            }
        }

        /* Avanzar al frame llamador POR TAMANO DE FRAME (no cadena RBP):
         *   next_pc = [rbp+8]  -> return address propio de este frame.
         *   next_sp = rbp+16   -> RSP del llamador justo antes de `call estefn`
         *                          (= CFA de este frame; RBP=CFA-16). */
        const uint64_t next_pc =
            read_u64(reinterpret_cast<const uint8_t *>(rbp + 8));
        const uint64_t next_sp = rbp + 16;
        /* La pila crece hacia abajo: el frame del llamador esta en direcciones
         * MAYORES.  Si no avanza hacia arriba, parar (ciclo / corrupcion). */
        if (next_sp <= sp) break;
        if (next_pc == 0) break;
        sp = next_sp;
        pc = next_pc;
    }

    return stats;
}

} // namespace jit
