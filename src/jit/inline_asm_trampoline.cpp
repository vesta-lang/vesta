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

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace jit {

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
