/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/inline_asm_trampoline.cpp
 * @brief Implementacion del trampoline de inline-asm para el interprete
 *        (Phase AS inc.6).  Ver inline_asm_trampoline.h.
 */

#include "jit/inline_asm_trampoline.h"

#include "jit/code_cache.h"
#include "vex/asm_backend.h"
#include "ffi/virtual_lib_registry.h"     // inc.6: registrar el helper runner
#include "runtime/proceso_runtime.h"      // inc.6: acceso a ProcessVM::asm_ctx + vm_mem
#include "ir/ssa_ir.h"                     // inc.6: IrFunction/IrInstr/IrOp del batch

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace jit {

    // =====================================================================
    //  Phase AS inc.6: registro global hash(NASM) -> trampoline + helper
    //  nativo que el interprete invoca por cada bloque inline-asm.
    // =====================================================================

    namespace {
        std::unordered_map<uint64_t, AsmTrampolineFn> &asm_tramp_map() {
            static std::unordered_map<uint64_t, AsmTrampolineFn> m;
            return m;
        }
        std::mutex &asm_tramp_mtx() {
            static std::mutex mtx;
            return mtx;
        }
    } // namespace

    void register_inline_asm_trampoline(uint64_t hash, AsmTrampolineFn fn) {
        if (fn == nullptr) return;
        std::lock_guard<std::mutex> lk(asm_tramp_mtx());
        // idempotente: cuerpos asm identicos comparten trampoline.
        asm_tramp_map().emplace(hash, fn);
    }

    AsmTrampolineFn lookup_inline_asm_trampoline(uint64_t hash) {
        std::lock_guard<std::mutex> lk(asm_tramp_mtx());
        auto it = asm_tramp_map().find(hash);
        return (it == asm_tramp_map().end()) ? nullptr : it->second;
    }

    /* Helper nativo invocado por el bytecode (calln @Method("vrt:inline_asm_exec")).
     * Convencion (argc SIEMPRE 12, slots no usados = 0):
     *   arg0  proc  = ProcessVM* (via getproc)
     *   arg1  hash  = FNV-1a del NASM (clave del trampoline)
     *   arg2  desc  = phys-id (4 bits) por binding, hasta 8 bindings
     *   arg3  n     = numero de bindings (<=8)
     *   arg4..11    = direcciones VM de los slots (alloca) de cada binding
     *
     * Marshalling: rellena @c vm->asm_ctx[phys] desde el slot VM (in), llama al
     * trampoline (que carga los 16 GP host desde ctx, corre el asm, y los
     * guarda de vuelta a ctx), y escribe @c ctx[phys] al slot VM (out).  Cada
     * binding es in+out (conservador, siempre correcto). */
    extern "C" uint64_t vrt_inline_asm_exec(
        uint64_t proc, uint64_t hash, uint64_t desc, uint64_t n_u,
        uint64_t s0, uint64_t s1, uint64_t s2, uint64_t s3,
        uint64_t s4, uint64_t s5, uint64_t s6, uint64_t s7) {
        auto *vm = reinterpret_cast<runtime::ProcessVM *>(proc);
        if (vm == nullptr) return 0;

        AsmTrampolineFn tramp = lookup_inline_asm_trampoline(hash);
        if (tramp == nullptr) {
            std::fprintf(stderr,
                "[asm] vrt_inline_asm_exec: trampoline no registrado "
                "(hash=0x%016llx) -- inline-asm no se ejecutara\n",
                static_cast<unsigned long long>(hash));
            return 0;
        }

        const int      n     = static_cast<int>(n_u);
        const uint64_t slots[8] = {s0, s1, s2, s3, s4, s5, s6, s7};
        uint64_t      *ctx   = vm->asm_ctx;          // host scratch del proceso

        for (int i = 0; i < 16; ++i) ctx[i] = 0;
        // in-marshalling: slot VM -> ctx[phys]
        for (int i = 0; i < n && i < 8; ++i) {
            const int phys = static_cast<int>((desc >> (i * 4)) & 0xF);
            ctx[phys] = vm->vm_mem.read_u64(slots[i]);
        }
        tramp(ctx);                                  // ejecuta el asm host
        // out-marshalling: ctx[phys] -> slot VM
        for (int i = 0; i < n && i < 8; ++i) {
            const int phys = static_cast<int>((desc >> (i * 4)) & 0xF);
            vm->vm_mem.write_u64(slots[i], ctx[phys]);
        }
        return 0;
    }

    void register_inline_asm_runner() {
        ffi::register_virtual_fn("vrt", "inline_asm_exec",
                                 reinterpret_cast<void *>(&vrt_inline_asm_exec));
    }

    void build_and_register_inline_asm_trampolines(
        const std::vector<ir::IrFunction> &fns) {
        if (vex::g_asm_backend == nullptr) return;   // sin ensamblador -> no-op
        // CodeCache de vida-proceso: los trampolines deben sobrevivir mientras
        // el programa corra.  Una sola instancia compartida por todos los
        // bloques asm (de cualquier modulo cargado).
        static CodeCache asm_cc;
        for (const auto &fn : fns) {
            for (const auto &blk : fn.blocks) {
                for (const auto &ins : blk.instrs) {
                    if (ins.op != ir::IrOp::INLINE_ASM) continue;
                    const uint64_t hash = fnv1a64_asm(ins.func_name);
                    if (lookup_inline_asm_trampoline(hash) != nullptr) continue;
                    std::string err;
                    AsmTrampolineFn tp =
                        build_asm_trampoline(ins.func_name, asm_cc, &err);
                    if (tp == nullptr) {
                        std::fprintf(stderr,
                            "[asm] no se pudo ensamblar el trampoline de "
                            "inline-asm en '%s': %s\n",
                            fn.name.c_str(), err.c_str());
                        continue;
                    }
                    register_inline_asm_trampoline(hash, tp);
                }
            }
        }
    }

    /* ABI del trampoline: @c void tramp(uint64_t ctx[16]).  El primer argumento
     * (ctx) llega en rcx (Win64) o rdi (SysV).  Layout del prologo/epilogo
     * (documentado en el header):
     *
     *   push rbx/rbp/rsi/rdi/r12..r15      ; salvar callee-saved (los pisamos)
     *   push <ctx>                          ; [rsp] = ctx (sobrevive el asm)
     *   <load 16 GP host desde [ctx + id*8]; cargar <ctx> el ULTIMO>
     *   <USER ASM>                          ; rsp 16-alineado aqui
     *   push rax                            ; salvar rax (resultado) temporal
     *   mov rax, [rsp+8]                    ; recargar ctx (estaba en [rsp+8])
     *   <save 15 GP host (todos menos rax) a [ctx + id*8]>
     *   pop rcx ; mov [rax+0], rcx          ; salvar rax (id 0)
     *   add rsp, 8                          ; descartar el slot de ctx
     *   pop r15..rbx                        ; restaurar callee-saved
     *   ret
     *
     * Generico: el mismo prologo/epilogo sirve para CUALQUIER asm + bindings
     * (carga/salva los 16; el caller usa solo los ids que le interesan).  El
     * ctx en pila hace el trampoline reentrante y absorbe clobbers de rbx/rbp.
     */
    AsmTrampolineFn build_asm_trampoline(const std::string &user_asm,
                                         CodeCache &cc, std::string *err) {
        if (vex::g_asm_backend == nullptr) {
            if (err) *err = "no hay backend de ensamblado (Keystone) registrado";
            return nullptr;
        }

#if defined(_WIN32)
        const char *CTX = "rcx";   // Win64: primer arg en rcx
#else
        const char *CTX = "rdi";   // SysV: primer arg en rdi
#endif

        std::string nasm;
        nasm.reserve(user_asm.size() + 1024);

        /* --- prologo: salvar callee-saved --- */
        nasm += "push rbx\npush rbp\npush rsi\npush rdi\n";
        nasm += "push r12\npush r13\npush r14\npush r15\n";
        /* salvar ctx en la pila (sobrevive cualquier clobber del asm) */
        nasm += "push "; nasm += CTX; nasm += "\n";

        /* --- cargar los 16 GP host desde ctx ([ctx + id*8]); rsp (id 4) NO se
         * carga (no se pisa el stack pointer).  El registro que TRAE ctx se
         * carga el ULTIMO (ya no se necesita ctx tras eso). --- */
        static const struct { const char *r; int id; } REGS[] = {
            {"rax",0},{"rcx",1},{"rdx",2},{"rbx",3},{"rbp",5},{"rsi",6},
            {"rdi",7},{"r8",8},{"r9",9},{"r10",10},{"r11",11},
            {"r12",12},{"r13",13},{"r14",14},{"r15",15},
        };
        char line[64];
        /* OJO: Keystone (NASM) interpreta los enteros BARE como HEX -> emitir
         * los offsets como 0x%x (el valor en hex) para que el disp sea correcto. */
        for (const auto &e : REGS) {
            if (std::strcmp(e.r, CTX) == 0) continue;  // ctx-reg: el ultimo
            std::snprintf(line, sizeof(line), "mov %s, [%s + 0x%x]\n",
                          e.r, CTX, e.id * 8);
            nasm += line;
        }
        /* el reg que trae ctx: cargarlo ahora (su valor logico esta en ctx) */
        {
            int ctx_id = (std::strcmp(CTX, "rcx") == 0) ? 1 : 7;  // rcx=1, rdi=7
            std::snprintf(line, sizeof(line), "mov %s, [%s + 0x%x]\n",
                          CTX, CTX, ctx_id * 8);
            nasm += line;
        }

        /* --- cuerpo del usuario --- */
        nasm += user_asm;
        if (!user_asm.empty() && user_asm.back() != '\n') nasm += "\n";

        /* --- epilogo: salvar los 16 GP host de vuelta a ctx --- */
        nasm += "push rax\n";          // salvar rax (resultado) temporal
        nasm += "mov rax, [rsp + 8]\n"; // recargar ctx (rax = ctx)
        for (const auto &e : REGS) {
            if (e.id == 0) continue;    // rax se salva al final (via pop)
            std::snprintf(line, sizeof(line), "mov [rax + 0x%x], %s\n",
                          e.id * 8, e.r);
            nasm += line;
        }
        nasm += "pop rcx\n";            // rcx = rax salvado
        nasm += "mov [rax + 0], rcx\n"; // salvar rax (id 0)
        nasm += "add rsp, 8\n";         // descartar el slot de ctx
        nasm += "pop r15\npop r14\npop r13\npop r12\n";
        nasm += "pop rdi\npop rsi\npop rbp\npop rbx\n";
        nasm += "ret\n";

        /* Diagnostico opt-in: volcar el NASM generado. */
        if (std::getenv("VESTA_ASM_TRAMP_DEBUG")) {
            std::fprintf(stderr, "=== trampoline NASM ===\n%s=======================\n",
                         nasm.c_str());
        }

        /* --- ensamblar (Keystone) --- */
        vex::AsmAssembleResult ar =
            vex::g_asm_backend->assemble(nasm, vex::AsmArch::X86_64);
        if (!ar.ok || ar.bytes.empty()) {
            if (err) *err = ar.ok ? "ensamblado vacio" : ar.error;
            return nullptr;
        }
        if (std::getenv("VESTA_ASM_TRAMP_DEBUG")) {
            std::fprintf(stderr, "=== bytes (%zu) ===\n", ar.bytes.size());
            for (size_t i = 0; i < ar.bytes.size(); ++i)
                std::fprintf(stderr, "%02x ", ar.bytes[i]);
            std::fprintf(stderr, "\n");
        }

        /* --- alojar ejecutable --- */
        uint8_t *code = cc.alloc(ar.bytes.size(), 16);
        if (!code) {
            if (err) *err = "code cache OOM";
            return nullptr;
        }
        std::memcpy(code, ar.bytes.data(), ar.bytes.size());
        cc.commit(code, ar.bytes.size());
        return reinterpret_cast<AsmTrampolineFn>(code);
    }

} // namespace jit
