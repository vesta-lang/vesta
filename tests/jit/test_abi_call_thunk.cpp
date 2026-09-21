/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/jit/test_abi_call_thunk.cpp
 * @brief Que una convencion DECLARADA se cumpla de verdad al llamar.
 *
 * El interprete llamaba a codigo nativo por una llamada C corriente, asi que
 * de una convencion declarada cumplia la mitad: los argumentos que sobran de
 * los registros salian a la pila en su sitio -- eso lo hace el compilador
 * anfitrion -- pero los registros FIJOS no, porque en una llamada C el primer
 * argumento va donde diga la plataforma y no donde lo pidio la declaracion.
 *
 * Media convencion no da un error: da otro resultado.  Por eso los casos de
 * aqui no miran "no revento", miran QUE REGISTRO llego con QUE valor -- y para
 * eso el destino es codigo ensamblado que lee un registro concreto, no una
 * funcion de C, que no puede afirmar nada sobre donde entro su argumento --.
 */
#include "jit/abi_call_thunk.h"
#include "jit/code_cache.h"
#include "jit/keystone_asm_backend.h"
#include "vx/asm/asm_backend.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_fail = 0;
int g_checks = 0;

void check(bool cond, const std::string &what) {
    ++g_checks;
    if (!cond) {
        ++g_fail;
        std::printf("  FAIL: %s\n", what.c_str());
    }
}

/// Ensambla @p src y lo deja ejecutable.  El destino de cada caso: unas pocas
/// instrucciones que DELATAN de donde leyeron su valor.
uint64_t make_target(const char *src, jit::CodeCache &cc) {
    vx::AsmAssembleResult ar =
        vx::g_asm_backend->assemble(src, vx::AsmArch::X86_64);
    if (!ar.ok || ar.bytes.empty()) return 0;
    uint8_t *code = cc.alloc(ar.bytes.size(), 16);
    if (code == nullptr) return 0;
    std::memcpy(code, ar.bytes.data(), ar.bytes.size());
    cc.commit(code, ar.bytes.size());
    return reinterpret_cast<uint64_t>(code);
}

/// Un argumento fijado a un registro llega A ESE registro, y no al que la
/// plataforma habria elegido.  Es lo que el puente de antes no hacia.
void case_pinned_register(jit::CodeCache &cc) {
    /* Devuelve lo que haya en r10.  En la ABI de la plataforma r10 NO es un
     * registro de argumento, asi que si llega el valor es porque la
     * convencion declarada se cumplio. */
    const uint64_t target = make_target("mov rax, r10\nret\n", cc);
    check(target != 0, "the target assembles");
    if (target == 0) return;

    std::string err;
    jit::AbiCallThunkFn thunk = jit::abi_call_thunk_for(0, &err);
    check(thunk != nullptr, "the thunk is generated: " + err);
    if (thunk == nullptr) return;

    jit::AbiCallCtx ctx;
    ctx.target = target;
    ctx.gp[10] = 0xC0FFEEULL; // r10
    check(thunk(&ctx) == 0xC0FFEEULL,
          "an argument pinned to r10 arrives in r10");
}

/// Y varios a la vez, incluido uno que NO es registro de argumento en ninguna
/// plataforma (rax): es el caso del numero de servicio de una syscall.
void case_several_pins(jit::CodeCache &cc) {
    /* rax + r10 + rdx: si cualquiera de los tres se hubiera colocado por la
     * regla de la plataforma en vez de por la declarada, la suma no cuadra. */
    const uint64_t target =
        make_target("add rax, r10\nadd rax, rdx\nret\n", cc);
    check(target != 0, "the target assembles");
    if (target == 0) return;

    std::string err;
    jit::AbiCallThunkFn thunk = jit::abi_call_thunk_for(0, &err);
    if (thunk == nullptr) return;

    jit::AbiCallCtx ctx;
    ctx.target = target;
    ctx.gp[0] = 1000; // rax
    ctx.gp[10] = 200; // r10
    ctx.gp[2] = 30;   // rdx
    check(thunk(&ctx) == 1230, "three pinned registers all arrive");
}

/// Lo que no cabe en registro va por la pila, al desplazamiento que la
/// plataforma pide.  El destino lo lee de ahi a mano.
void case_stack_arguments(jit::CodeCache &cc) {
#if defined(_WIN32)
    /* Windows: tras el `call`, el primero de pila esta en [rsp+0x28] -- 8 de
     * la direccion de retorno mas 0x20 de sombra --. */
    const char *src = "mov rax, [rsp + 0x28]\nret\n";
    const char *src2 = "mov rax, [rsp + 0x30]\nret\n";
#else
    /* System V no tiene sombra: el primero queda justo tras la direccion de
     * retorno. */
    const char *src = "mov rax, [rsp + 0x8]\nret\n";
    const char *src2 = "mov rax, [rsp + 0x10]\nret\n";
#endif
    std::string err;
    jit::AbiCallThunkFn thunk = jit::abi_call_thunk_for(2, &err);
    check(thunk != nullptr, "the thunk with stack arguments is generated");
    if (thunk == nullptr) return;

    const uint64_t t1 = make_target(src, cc);
    const uint64_t t2 = make_target(src2, cc);
    if (t1 == 0 || t2 == 0) return;

    jit::AbiCallCtx ctx;
    ctx.stack[0] = 0x1111222233334444ULL;
    ctx.stack[1] = 0x5555666677778888ULL;

    ctx.target = t1;
    check(thunk(&ctx) == 0x1111222233334444ULL,
          "the first stack argument lands where the platform says");
    ctx.target = t2;
    check(thunk(&ctx) == 0x5555666677778888ULL, "and the second right after");
}

/// Los dos a la vez, que es lo que de verdad pide una convencion como la de
/// NT: cuatro en registro -- uno de ellos fuera de la ABI de la plataforma --
/// y el resto por la pila.
void case_pins_and_stack_together(jit::CodeCache &cc) {
#if defined(_WIN32)
    const char *src = "mov rax, r10\nadd rax, [rsp + 0x28]\nret\n";
#else
    const char *src = "mov rax, r10\nadd rax, [rsp + 0x8]\nret\n";
#endif
    const uint64_t target = make_target(src, cc);
    if (target == 0) return;

    std::string err;
    jit::AbiCallThunkFn thunk = jit::abi_call_thunk_for(1, &err);
    if (thunk == nullptr) return;

    jit::AbiCallCtx ctx;
    ctx.target = target;
    ctx.gp[10] = 7000;
    ctx.stack[0] = 42;
    check(thunk(&ctx) == 7042,
          "registers and stack are honoured in the same call");
}

/// Los preservados vuelven como estaban.  Si el thunk se los comiera, lo que
/// reventaria seria el llamante, lejos de aqui y sin relacion aparente.
void case_callee_saved_survive(jit::CodeCache &cc) {
    /* El destino los PISA a proposito. */
    const uint64_t target = make_target(
        "mov rbx, 0\nmov rbp, 0\nmov r12, 0\nmov r13, 0\n"
        "mov r14, 0\nmov r15, 0\nmov rsi, 0\nmov rdi, 0\nmov rax, 5\nret\n",
        cc);
    if (target == 0) return;

    std::string err;
    jit::AbiCallThunkFn thunk = jit::abi_call_thunk_for(0, &err);
    if (thunk == nullptr) return;

    jit::AbiCallCtx ctx;
    ctx.target = target;
    volatile uint64_t before = 0xABCDEF;
    uint64_t r = thunk(&ctx);
    check(r == 5, "the result comes back");
    check(before == 0xABCDEF, "the caller's own state is intact");
}

/// Pedir mas de los que caben se DICE, no se recorta en silencio.
void case_too_many_is_refused() {
    std::string err;
    jit::AbiCallThunkFn thunk =
        jit::abi_call_thunk_for(jit::kAbiCallMaxArgs + 1, &err);
    check(thunk == nullptr, "too many stack arguments is refused");
    check(!err.empty(), "and it says why");
}

} // namespace

int main() {
    std::printf("== a declared calling convention is honoured ==\n");
    jit::register_keystone_asm_backend();

    jit::CodeCache cc;
    case_pinned_register(cc);
    case_several_pins(cc);
    case_stack_arguments(cc);
    case_pins_and_stack_together(cc);
    case_callee_saved_survive(cc);
    case_too_many_is_refused();

    std::printf("%d of %d OK\n", g_checks - g_fail, g_checks);
    return g_fail == 0 ? 0 : 1;
}
