/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vx/lowering/memory_ops.cpp
 * @brief Mover bloques de memoria, y elegir COMO segun la maquina que ejecute.
 *
 * Copiar mil bytes no tiene una forma buena, tiene varias: byte a byte, de
 * palabra en palabra, con la instruccion de bloque del procesador, o con
 * registros anchos si la maquina los tiene.  Y cual es la mejor no se sabe al
 * compilar, porque el binario puede acabar en un procesador distinto del que lo
 * compilo.
 *
 * De ahi lo que hay aqui: emitir VARIAS versiones, preguntar UNA vez al
 * arrancar que sabe hacer la maquina, y dejar que cada llamada vaya a la que
 * toca.  El coste de preguntar se paga una vez; el de elegir mal, en cada copia.
 */
#include "vx/lowering.h"
#include "util/thread_slot.h" // el estado por hilo NO va en thread_local
#include "ir/ir_type_info.h" // vocabulario UNICO de anchura/clase de un IrType
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <utility>
#include "lowering_internal.h" // la cocina compartida del lowering

namespace vx {

uint64_t Lowering::ensure_cpu_features_global() {
    // Idempotente: si ya esta emitido, devolver el slot existente.
    if (cpu_features_slot_ != UINT64_MAX) return cpu_features_slot_;

    // 1. Slot static_data de 8 bytes zero-init para el global.  Va a `.data`
    //    (WRITABLE): __vx_cpu_init le hace STORE en runtime.  El default de
    //    STR_LIT_ADDR es `.rodata` (read-only) -> un STORE ahi fallaria.
    //    NON_DEDUP para que el merge cross-module no lo colapse con otro
    //    all-zero; FORCE_EMIT para garantizar su presencia aunque el optimizer
    //    toque los relocs.
    std::vector<uint8_t> zero(8, 0);
    const uint64_t slot =
        static_cast<uint64_t>(out_mod_->static_data.push_back(std::move(zero)));
    {
        auto &m = out_mod_->static_data.meta_at(slot);
        m.section_name = ".data";
        m.flags |=
            ir::IrModule::SD_FLAG_NON_DEDUP | ir::IrModule::SD_FLAG_FORCE_EMIT;
        // Global de programa: unificar el slot cross-module en el merge.
        m.shared_key = "__vx_cpu_features";
    }
    cpu_features_slot_ = slot;

    if (cpu_init_emitted_) return slot;
    cpu_init_emitted_ = true;

    // 2. Helper __vx_cpu_init(): un bloque asm que detecta features y un
    //    STORE del bitmask al slot.  Construido como IrFunction aparte.
    const std::string name = "__vx_cpu_init";

    ir::IrFunction *saved_fn = fn_;
    ir::IrBlockId saved_block = current_block_;
    bool saved_terminated = block_terminated_;

    ir::IrFunction hf;
    hf.name = name;
    hf.ret_type = ir::IrType::VOID;
    const ir::IrBlockId e = hf.new_block("entry");

    fn_ = &hf;
    current_block_ = e;
    block_terminated_ = false;
    const uint32_t ln = 0;

    // --- binding register("rax") u64 feat;  (output only) ---
    // ALLOCA estable + AsmRegBinding -> el selector lo precolorea a rax.
    const ir::IrValueId rax_slot = fn_->new_value(ir::IrType::PTR);
    fn_->values[rax_slot].is_host_ptr = true;
    {
        ir::IrInstr al{};
        al.op = ir::IrOp::ALLOCA;
        al.type = ir::IrType::I8;
        al.dst = rax_slot;
        al.imm = 8;
        al.host_alloca = true;
        al.source_line = ln;
        emit(current_block_, std::move(al));
    }
    // En modo protegido los registros son de 32 bits y `rax` no existe: el
    // binding, y todo el cuerpo de abajo, se nombran segun el ANCHO DEL TARGET.
    // Antes se emitia siempre en 64 bits, asi que en x86-32 el ensamblado
    // fallaba ("xor rsi, rsi") y con el se caia la funcion ENTERA que hubiera
    // disparado la deteccion -- normalmente `main`.
    const bool bits32 = (asm_target_bits_ == 32);
    {
        ir::AsmRegBinding b{rax_slot, bits32 ? "eax" : "rax", ir::IrType::U64,
                            false, "__cpu_feat"};
        b.reg_class = b.reg; // registro concreto.
        fn_->asm_reg_bindings.push_back(std::move(b));
    }

    // --- bloque INLINE_ASM: deteccion + empaquetado completo, bitmask en rax
    // --- bit0=SSE2(L1.EDX.26) bit1=SSE4.2(L1.ECX.20) bit2=POPCNT(L1.ECX.23)
    // bit3=AVX(L1.ECX.28) bit4=AVX2(L7.EBX.5) bit5=BMI1(L7.EBX.3)
    // bit6=BMI2(L7.EBX.8) bit7=AVX512F(L7.EBX.16) bit8=ERMS(L7.EBX.9).
    const std::string asm_body = "xor rsi, rsi\n" // acumulador = 0
                                                  // ----- leaf 1 -----
                                 "mov rax, 0x1\n"
                                 "xor rcx, rcx\n"
                                 "cpuid\n" // -> ecx, edx
                                 // SSE2 = EDX bit26 -> acc bit0
                                 "mov rdi, rdx\n"
                                 "shr rdi, 0x1a\n"
                                 "and rdi, 0x1\n"
                                 "or rsi, rdi\n"
                                 // SSE4.2 = ECX bit20 -> acc bit1
                                 "mov rdi, rcx\n"
                                 "shr rdi, 0x14\n"
                                 "and rdi, 0x1\n"
                                 "shl rdi, 0x1\n"
                                 "or rsi, rdi\n"
                                 // POPCNT = ECX bit23 -> acc bit2
                                 "mov rdi, rcx\n"
                                 "shr rdi, 0x17\n"
                                 "and rdi, 0x1\n"
                                 "shl rdi, 0x2\n"
                                 "or rsi, rdi\n"
                                 // AVX = ECX bit28 -> acc bit3
                                 "mov rdi, rcx\n"
                                 "shr rdi, 0x1c\n"
                                 "and rdi, 0x1\n"
                                 "shl rdi, 0x3\n"
                                 "or rsi, rdi\n"
                                 // ----- leaf 7 subleaf 0 -----
                                 "mov rax, 0x7\n"
                                 "xor rcx, rcx\n"
                                 "cpuid\n" // -> ebx, ecx, edx
                                 // AVX2 = EBX bit5 -> acc bit4
                                 "mov rdi, rbx\n"
                                 "shr rdi, 0x5\n"
                                 "and rdi, 0x1\n"
                                 "shl rdi, 0x4\n"
                                 "or rsi, rdi\n"
                                 // BMI1 = EBX bit3 -> acc bit5
                                 "mov rdi, rbx\n"
                                 "shr rdi, 0x3\n"
                                 "and rdi, 0x1\n"
                                 "shl rdi, 0x5\n"
                                 "or rsi, rdi\n"
                                 // BMI2 = EBX bit8 -> acc bit6
                                 "mov rdi, rbx\n"
                                 "shr rdi, 0x8\n"
                                 "and rdi, 0x1\n"
                                 "shl rdi, 0x6\n"
                                 "or rsi, rdi\n"
                                 // AVX512F = EBX bit16 -> acc bit7
                                 "mov rdi, rbx\n"
                                 "shr rdi, 0x10\n"
                                 "and rdi, 0x1\n"
                                 "shl rdi, 0x7\n"
                                 "or rsi, rdi\n"
                                 // ERMS = EBX bit9 -> acc bit8
                                 "mov rdi, rbx\n"
                                 "shr rdi, 0x9\n"
                                 "and rdi, 0x1\n"
                                 "shl rdi, 0x8\n"
                                 "or rsi, rdi\n"
                                 // resultado -> rax (binding de salida)
                                 "mov rax, rsi\n";

    // El cuerpo se escribe una sola vez, en 64 bits, y se reescribe a los
    // nombres de 32 cuando toca: `cpuid` y todo lo que hace aqui (mascaras de
    // 9 bits, desplazamientos) existe igual en modo protegido, lo unico que no
    // existe alli son los registros anchos.
    std::string asm_body_t = asm_body;
    if (bits32) {
        static const char *const kRegs[][2] = {{"rax", "eax"}, {"rbx", "ebx"},
                                               {"rcx", "ecx"}, {"rdx", "edx"},
                                               {"rsi", "esi"}, {"rdi", "edi"}};
        for (const auto &par : kRegs) {
            size_t pos = 0;
            while ((pos = asm_body_t.find(par[0], pos)) != std::string::npos) {
                asm_body_t.replace(pos, 3, par[1]);
                pos += 3;
            }
        }
    }
    {
        ir::IrInstr ia{};
        ia.op = ir::IrOp::INLINE_ASM;
        ia.type = ir::IrType::VOID;
        ia.dst = ir::IR_NO_VALUE;
        ia.source_line = ln;
        ia.func_name = asm_body_t;
        ia.preserve = true; // volatile: nunca eliminar/reordenar.

        // Listar el slot del binding como operando (lo mantiene vivo + lo
        // clasifica como output via el LOAD posterior).
        ia.operands.push_back(rax_slot);

        // Clobbers explicitos: cpuid pisa rax/rbx/rcx/rdx; ademas usamos
        // rsi/rdi como scratch.  El selector excluye los GP usables de los
        // vregs no-binding vivos (no hay ninguno aqui) y SALVA/RESTAURA rbx
        // (reservado) alrededor del bloque.  flags: memory=0 (no toca mem),
        // flags=1 (cpuid/and/shr afectan flags).
        std::vector<std::string> clob = {"rbx", "rcx", "rdx", "rsi", "rdi"};
        uint64_t q = 0;
        q |= 1ull << 0; // volatile
        q |= 1ull << 5; // clobbers flags
        const uint64_t asm_id = (uint64_t)fn_->asm_clobber_lists.size();
        fn_->asm_clobber_lists.push_back(std::move(clob));
        q |= (asm_id & 0xFFFFFFull) << 8;
        ia.imm = q;
        emit(current_block_, std::move(ia));
    }

    // --- LOAD del binding (lee rax) -> bitmask u64 ---
    const ir::IrValueId v_feat = fn_->new_value(ir::IrType::U64);
    {
        ir::IrInstr ld{};
        ld.op = ir::IrOp::LOAD;
        ld.type = ir::IrType::U64;
        ld.dst = v_feat;
        ld.operands = {rax_slot};
        ld.source_line = ln;
        emit(current_block_, std::move(ld));
    }

    // --- STORE del bitmask al slot global __vx_cpu_features ---
    const ir::IrValueId v_gaddr = emit_str_lit_addr(slot, ln);
    {
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ir::IrType::I64;
        st.dst = ir::IR_NO_VALUE;
        st.operands = {v_feat, v_gaddr};
        st.source_line = ln;
        emit(current_block_, std::move(st));
    }

    // --- RET void ---
    {
        ir::IrInstr ret{};
        ret.op = ir::IrOp::RET;
        ret.type = ir::IrType::VOID;
        ret.dst = ir::IR_NO_VALUE;
        ret.source_line = ln;
        emit(current_block_, std::move(ret));
    }
    block_terminated_ = true;

    fn_ = saved_fn;
    current_block_ = saved_block;
    block_terminated_ = saved_terminated;
    out_mod_->add_function(std::move(hf));

    // Registrar el import nativo del runner de inline asm (igual que lower_asm)
    // para el path bytecode/interp; en native_poo_ no se usa, pero es
    // idempotente y mantiene la coherencia.
    out_mod_->register_native_import("vrt", "inline_asm_exec");
    return slot;
}

// ---------------------------------------------------------------------
// CPU dispatch (Inc 2): memcpy multi-versionado por tabla de punteros.
//
// Tres piezas:
//   1. Global __vx_memcpy_fp (u64 en ".data"): puntero a la variante elegida.
//   2. Variantes:
//        - __vx_memcpy_base(dst, src, n): rep movsb (segura, cualquier n).
//        - __vx_memcpy_avx2(dst, src, n): 32B con vmovdqu ymm + cola
//          byte-a-byte (sin leer/escribir fuera de [0,n)).
//   3. __vx_memcpy_init(): lee __vx_cpu_features, si el bit AVX2 (bit 4)
//      esta activo setea fp = &__vx_memcpy_avx2, si no &__vx_memcpy_base.
//
// El wiring (run()) prepone `call __vx_cpu_init` + `call __vx_memcpy_init`
// al entry de main (en ese orden).  Los memcpy del concat/slice/+= bajan a
// `call [__vx_memcpy_fp]` (CALLIND) en lugar de rep movsb inline.
//
// La direccion de cada variante se obtiene via LABEL_ADDR (en AOT baja a una
// reloc "fnsym:<name>" que el driver resuelve contra el offset de la funcion).
// Todo es PURE_NATIVE (CALL/CALLIND/LABEL_ADDR/INLINE_ASM/MEMCPY/LOAD/STORE).
// ---------------------------------------------------------------------
uint64_t Lowering::ensure_memcpy_dispatch() {
    cpu_dispatch_used_ = true;
    if (memcpy_helpers_emitted_) return memcpy_fp_slot_;
    memcpy_helpers_emitted_ = true;

    // 1. Global __vx_memcpy_fp (8 bytes zero-init) en ".data" (writable: el
    //    init le hace STORE en runtime).  NON_DEDUP + FORCE_EMIT como el slot
    //    de features.
    {
        std::vector<uint8_t> zero(8, 0);
        const uint64_t slot = static_cast<uint64_t>(
            out_mod_->static_data.push_back(std::move(zero)));
        auto &m = out_mod_->static_data.meta_at(slot);
        m.section_name = ".data";
        m.flags |=
            ir::IrModule::SD_FLAG_NON_DEDUP | ir::IrModule::SD_FLAG_FORCE_EMIT;
        // Global de programa: unificar el slot cross-module en el merge.
        m.shared_key = "__vx_memcpy_fp";
        memcpy_fp_slot_ = slot;
    }
    const uint64_t fp_slot = memcpy_fp_slot_;

    ir::IrFunction *saved_fn = fn_;
    ir::IrBlockId saved_block = current_block_;
    bool saved_terminated = block_terminated_;
    const uint32_t ln = 0;

    // --- Helper para construir una variante memcpy(dst, src, n) -------------
    // body_emitter recibe los SSA de los 3 params + el bloque entry activo y
    // emite el cuerpo (terminando con RET void).
    auto build_variant =
        [&](const std::string &name,
            const std::function<void(ir::IrValueId, ir::IrValueId,
                                     ir::IrValueId)> &body_emitter) {
            ir::IrFunction hf;
            hf.name = name;
            hf.ret_type = ir::IrType::VOID;
            const ir::IrValueId p_dst = hf.new_value(ir::IrType::PTR, "%dst");
            hf.values[p_dst].is_param = true;
            hf.values[p_dst].is_host_ptr = true;
            hf.params.push_back(p_dst);
            const ir::IrValueId p_src = hf.new_value(ir::IrType::PTR, "%src");
            hf.values[p_src].is_param = true;
            hf.values[p_src].is_host_ptr = true;
            hf.params.push_back(p_src);
            const ir::IrValueId p_n = hf.new_value(ir::IrType::I64, "%n");
            hf.values[p_n].is_param = true;
            hf.params.push_back(p_n);
            const ir::IrBlockId e = hf.new_block("entry");

            fn_ = &hf;
            current_block_ = e;
            block_terminated_ = false;

            body_emitter(p_dst, p_src, p_n);

            block_terminated_ = true;
            out_mod_->add_function(std::move(hf));
        };

    // Cuerpo "base": MEMCPY (rep movsb) + RET void.  Cubre cualquier n,
    // incluido 0 (rep movsb con rcx=0 no copia nada).
    auto emit_base_body = [&](ir::IrValueId dst, ir::IrValueId src,
                              ir::IrValueId n) {
        ir::IrInstr mc{};
        mc.op = ir::IrOp::MEMCPY;
        mc.type = ir::IrType::I8;
        mc.dst = ir::IR_NO_VALUE;
        mc.operands = {dst, src, n};
        mc.source_line = ln;
        emit(current_block_, std::move(mc));
        ir::IrInstr ret{};
        ret.op = ir::IrOp::RET;
        ret.type = ir::IrType::VOID;
        ret.dst = ir::IR_NO_VALUE;
        ret.source_line = ln;
        emit(current_block_, std::move(ret));
    };

    build_variant("__vx_memcpy_base", emit_base_body);

    // --- Variante AVX2: vmovdqu ymm de a 32 bytes + cola byte-a-byte ---------
    // Helper INLINE_ASM auto-contenido: los 3 params (dst/src/n) llegan en los
    // arg_regs del ABI; los fijamos a rdi/rsi/rdx via AsmRegBinding (el
    // selector precolorea y el regalloc inserta el move desde el arg_reg).  El
    // bloque asm copia bloques de 32 B con vmovdqu ymm0 mientras queden >= 32
    // bytes, luego la cola (< 32) byte-a-byte.  CRITICO valgrind: nunca
    // lee/escribe fuera de [0, n) -- el chunk de 32 solo corre con n >= 32; el
    // resto byte a byte. vzeroupper al final (penalizacion AVX<->SSE).  Labels
    // intra-bloque las resuelve Keystone.  El asm clobbea rax + ymm0 + flags +
    // memoria; rdi/rsi/ rdx son operandos (bindings), no clobbers.
    {
        ir::IrFunction hf;
        hf.name = "__vx_memcpy_avx2";
        hf.ret_type = ir::IrType::VOID;
        const ir::IrValueId p_dst = hf.new_value(ir::IrType::PTR, "%dst");
        hf.values[p_dst].is_param = true;
        hf.values[p_dst].is_host_ptr = true;
        hf.params.push_back(p_dst);
        const ir::IrValueId p_src = hf.new_value(ir::IrType::PTR, "%src");
        hf.values[p_src].is_param = true;
        hf.values[p_src].is_host_ptr = true;
        hf.params.push_back(p_src);
        const ir::IrValueId p_n = hf.new_value(ir::IrType::I64, "%n");
        hf.values[p_n].is_param = true;
        hf.params.push_back(p_n);
        const ir::IrBlockId e = hf.new_block("entry");

        fn_ = &hf;
        current_block_ = e;
        block_terminated_ = false;

        // 3 ALLOCA estables + AsmRegBinding (dst->rdi, src->rsi, n->rdx).
        // El selector convierte el STORE param->alloca en `MOV rXX, param` y
        // lista la alloca como operando del INLINE_ASM (input).
        auto make_binding = [&](const char *reg, ir::IrType ty,
                                ir::IrValueId param,
                                const char *dbg) -> ir::IrValueId {
            ir::IrValueId slot = fn_->new_value(ir::IrType::PTR);
            fn_->values[slot].is_host_ptr = true;
            {
                ir::IrInstr al{};
                al.op = ir::IrOp::ALLOCA;
                al.type = ir::IrType::I8;
                al.dst = slot;
                al.imm = 8;
                al.host_alloca = true;
                al.source_line = ln;
                emit(current_block_, std::move(al));
            }
            {
                ir::AsmRegBinding b{slot, reg, ty, false, dbg};
                b.reg_class = reg; // registro concreto.
                fn_->asm_reg_bindings.push_back(std::move(b));
            }
            // STORE param -> alloca (carga el input en el reg fijado).
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = ir::IrType::I64;
            st.dst = ir::IR_NO_VALUE;
            st.operands = {param, slot};
            st.source_line = ln;
            emit(current_block_, std::move(st));
            return slot;
        };
        const ir::IrValueId s_dst =
            make_binding("rdi", ir::IrType::U64, p_dst, "__mc_dst");
        const ir::IrValueId s_src =
            make_binding("rsi", ir::IrType::U64, p_src, "__mc_src");
        const ir::IrValueId s_n =
            make_binding("rdx", ir::IrType::U64, p_n, "__mc_n");

        // Cuerpo NASM.  rdi=dst, rsi=src, rdx=n.  rax = scratch del byte de
        // cola.
        const std::string asm_body =
            ".chunk:\n"
            "cmp rdx, 0x20\n" // mientras queden >= 32 bytes
            "jb .tail\n"
            "vmovdqu ymm0, [rsi]\n" // 32 B src -> ymm0
            "vmovdqu [rdi], ymm0\n" // ymm0 -> 32 B dst
            "add rsi, 0x20\n"
            "add rdi, 0x20\n"
            "sub rdx, 0x20\n"
            "jmp .chunk\n"
            ".tail:\n"
            "test rdx, rdx\n" // cola (< 32) byte a byte
            "jz .done\n"
            ".tloop:\n"
            "mov al, [rsi]\n"
            "mov [rdi], al\n"
            "inc rsi\n"
            "inc rdi\n"
            "dec rdx\n"
            "jnz .tloop\n"
            ".done:\n"
            "vzeroupper\n";
        {
            ir::IrInstr ia{};
            ia.op = ir::IrOp::INLINE_ASM;
            ia.type = ir::IrType::VOID;
            ia.dst = ir::IR_NO_VALUE;
            ia.source_line = ln;
            ia.func_name = asm_body;
            ia.preserve = true; // volatile

            // Operandos: las 3 allocas binding (inputs).
            ia.operands = {s_dst, s_src, s_n};

            // Clobbers: rax (scratch de la cola) + memoria.  ymm0 lo asume el
            // ABI (caller-saved); rdi/rsi/rdx son operandos.  flags por las
            // comparaciones del loop.
            std::vector<std::string> clob = {"rax", "memory"};
            uint64_t q = 0;
            q |= 1ull << 0; // volatile
            q |= 1ull << 4; // clobbers memory
            q |= 1ull << 5; // clobbers flags
            const uint64_t asm_id = (uint64_t)fn_->asm_clobber_lists.size();
            fn_->asm_clobber_lists.push_back(std::move(clob));
            q |= (asm_id & 0xFFFFFFull) << 8;
            ia.imm = q;
            emit(current_block_, std::move(ia));
        }

        // RET void.
        {
            ir::IrInstr ret{};
            ret.op = ir::IrOp::RET;
            ret.type = ir::IrType::VOID;
            ret.dst = ir::IR_NO_VALUE;
            ret.source_line = ln;
            emit(current_block_, std::move(ret));
        }
        block_terminated_ = true;
        out_mod_->add_function(std::move(hf));
        out_mod_->register_native_import("vrt", "inline_asm_exec");
    }

    // --- 3. __vx_memcpy_init(): setea el fp segun el bit AVX2 --------------
    {
        ir::IrFunction hf;
        hf.name = "__vx_memcpy_init";
        hf.ret_type = ir::IrType::VOID;
        const ir::IrBlockId e = hf.new_block("entry");
        fn_ = &hf;
        current_block_ = e;
        block_terminated_ = false;

        // Helper local: STORE &<fn_name> al global fp en el bloque actual.
        // (Sin BR; el caller decide el terminador.)  Reusado por el camino
        // de override (RET directo) y por las ramas del dispatch cpuid.
        auto emit_store_fp = [&](const std::string &fn_name) {
            ir::IrValueId v_addr = emit_label_addr(fn_name, ln);
            ir::IrValueId v_gaddr = emit_str_lit_addr(fp_slot, ln, true);
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = ir::IrType::I64;
            st.dst = ir::IR_NO_VALUE;
            st.operands = {v_addr, v_gaddr};
            st.source_line = ln;
            emit(current_block_, std::move(st));
        };

        // CPU dispatch Inc 4: si el usuario declaro @HelperOverride(memcpy),
        // el fp apunta a SU funcion de forma INCONDICIONAL (sin leer cpuid).
        // Esto reemplaza el memcpy del build entero por el del usuario.
        if (!memcpy_override_.empty()) {
            emit_store_fp(memcpy_override_);
            ir::IrInstr ret{};
            ret.op = ir::IrOp::RET;
            ret.type = ir::IrType::VOID;
            ret.dst = ir::IR_NO_VALUE;
            ret.source_line = ln;
            emit(current_block_, std::move(ret));
            block_terminated_ = true;
            out_mod_->add_function(std::move(hf));
            fn_ = saved_fn;
            current_block_ = saved_block;
            block_terminated_ = saved_terminated;
            return fp_slot;
        }

        // feat = LOAD i64 [__vx_cpu_features].
        const uint64_t feat_slot = ensure_cpu_features_global();
        ir::IrValueId v_faddr = emit_str_lit_addr(feat_slot, ln, true);
        ir::IrValueId v_feat = fn_->new_value(ir::IrType::I64);
        {
            ir::IrInstr ld{};
            ld.op = ir::IrOp::LOAD;
            ld.type = ir::IrType::I64;
            ld.dst = v_feat;
            ld.operands = {v_faddr};
            ld.source_line = ln;
            emit(current_block_, std::move(ld));
        }
        // has_avx2 = (feat >> 4) & 1.  (bit4 = AVX2.)
        ir::IrValueId v_sh = fn_->new_value(ir::IrType::I64);
        {
            ir::IrValueId v4 = emit_const(ir::IrType::I64, 4, ln);
            ir::IrInstr sh{};
            sh.op = ir::IrOp::SHR; // logico (feat es bitmask)
            sh.type = ir::IrType::I64;
            sh.dst = v_sh;
            sh.operands = {v_feat, v4};
            sh.source_line = ln;
            emit(current_block_, std::move(sh));
        }
        ir::IrValueId v_bit = fn_->new_value(ir::IrType::I64);
        {
            ir::IrValueId v1 = emit_const(ir::IrType::I64, 1, ln);
            ir::IrInstr an{};
            an.op = ir::IrOp::AND;
            an.type = ir::IrType::I64;
            an.dst = v_bit;
            an.operands = {v_sh, v1};
            an.source_line = ln;
            emit(current_block_, std::move(an));
        }
        ir::IrValueId v_has = fn_->new_value(ir::IrType::BOOL);
        {
            ir::IrValueId v0 = emit_const(ir::IrType::I64, 0, ln);
            ir::IrInstr cm{};
            cm.op = ir::IrOp::CMP_NE;
            cm.type = ir::IrType::BOOL;
            cm.dst = v_has;
            cm.operands = {v_bit, v0};
            cm.source_line = ln;
            emit(current_block_, std::move(cm));
        }

        // Ramas: avx2 -> fp=&avx2 ; base -> fp=&base ; join -> RET.
        const ir::IrBlockId bb_avx2 = fn_->new_block("avx2");
        const ir::IrBlockId bb_base = fn_->new_block("base");
        const ir::IrBlockId bb_join = fn_->new_block("join");
        {
            ir::IrInstr br{};
            br.op = ir::IrOp::BR_COND;
            br.type = ir::IrType::VOID;
            br.dst = ir::IR_NO_VALUE;
            br.operands = {v_has};
            br.target_block = bb_avx2;
            br.false_block = bb_base;
            br.source_line = ln;
            emit(current_block_, std::move(br));
            fn_->blocks[current_block_].succs.push_back(bb_avx2);
            fn_->blocks[current_block_].succs.push_back(bb_base);
            fn_->blocks[bb_avx2].preds.push_back(current_block_);
            fn_->blocks[bb_base].preds.push_back(current_block_);
        }

        // Helper: en el bloque actual, STORE &<fn_name> al global fp + BR join.
        auto store_fp_and_join = [&](const std::string &fn_name) {
            emit_store_fp(fn_name);
            ir::IrInstr br{};
            br.op = ir::IrOp::BR;
            br.type = ir::IrType::VOID;
            br.dst = ir::IR_NO_VALUE;
            br.target_block = bb_join;
            br.source_line = ln;
            emit(current_block_, std::move(br));
            fn_->blocks[current_block_].succs.push_back(bb_join);
            fn_->blocks[bb_join].preds.push_back(current_block_);
        };

        current_block_ = bb_avx2;
        store_fp_and_join("__vx_memcpy_avx2");
        current_block_ = bb_base;
        store_fp_and_join("__vx_memcpy_base");

        current_block_ = bb_join;
        {
            ir::IrInstr ret{};
            ret.op = ir::IrOp::RET;
            ret.type = ir::IrType::VOID;
            ret.dst = ir::IR_NO_VALUE;
            ret.source_line = ln;
            emit(current_block_, std::move(ret));
        }
        block_terminated_ = true;
        out_mod_->add_function(std::move(hf));
    }

    fn_ = saved_fn;
    current_block_ = saved_block;
    block_terminated_ = saved_terminated;
    return fp_slot;
}

// ---------------------------------------------------------------------
// AUTO multiversion (--float-isa auto): despacha el MAIN por cpuid.
//
// Problema: main es el entry; el _start stub lo llama por NOMBRE.  Si lo
// multiversionaramos directamente (main$sse2/avx2/avx512) nadie correria el
// init (cpuid) antes de elegir la variante.  Fix: reducir "multiversionar
// main" a "despachar un helper":
//   1. El main del usuario se RENOMBRA a __vx_main_body (un helper VEC
//      normal; el driver lo compila 3x: $sse2/$avx2/$avx512).
//   2. Se sintetiza un main fino = { <inits> ; r = CALLIND [__vx_main_body$fp]
//      (args...) ; ret r }.  Los inits (cpu_init + auto_init) los prepone
//      run() en su entry, asi corren ANTES del CALLIND que lee el fp.
//   3. __vx_auto_init() elige la variante por cpuid (AVX512F bit7 > AVX2 bit4
//      > SSE2) y la guarda en __vx_main_body$fp.
// El fp se referencia por INDICE (STR_LIT_ADDR), no por nombre -> no hace
// falta trampolin de bytes crudos ni reloc DATA_REL32: todo es IR estandar
// (CALLIND + LABEL_ADDR + LOAD/STORE), PURE_NATIVE.
void Lowering::ensure_auto_multiversion(ir::IrModule &out_module) {
    if (!native_poo_ || !aot_auto_vec_) return; // solo AOT --float-isa auto
    if (auto_dispatch_emitted_) return;         // idempotente

    // Detector de ops VEC_* (idem al driver): solo despachamos lo vectorizado.
    auto fn_has_vec = [](const ir::IrFunction &f) -> bool {
        for (const auto &b : f.blocks)
            for (const auto &in : b.instrs) {
                const auto op = in.op;
                if (op == ir::IrOp::VEC_BINOP || op == ir::IrOp::VEC_UNOP ||
                    op == ir::IrOp::VEC_FMA || op == ir::IrOp::VEC_BINOP_S ||
                    op == ir::IrOp::VEC_BCAST || op == ir::IrOp::VEC_ACC_ZERO ||
                    op == ir::IrOp::VEC_ACC_ADD ||
                    op == ir::IrOp::VEC_ACC_FMA ||
                    op == ir::IrOp::VEC_ACC_STORE ||
                    op == ir::IrOp::VEC_ACC_COMBINE)
                    return true;
            }
        return false;
    };

    // Recolectar TODAS las funciones con ops VEC (main + helpers).  Capturar
    // firma + RENOMBRAR en sitio ANTES de cualquier add_function (que
    // realocaria out_module.functions e invalidaria indices/punteros).  El
    // renombrado NO anñade funciones, asi que el primer bucle es seguro.
    struct PInfo {
        ir::IrType ty;
        bool host;
    };
    struct MvEntry {
        std::string wrapper_name; // nombre que ven los callers (el original)
        std::string
            body_name; // cuerpo multiversionado (el driver lo compila 3x)
        ir::IrType ret;
        std::vector<PInfo> params;
        uint64_t fp_slot = 0; // se rellena en la fase de wrappers
    };
    std::vector<MvEntry> mv;
    for (size_t i = 0; i < out_module.functions.size(); ++i) {
        ir::IrFunction &f = out_module.functions[i];
        if (!fn_has_vec(f)) continue;
        MvEntry e;
        e.wrapper_name = f.name;
        // main mantiene el nombre historico __vx_main_body; los helpers usan
        // <nombre>$mv.  El driver suffija $sse2/$avx2/$avx512 a estos nombres.
        e.body_name =
            (f.name == "main") ? std::string("__vx_main_body") : f.name + "$mv";
        e.ret = f.ret_type;
        for (ir::IrValueId pid : f.params)
            e.params.push_back({f.values[pid].type, f.values[pid].is_host_ptr});
        f.name = e.body_name; // renombrado en sitio (sin add_function)
        mv.push_back(std::move(e));
    }
    if (mv.empty()) return; // ninguna funcion vectorizada -> nada que despachar

    auto_dispatch_emitted_ = true;
    cpu_dispatch_used_ = true;
    // Garantizar el global de features + __vx_cpu_init (puede anñadir una
    // funcion -> realoc, pero ya no tenemos referencias vivas a las funciones).
    (void)ensure_cpu_features_global();

    ir::IrFunction *saved_fn = fn_;
    ir::IrBlockId saved_block = current_block_;
    bool saved_terminated = block_terminated_;
    const uint32_t ln = 0;

    // Por cada funcion VEC: slot fp <body>$fp + wrapper sintetico (nombre
    // original) que hace CALLIND a la variante elegida.  Los callers siguen
    // llamando por nombre -> caen en el wrapper -> despacho transparente.
    for (auto &e : mv) {
        {
            std::vector<uint8_t> zero(8, 0);
            e.fp_slot = static_cast<uint64_t>(
                out_module.static_data.push_back(std::move(zero)));
            auto &m = out_module.static_data.meta_at(e.fp_slot);
            m.section_name = ".data";
            m.flags |= ir::IrModule::SD_FLAG_NON_DEDUP |
                       ir::IrModule::SD_FLAG_FORCE_EMIT;
            m.shared_key = e.body_name + "$fp";
        }
        ir::IrFunction w;
        w.name = e.wrapper_name;
        w.ret_type = e.ret;
        std::vector<ir::IrValueId> sparams;
        for (const auto &pi : e.params) {
            const ir::IrValueId pv = w.new_value(pi.ty);
            w.values[pv].is_param = true;
            w.values[pv].is_host_ptr = pi.host;
            w.params.push_back(pv);
            sparams.push_back(pv);
        }
        const ir::IrBlockId be = w.new_block("entry");
        fn_ = &w;
        current_block_ = be;
        block_terminated_ = false;

        // v_fpaddr = &<body>$fp ; v_fp = LOAD i64 [v_fpaddr].
        ir::IrValueId v_fpaddr = w.new_value(ir::IrType::PTR);
        w.values[v_fpaddr].is_host_ptr = true;
        {
            ir::IrInstr la{};
            la.op = ir::IrOp::STR_LIT_ADDR;
            la.type = ir::IrType::PTR;
            la.dst = v_fpaddr;
            la.imm = e.fp_slot;
            la.source_line = ln;
            w.append(current_block_, std::move(la));
        }
        ir::IrValueId v_fp = w.new_value(ir::IrType::PTR);
        w.values[v_fp].is_host_ptr = true;
        {
            ir::IrInstr ld{};
            ld.op = ir::IrOp::LOAD;
            ld.type = ir::IrType::I64;
            ld.dst = v_fp;
            ld.operands = {v_fpaddr};
            ld.source_line = ln;
            w.append(current_block_, std::move(ld));
        }
        // CALLIND v_fp(args...) -> r.
        ir::IrValueId v_ret = ir::IR_NO_VALUE;
        {
            ir::IrInstr ci{};
            ci.op = ir::IrOp::CALLIND;
            ci.type = e.ret;
            ci.func_ptr = v_fp;
            ci.operands = sparams;
            if (e.ret != ir::IrType::VOID) {
                v_ret = w.new_value(e.ret);
                ci.dst = v_ret;
            } else {
                ci.dst = ir::IR_NO_VALUE;
            }
            ci.source_line = ln;
            w.append(current_block_, std::move(ci));
        }
        {
            ir::IrInstr ret{};
            ret.op = ir::IrOp::RET;
            ret.type = e.ret;
            ret.dst = ir::IR_NO_VALUE;
            if (e.ret != ir::IrType::VOID) ret.operands.push_back(v_ret);
            ret.source_line = ln;
            w.append(current_block_, std::move(ret));
        }
        block_terminated_ = true;
        out_module.add_function(std::move(w));
    }

    // __vx_auto_init(): un solo cpuid -> tres ramas (AVX512F bit7 > AVX2 bit4 >
    // SSE2); cada rama setea el fp de TODAS las funciones VEC a su variante del
    // ancho elegido.  (La decision de ISA es global a la CPU -> una sola vez.)
    {
        ir::IrFunction hf;
        hf.name = "__vx_auto_init";
        hf.ret_type = ir::IrType::VOID;
        const ir::IrBlockId e = hf.new_block("entry");
        fn_ = &hf;
        current_block_ = e;
        block_terminated_ = false;

        // STORE &<variante> al fp dado, en el bloque actual (sin terminador).
        auto emit_store_fp = [&](const std::string &variant, uint64_t fp_slot) {
            ir::IrValueId v_addr = emit_label_addr(variant, ln);
            ir::IrValueId v_gaddr = emit_str_lit_addr(fp_slot, ln, true);
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = ir::IrType::I64;
            st.dst = ir::IR_NO_VALUE;
            st.operands = {v_addr, v_gaddr};
            st.source_line = ln;
            emit(current_block_, std::move(st));
        };

        // feat = LOAD i64 [__vx_cpu_features].
        const uint64_t feat_slot = ensure_cpu_features_global();
        ir::IrValueId v_faddr = emit_str_lit_addr(feat_slot, ln, true);
        ir::IrValueId v_feat = fn_->new_value(ir::IrType::I64);
        {
            ir::IrInstr ld{};
            ld.op = ir::IrOp::LOAD;
            ld.type = ir::IrType::I64;
            ld.dst = v_feat;
            ld.operands = {v_faddr};
            ld.source_line = ln;
            emit(current_block_, std::move(ld));
        }
        // bit_set(n): has = ((feat >> n) & 1) != 0.
        auto bit_set = [&](int n) -> ir::IrValueId {
            ir::IrValueId v_sh = fn_->new_value(ir::IrType::I64);
            {
                ir::IrValueId vn = emit_const(ir::IrType::I64, (uint64_t)n, ln);
                ir::IrInstr sh{};
                sh.op = ir::IrOp::SHR;
                sh.type = ir::IrType::I64;
                sh.dst = v_sh;
                sh.operands = {v_feat, vn};
                sh.source_line = ln;
                emit(current_block_, std::move(sh));
            }
            ir::IrValueId v_bit = fn_->new_value(ir::IrType::I64);
            {
                ir::IrValueId v1 = emit_const(ir::IrType::I64, 1, ln);
                ir::IrInstr an{};
                an.op = ir::IrOp::AND;
                an.type = ir::IrType::I64;
                an.dst = v_bit;
                an.operands = {v_sh, v1};
                an.source_line = ln;
                emit(current_block_, std::move(an));
            }
            ir::IrValueId v_has = fn_->new_value(ir::IrType::BOOL);
            {
                ir::IrValueId v0 = emit_const(ir::IrType::I64, 0, ln);
                ir::IrInstr cm{};
                cm.op = ir::IrOp::CMP_NE;
                cm.type = ir::IrType::BOOL;
                cm.dst = v_has;
                cm.operands = {v_bit, v0};
                cm.source_line = ln;
                emit(current_block_, std::move(cm));
            }
            return v_has;
        };

        const ir::IrBlockId bb_512 = fn_->new_block("pick512");
        const ir::IrBlockId bb_not512 = fn_->new_block("not512");
        const ir::IrBlockId bb_2 = fn_->new_block("pick2");
        const ir::IrBlockId bb_sse = fn_->new_block("picksse");
        const ir::IrBlockId bb_join = fn_->new_block("join");

        auto branch = [&](ir::IrValueId cond, ir::IrBlockId t,
                          ir::IrBlockId f) {
            ir::IrInstr br{};
            br.op = ir::IrOp::BR_COND;
            br.type = ir::IrType::VOID;
            br.dst = ir::IR_NO_VALUE;
            br.operands = {cond};
            br.target_block = t;
            br.false_block = f;
            br.source_line = ln;
            emit(current_block_, std::move(br));
            fn_->blocks[current_block_].succs.push_back(t);
            fn_->blocks[current_block_].succs.push_back(f);
            fn_->blocks[t].preds.push_back(current_block_);
            fn_->blocks[f].preds.push_back(current_block_);
        };
        // En el bloque actual: setea el fp de TODAS las entries a la variante
        // del sufijo dado + BR a join.
        auto store_all_and_join = [&](const char *suffix) {
            for (const auto &en : mv)
                emit_store_fp(en.body_name + suffix, en.fp_slot);
            ir::IrInstr br{};
            br.op = ir::IrOp::BR;
            br.type = ir::IrType::VOID;
            br.dst = ir::IR_NO_VALUE;
            br.target_block = bb_join;
            br.source_line = ln;
            emit(current_block_, std::move(br));
            fn_->blocks[current_block_].succs.push_back(bb_join);
            fn_->blocks[bb_join].preds.push_back(current_block_);
        };

        // entry: has512 = bit7 ; br has512 -> pick512 : not512.
        ir::IrValueId v512 = bit_set(7);
        branch(v512, bb_512, bb_not512);
        current_block_ = bb_512;
        store_all_and_join("$avx512");
        current_block_ = bb_not512;
        ir::IrValueId v2 = bit_set(4);
        branch(v2, bb_2, bb_sse);
        current_block_ = bb_2;
        store_all_and_join("$avx2");
        current_block_ = bb_sse;
        store_all_and_join("$sse2");
        // join: RET void.
        current_block_ = bb_join;
        {
            ir::IrInstr ret{};
            ret.op = ir::IrOp::RET;
            ret.type = ir::IrType::VOID;
            ret.dst = ir::IR_NO_VALUE;
            ret.source_line = ln;
            emit(current_block_, std::move(ret));
        }
        block_terminated_ = true;
        out_module.add_function(std::move(hf));
    }

    fn_ = saved_fn;
    current_block_ = saved_block;
    block_terminated_ = saved_terminated;
}

void Lowering::emit_memcpy_dispatched(ir::IrValueId dst, ir::IrValueId src,
                                      ir::IrValueId len, uint32_t line) {
    // Asegura el global fp + variantes + init (idempotente) y marca el uso
    // para que el wiring prepone __vx_memcpy_init en main.
    const uint64_t fp_slot = ensure_memcpy_dispatch();

    // v_fpaddr = &__vx_memcpy_fp ; v_fp = LOAD i64 [v_fpaddr].
    ir::IrValueId v_fpaddr = emit_str_lit_addr(fp_slot, line, true);
    ir::IrValueId v_fp = fn_->new_value(ir::IrType::PTR);
    fn_->values[v_fp].is_host_ptr = true;
    {
        ir::IrInstr ld{};
        ld.op = ir::IrOp::LOAD;
        ld.type = ir::IrType::I64;
        ld.dst = v_fp;
        ld.operands = {v_fpaddr};
        ld.source_line = line;
        emit(current_block_, std::move(ld));
    }
    // CALLIND v_fp(dst, src, len) -> void.
    ir::IrInstr ci{};
    ci.op = ir::IrOp::CALLIND;
    ci.type = ir::IrType::VOID;
    ci.dst = ir::IR_NO_VALUE;
    ci.func_ptr = v_fp;
    ci.operands = {dst, src, len};
    ci.source_line = line;
    emit(current_block_, std::move(ci));
}

// ---------------------------------------------------------------------
// CPU dispatch (Inc 5a): strcmp/strlen multi-versionados por tabla de
// punteros.  Foundation para que una libreria stdlib provea variantes SIMD
// via @HelperOverride(strcmp)/(strlen).  A DIFERENCIA de memcpy, el
// compilador NO hace cpuid aqui: el default es el BASELINE escalar
// (__vx_strcmp_base / __vx_strlen_base, la impl actual del compilador);
// la variante SIMD vendra de una lib importada (Inc 5c) via @HelperOverride.
//
// Tres piezas:
//   1. Globals __vx_strcmp_fp / __vx_strlen_fp (u64 en ".data").
//   2. Baselines __vx_strcmp_base / __vx_strlen_base (los renombrados
//      ensure_strcmp_helper / ensure_strlen_helper; siempre presentes,
//      llamables por nombre para que un override delegue a ellos).
//   3. __vx_strdisp_init(): setea cada fp al override del usuario (si
//      declarado @HelperOverride) o al baseline.
//
// run() prepone `call __vx_strdisp_init` al entry de main (junto al resto
// de inits).  Los call sites de strcmp/strlen bajan a `call [fp]` (CALLIND)
// en native_poo_.  Todo PURE_NATIVE (CALL/CALLIND/LABEL_ADDR/LOAD/STORE).
// ---------------------------------------------------------------------
void Lowering::ensure_strdisp() {
    cpu_dispatch_used_ = true;
    if (strdisp_emitted_) return;
    strdisp_emitted_ = true;

    // 1. Globals fp (8 bytes zero-init) en ".data" (writable: el init les
    //    hace STORE en runtime).  NON_DEDUP + FORCE_EMIT como los demas fp.
    auto make_fp_slot = [&](const char *shared_key) -> uint64_t {
        std::vector<uint8_t> zero(8, 0);
        const uint64_t slot = static_cast<uint64_t>(
            out_mod_->static_data.push_back(std::move(zero)));
        auto &m = out_mod_->static_data.meta_at(slot);
        m.section_name = ".data";
        m.flags |=
            ir::IrModule::SD_FLAG_NON_DEDUP | ir::IrModule::SD_FLAG_FORCE_EMIT;
        // Global de programa: unificar el slot cross-module en el merge.
        m.shared_key = shared_key;
        return slot;
    };
    strcmp_fp_slot_ = make_fp_slot("__vx_strcmp_fp");
    strlen_fp_slot_ = make_fp_slot("__vx_strlen_fp");

    // 2. Asegurar los baselines (emiten __vx_strcmp_base / __vx_strlen_base).
    (void)ensure_strcmp_helper();
    (void)ensure_strlen_helper();

    // 3. __vx_strdisp_init(): para cada fp, STORE &<variante> al global.
    ir::IrFunction *saved_fn = fn_;
    ir::IrBlockId saved_block = current_block_;
    bool saved_terminated = block_terminated_;
    const uint32_t ln = 0;

    ir::IrFunction hf;
    hf.name = "__vx_strdisp_init";
    hf.ret_type = ir::IrType::VOID;
    const ir::IrBlockId e = hf.new_block("entry");
    fn_ = &hf;
    current_block_ = e;
    block_terminated_ = false;

    // STORE &<fn_name> al global fp del slot dado.
    auto emit_store_fp = [&](uint64_t fp_slot, const std::string &fn_name) {
        ir::IrValueId v_addr = emit_label_addr(fn_name, ln);
        ir::IrValueId v_gaddr = emit_str_lit_addr(fp_slot, ln, true);
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ir::IrType::I64;
        st.dst = ir::IR_NO_VALUE;
        st.operands = {v_addr, v_gaddr};
        st.source_line = ln;
        emit(current_block_, std::move(st));
    };

    // fp = override del usuario si lo hay; si no, el baseline.
    emit_store_fp(strcmp_fp_slot_, strcmp_override_.empty()
                                       ? std::string("__vx_strcmp_base")
                                       : strcmp_override_);
    emit_store_fp(strlen_fp_slot_, strlen_override_.empty()
                                       ? std::string("__vx_strlen_base")
                                       : strlen_override_);
    {
        ir::IrInstr ret{};
        ret.op = ir::IrOp::RET;
        ret.type = ir::IrType::VOID;
        ret.dst = ir::IR_NO_VALUE;
        ret.source_line = ln;
        emit(current_block_, std::move(ret));
    }
    block_terminated_ = true;
    out_mod_->add_function(std::move(hf));

    fn_ = saved_fn;
    current_block_ = saved_block;
    block_terminated_ = saved_terminated;
}


void Lowering::emit_word_copy_loop(ir::IrValueId dst_base,
                                   ir::IrValueId src_base, ir::IrValueId v_len,
                                   uint32_t source_line) {
    // Copia v_len bytes de src_base -> dst_base.  Estrategia: dos loops.
    //   (1) Loop de PALABRA: mientras i + 8 <= len, copia un qword (LOAD
    //       i64 + STORE i64) y avanza i += 8.  ~8x menos iteraciones que
    //       byte-a-byte.
    //   (2) Loop de COLA: copia los <8 bytes restantes byte-a-byte
    //       (mientras i < len).
    // El contador i vive en un ALLOCA de 8 bytes (mem2reg lo promueve a
    // PHI en O2).  Las direcciones src/dst se recalculan con ADD por
    // iteracion.  Sin registros fijos -> cero impacto en el regalloc.
    // Correctness: nunca lee/escribe fuera de [base, base+len) (el qword
    // solo corre cuando i+8 <= len; la cola cubre el resto exacto).  El
    // buffer destino tiene cap = total+1 bytes -> margen suficiente.

    // Helper local: addr = base + off (off es un IrValue I64).
    // Slot del contador i = 0 (compartido por ambos loops).
    const ir::IrValueId v_i_slot = fn_->new_value(ir::IrType::PTR);
    {
        ir::IrInstr al{};
        al.op = ir::IrOp::ALLOCA;
        al.type = ir::IrType::I8;
        al.dst = v_i_slot;
        al.imm = 8;
        al.source_line = source_line;
        emit(current_block_, std::move(al));
    }
    {
        ir::IrValueId v_z = emit_const(ir::IrType::I64, 0, source_line);
        ir::IrInstr st{};
        st.op = ir::IrOp::STORE;
        st.type = ir::IrType::I64;
        st.dst = ir::IR_NO_VALUE;
        st.operands = {v_z, v_i_slot};
        st.source_line = source_line;
        emit(current_block_, std::move(st));
    }

    // limit8 = len - 7 (el qword corre mientras i < limit8, i.e. i+8 <= len).
    // Para len < 8 -> limit8 <= 0 -> el loop de palabra no entra (i=0 >= 0
    // no se cumple con CMP_LT signed) y todo se copia por la cola.
    ir::IrValueId v_seven = emit_const(ir::IrType::I64, 7, source_line);
    ir::IrValueId v_limit8 = fn_->new_value(ir::IrType::I64);
    {
        ir::IrInstr su{};
        su.op = ir::IrOp::SUB;
        su.type = ir::IrType::I64;
        su.dst = v_limit8;
        su.operands = {v_len, v_seven};
        su.source_line = source_line;
        emit(current_block_, std::move(su));
    }

    // ---- Loop 1: copia de palabra (8 bytes/iter). ----
    {
        const ir::IrBlockId hdr = fn_->new_block("wcopy_w_hdr");
        const ir::IrBlockId body = fn_->new_block("wcopy_w_body");
        const ir::IrBlockId done = fn_->new_block("wcopy_w_done");
        {
            ir::IrInstr br{};
            br.op = ir::IrOp::BR;
            br.target_block = hdr;
            br.source_line = source_line;
            emit(current_block_, std::move(br));
        }
        fn_->blocks[current_block_].succs.push_back(hdr);
        fn_->blocks[hdr].preds.push_back(current_block_);

        // hdr: i = load slot ; cond = i < limit8 ; br body, done
        current_block_ = hdr;
        ir::IrValueId v_i = fn_->new_value(ir::IrType::I64);
        {
            ir::IrInstr ld{};
            ld.op = ir::IrOp::LOAD;
            ld.type = ir::IrType::I64;
            ld.dst = v_i;
            ld.operands = {v_i_slot};
            ld.source_line = source_line;
            emit(current_block_, std::move(ld));
        }
        ir::IrValueId v_cond = fn_->new_value(ir::IrType::BOOL);
        {
            ir::IrInstr cmp{};
            cmp.op = ir::IrOp::CMP_LT; // signed; len/i no negativos
            cmp.type = ir::IrType::BOOL;
            cmp.dst = v_cond;
            cmp.operands = {v_i, v_limit8};
            cmp.source_line = source_line;
            emit(current_block_, std::move(cmp));
        }
        {
            ir::IrInstr brc{};
            brc.op = ir::IrOp::BR_COND;
            brc.operands = {v_cond};
            brc.target_block = body;
            brc.false_block = done;
            brc.source_line = source_line;
            emit(current_block_, std::move(brc));
        }
        fn_->blocks[hdr].succs.push_back(body);
        fn_->blocks[hdr].succs.push_back(done);
        fn_->blocks[body].preds.push_back(hdr);
        fn_->blocks[done].preds.push_back(hdr);

        // body: w = load.i64 src+i ; store.i64 w -> dst+i ; i += 8 ; -> hdr
        current_block_ = body;
        ir::IrValueId v_src = emit_ptr_add(src_base, v_i, source_line);
        ir::IrValueId v_dst = emit_ptr_add(dst_base, v_i, source_line);
        ir::IrValueId v_w = fn_->new_value(ir::IrType::I64);
        {
            ir::IrInstr ld{};
            ld.op = ir::IrOp::LOAD;
            ld.type = ir::IrType::I64;
            ld.dst = v_w;
            ld.operands = {v_src};
            ld.source_line = source_line;
            emit(current_block_, std::move(ld));
        }
        {
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = ir::IrType::I64;
            st.dst = ir::IR_NO_VALUE;
            st.operands = {v_w, v_dst};
            st.source_line = source_line;
            emit(current_block_, std::move(st));
        }
        ir::IrValueId v_i8 = fn_->new_value(ir::IrType::I64);
        {
            ir::IrValueId v_8 = emit_const(ir::IrType::I64, 8, source_line);
            ir::IrInstr ad{};
            ad.op = ir::IrOp::ADD;
            ad.type = ir::IrType::I64;
            ad.dst = v_i8;
            ad.operands = {v_i, v_8};
            ad.source_line = source_line;
            emit(current_block_, std::move(ad));
        }
        {
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = ir::IrType::I64;
            st.dst = ir::IR_NO_VALUE;
            st.operands = {v_i8, v_i_slot};
            st.source_line = source_line;
            emit(current_block_, std::move(st));
        }
        {
            ir::IrInstr br{};
            br.op = ir::IrOp::BR;
            br.target_block = hdr;
            br.source_line = source_line;
            emit(current_block_, std::move(br));
        }
        fn_->blocks[body].succs.push_back(hdr);
        fn_->blocks[hdr].preds.push_back(body);

        current_block_ = done;
        block_terminated_ = false;
    }

    // ---- Loop 2: cola byte-a-byte (mientras i < len). ----
    {
        const ir::IrBlockId hdr = fn_->new_block("wcopy_b_hdr");
        const ir::IrBlockId body = fn_->new_block("wcopy_b_body");
        const ir::IrBlockId done = fn_->new_block("wcopy_b_done");
        {
            ir::IrInstr br{};
            br.op = ir::IrOp::BR;
            br.target_block = hdr;
            br.source_line = source_line;
            emit(current_block_, std::move(br));
        }
        fn_->blocks[current_block_].succs.push_back(hdr);
        fn_->blocks[hdr].preds.push_back(current_block_);

        // hdr: i = load slot ; cond = i < len ; br body, done
        current_block_ = hdr;
        ir::IrValueId v_i = fn_->new_value(ir::IrType::I64);
        {
            ir::IrInstr ld{};
            ld.op = ir::IrOp::LOAD;
            ld.type = ir::IrType::I64;
            ld.dst = v_i;
            ld.operands = {v_i_slot};
            ld.source_line = source_line;
            emit(current_block_, std::move(ld));
        }
        ir::IrValueId v_cond = fn_->new_value(ir::IrType::BOOL);
        {
            ir::IrInstr cmp{};
            cmp.op = ir::IrOp::CMP_LT;
            cmp.type = ir::IrType::BOOL;
            cmp.dst = v_cond;
            cmp.operands = {v_i, v_len};
            cmp.source_line = source_line;
            emit(current_block_, std::move(cmp));
        }
        {
            ir::IrInstr brc{};
            brc.op = ir::IrOp::BR_COND;
            brc.operands = {v_cond};
            brc.target_block = body;
            brc.false_block = done;
            brc.source_line = source_line;
            emit(current_block_, std::move(brc));
        }
        fn_->blocks[hdr].succs.push_back(body);
        fn_->blocks[hdr].succs.push_back(done);
        fn_->blocks[body].preds.push_back(hdr);
        fn_->blocks[done].preds.push_back(hdr);

        // body: byte = load.u8 src+i ; store.u8 byte -> dst+i ; i += 1 ; -> hdr
        current_block_ = body;
        ir::IrValueId v_src = emit_ptr_add(src_base, v_i, source_line);
        ir::IrValueId v_dst = emit_ptr_add(dst_base, v_i, source_line);
        ir::IrValueId v_byte = fn_->new_value(ir::IrType::U8);
        {
            ir::IrInstr ld{};
            ld.op = ir::IrOp::LOAD;
            ld.type = ir::IrType::U8;
            ld.dst = v_byte;
            ld.operands = {v_src};
            ld.source_line = source_line;
            emit(current_block_, std::move(ld));
        }
        {
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = ir::IrType::U8;
            st.dst = ir::IR_NO_VALUE;
            st.operands = {v_byte, v_dst};
            st.source_line = source_line;
            emit(current_block_, std::move(st));
        }
        ir::IrValueId v_i1 = fn_->new_value(ir::IrType::I64);
        {
            ir::IrValueId v_1 = emit_const(ir::IrType::I64, 1, source_line);
            ir::IrInstr ad{};
            ad.op = ir::IrOp::ADD;
            ad.type = ir::IrType::I64;
            ad.dst = v_i1;
            ad.operands = {v_i, v_1};
            ad.source_line = source_line;
            emit(current_block_, std::move(ad));
        }
        {
            ir::IrInstr st{};
            st.op = ir::IrOp::STORE;
            st.type = ir::IrType::I64;
            st.dst = ir::IR_NO_VALUE;
            st.operands = {v_i1, v_i_slot};
            st.source_line = source_line;
            emit(current_block_, std::move(st));
        }
        {
            ir::IrInstr br{};
            br.op = ir::IrOp::BR;
            br.target_block = hdr;
            br.source_line = source_line;
            emit(current_block_, std::move(br));
        }
        fn_->blocks[body].succs.push_back(hdr);
        fn_->blocks[hdr].preds.push_back(body);

        current_block_ = done;
        block_terminated_ = false;
    }
}

/**
 * @brief Reserva un hueco de @p bytes en el marco actual y devuelve su
 *        direccion.
 *
 * Un hueco de pila no se libera: muere con el marco que lo creo.  Por eso es
 * lo correcto para lo que no debe sobrevivir a la funcion -- el par
 * {hay, valor} de un `Optional`, el par {puntero, borrador} de un puntero
 * inteligente -- y por eso envolver un valor no cuesta una reserva de memoria.
 *
 * El @p host_memory no es un detalle, y por eso se llama por lo que HACE y no
 * por quien lo pedia: pone el hueco en la memoria del ANFITRION en vez de en
 * la de la maquina virtual.  Un `Optional` que se devuelve viaja por la
 * direccion que dio el llamante, y esa direccion es del anfitrion: reservarlo
 * en la otra memoria hace que el llamante lea en el sitio equivocado.  En un
 * binario nativo pasa lo mismo con TODO hueco, no solo con esos, que es la
 * razon de que el parametro no pudiera seguir llamandose por su primer caso.
 *
 * Era una lambda dentro de la bajada de los builtins, asi que de ahi no salia
 * aunque la necesitaran otras familias.  No capturaba nada -- solo usa el
 * estado del propio bajador --, de modo que pasar a metodo no cambio ninguna
 * llamada.
 *
 * @param bytes      Cuanto reservar.
 * @param line       Linea fuente, para la depuracion.
 * @param host_memory Si el hueco va en la memoria del anfitrion.
 * @return El valor SSA con la direccion del hueco.
 */
ir::IrValueId Lowering::stack_alloc_buf(uint64_t bytes, uint32_t line,
                                        bool host_memory) {
    const ir::IrValueId v_buf = fn_->new_value(ir::IrType::PTR);
    ir::IrInstr al{};
    al.op = ir::IrOp::ALLOCA;
    al.type = ir::IrType::I8;
    al.imm = bytes;
    al.dst = v_buf;
    al.source_line = line;
    al.host_alloca = host_memory;
    emit(current_block_, std::move(al));
    if (host_memory) fn_->values[v_buf].is_host_ptr = true;
    return v_buf;
}

/**
 * @brief Reserva el hueco donde vive un puntero inteligente, en la pila o en
 *        el monton segun a donde vaya a parar.
 *
 * Un `unique<T>` es un par {puntero, quien lo borra}, y donde vive ese par
 * depende de a quien pertenece.  Si es una variable local, la pila vale: muere
 * con la funcion, que es justo cuando toca soltarlo.  Pero si se guarda en un
 * CAMPO de un objeto, tiene que sobrevivir a la funcion que lo creo -- el
 * objeto sigue vivo --, asi que va al monton y lo suelta el destructor del que
 * lo contiene.
 *
 * Quien decide es la marca @c unique_slot_to_heap_, que pone el que esta
 * bajando la asignacion a un campo.  Se CONSUME al leerla: si no, un
 * `unique<T>` anidado o el siguiente de la misma funcion heredarian una
 * decision que no era suya y acabarian en el monton sin que nadie los suelte.
 *
 * Era una lambda dentro de la bajada de los builtins y de ahi no salia.  No
 * capturaba nada -- solo usa el estado del propio bajador --, asi que pasar a
 * metodo no cambio ninguna llamada.
 *
 * @param line Linea fuente, para la depuracion.
 * @return El valor SSA con la direccion del hueco.
 */
ir::IrValueId Lowering::unique_slot_buf(uint32_t line) {
    if (!unique_slot_to_heap_) return stack_alloc_buf(16, line);
    unique_slot_to_heap_ = false;
    const ir::IrValueId v_buf = fn_->new_value(ir::IrType::PTR);
    fn_->values[v_buf].is_host_ptr = true;
    const ir::IrValueId v_size = emit_const(ir::IrType::I64, 16, line);
    ir::IrInstr al{};
    al.op = ir::IrOp::RAW_ALLOC;
    al.type = ir::IrType::PTR;
    al.dst = v_buf;
    al.operands = {v_size};
    al.source_line = line;
    emit(current_block_, std::move(al));
    return v_buf;
}

/**
 * @brief Suma un desplazamiento a una direccion y devuelve la de destino.
 *
 * Estaba escrito DOCE veces, como una lambda dentro de la funcion que lo
 * necesitaba, en dos formas que no coincidian: nueve fijaban el resultado como
 * direccion del anfitrion y tres lo HEREDABAN de la base.  Heredarlo es lo
 * correcto siempre: una direccion mas ocho sigue apuntando a la misma memoria,
 * y decir que es del anfitrion cuando la base es de la maquina virtual hace que
 * quien la lea despues emita el acceso equivocado -- no da error, lee otra
 * cosa --.
 *
 * El atajo de desplazamiento cero tampoco estaba en todas: sumar cero emite una
 * instruccion que no hace nada, y aqui se evita en todas por igual.
 *
 * @param base        La direccion de partida.
 * @param off         Cuanto sumarle.
 * @param source_line Linea fuente, para la depuracion.
 * @return El valor SSA con la direccion resultante.
 */
ir::IrValueId Lowering::emit_ptr_add(ir::IrValueId base, ir::IrValueId off,
                                     uint32_t source_line) {
    const ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
    fn_->values[v].is_host_ptr = fn_->values[base].is_host_ptr;
    ir::IrInstr ad{};
    ad.op = ir::IrOp::ADD;
    ad.type = ir::IrType::I64;
    ad.dst = v;
    ad.operands = {base, off};
    ad.source_line = source_line;
    emit(current_block_, std::move(ad));
    return v;
}

/**
 * @brief Igual, con el desplazamiento conocido al compilar.
 *
 * Sumar cero devuelve la propia base sin emitir nada: no es una optimizacion
 * suelta sino lo que hace que quien recorre una estructura pueda pedir el campo
 * en el desplazamiento 0 igual que los demas, sin tratarlo aparte.
 *
 * @param base        La direccion de partida.
 * @param off         Cuanto sumarle, conocido al compilar.
 * @param source_line Linea fuente, para la depuracion.
 * @return El valor SSA con la direccion resultante, o @p base si @p off es 0.
 */
ir::IrValueId Lowering::emit_ptr_add(ir::IrValueId base, uint64_t off,
                                     uint32_t source_line) {
    if (off == 0) return base;
    return emit_ptr_add(base, emit_const(ir::IrType::I64, off, source_line),
                        source_line);
}

/**
 * @brief Lee un qword de una direccion.
 *
 * Ocho bytes es el ancho con el que se leen los campos internos de una cadena
 * -- su longitud, su capacidad, el puntero a sus bytes -- y de ahi que este
 * caso concreto valga la pena aparte: estaba escrito cuatro veces, una por
 * funcion que monta uno de esos helpers a mano.
 *
 * @param addr        De donde leer.
 * @param source_line Linea fuente, para la depuracion.
 * @return El valor SSA leido.
 */
ir::IrValueId Lowering::emit_load_i64(ir::IrValueId addr,
                                      uint32_t source_line) {
    const ir::IrValueId v = fn_->new_value(ir::IrType::I64);
    ir::IrInstr ld{};
    ld.op = ir::IrOp::LOAD;
    ld.type = ir::IrType::I64;
    ld.dst = v;
    ld.operands = {addr};
    ld.source_line = source_line;
    emit(current_block_, std::move(ld));
    return v;
}

/**
 * @brief Escribe un valor del ancho que se pida en una direccion.
 *
 * El orden de los operandos NO es el de la firma: la instruccion los quiere
 * `{valor, direccion}` y aqui se reciben al reves, que es como se lee en el
 * fuente ("escribe en esta direccion este valor").  Invertirlos escribe la
 * direccion como si fuera el dato.
 *
 * El ancho importa y no se deduce del valor: escribir un entero de ocho bytes
 * donde caben dos pisa lo que hay detras, y escribir dos donde van ocho deja la
 * mitad de antes.
 *
 * @param addr        Donde escribir.
 * @param val         Que escribir.
 * @param ty          De que ancho.
 * @param source_line Linea fuente, para la depuracion.
 */
void Lowering::emit_store_typed(ir::IrValueId addr, ir::IrValueId val,
                               ir::IrType ty, uint32_t source_line) {
    ir::IrInstr st{};
    st.op = ir::IrOp::STORE;
    st.type = ty;
    st.dst = ir::IR_NO_VALUE;
    st.operands = {val, addr};
    st.source_line = source_line;
    emit(current_block_, std::move(st));
}

/**
 * @copydoc vx::Lowering::emit_store_i64
 */
void Lowering::emit_store_i64(ir::IrValueId addr, ir::IrValueId val,
                              uint32_t source_line) {
    emit_store_typed(addr, val, ir::IrType::I64, source_line);
}

/**
 * @brief Emite una operacion de dos operandos y devuelve su resultado.
 *
 * Estaba escrito seis veces en dos formas, y la diferencia entre ellas no era
 * de estilo: tres pasaban el tipo del resultado y NO ponian linea fuente -- esas
 * instrucciones salian sin linea, y la depuracion las perdia --, y las otras
 * tres fijaban el tipo a entero sin signo y si la ponian.  Aqui van las dos
 * cosas, porque las dos hacen falta.
 *
 * @param op          Que operacion.
 * @param a           Primer operando.
 * @param b           Segundo operando.
 * @param t           Tipo del resultado.
 * @param source_line Linea fuente, para la depuracion.
 * @return El valor SSA con el resultado.
 */
ir::IrValueId Lowering::emit_ir_binop(ir::IrOp op, ir::IrValueId a,
                                      ir::IrValueId b, ir::IrType t,
                                      uint32_t source_line) {
    const ir::IrValueId d = fn_->new_value(t);
    ir::IrInstr in{};
    in.op = op;
    in.type = t;
    in.dst = d;
    in.operands = {a, b};
    in.source_line = source_line;
    emit(current_block_, std::move(in));
    return d;
}

/**
 * @brief Copia @p len bytes, eligiendo COMO segun a donde se compile.
 *
 * Hay dos maneras y no son intercambiables.  En la maquina virtual y en el JIT
 * la copia es una instruccion suya: el motor la hace y sabe hacerla bien.  En
 * un binario nativo no hay motor detras, asi que se llama a una copia escrita
 * en Vesta -- y no a una cualquiera: a la que el procesador de esa maquina
 * pueda ejecutar mas rapido, elegida al arrancar y guardada en una tabla --.
 *
 * Elegir mal no da error: en nativo, emitir la instruccion deja una copia que
 * nadie implementa.  Por eso la decision esta aqui y no en cada llamante, que
 * es donde estaba -- tres veces, con el mismo comentario copiado --.
 *
 * @param dst         Destino.
 * @param src         Origen.
 * @param len         Cuantos bytes.
 * @param source_line Linea fuente, para la depuracion.
 */
void Lowering::emit_memcpy(ir::IrValueId dst, ir::IrValueId src,
                           ir::IrValueId len, uint32_t source_line) {
    if (native_poo_) {
        emit_memcpy_dispatched(dst, src, len, source_line);
        return;
    }
    ir::IrInstr mc{};
    mc.op = ir::IrOp::MEMCPY;
    mc.type = ir::IrType::I8;
    mc.dst = ir::IR_NO_VALUE;
    mc.operands = {dst, src, len};
    mc.source_line = source_line;
    emit(current_block_, std::move(mc));
}

/**
 * @brief La direccion de un dato que ya vive en el ejecutable.
 *
 * Los textos, las tablas y todo lo que se conoce al compilar no se construyen
 * en marcha: se guardan en el propio ejecutable y lo que hace falta en tiempo
 * de ejecucion es su DIRECCION.  Eso es una sola instruccion, y el numero que
 * recibe (@p idx) es el sitio que le dio `intern_static_data`.
 *
 * El @p host_ptr dice si esa direccion es de la memoria del anfitrion.  Depende
 * de a donde se compile, no del dato, y marcarlo mal no da error: hace que
 * quien lo lea despues emita el acceso contra la otra memoria.
 *
 * Estaba escrito cuarenta y cuatro veces, en veinte unidades distintas, con
 * ocho lineas cada vez.
 *
 * @param idx         El sitio del dato, el que devolvio `intern_static_data`.
 * @param source_line Linea fuente, para la depuracion.
 * @param host_ptr    Si la direccion es de la memoria del anfitrion.
 * @return El valor SSA con la direccion.
 */
ir::IrValueId Lowering::emit_str_lit_addr(uint64_t idx, uint32_t source_line,
                                          bool host_ptr) {
    const ir::IrValueId v = fn_->new_value(ir::IrType::PTR);
    if (host_ptr) fn_->values[v].is_host_ptr = true;
    ir::IrInstr la{};
    la.op = ir::IrOp::STR_LIT_ADDR;
    la.type = ir::IrType::PTR;
    la.dst = v;
    la.imm = idx;
    la.source_line = source_line;
    emit(current_block_, std::move(la));
    return v;
}

} // namespace vx
