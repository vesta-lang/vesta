/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file jit/inline_asm_trampoline.cpp
 * @brief Implementacion del trampoline de inline-asm para el interprete
 *        ( AS inc.6).  Ver inline_asm_trampoline.h.
 */

#include "jit/inline_asm_trampoline.h"

#include "jit/code_cache.h"
#include "vx/asm/asm_backend.h"
#include "ffi/virtual_lib_registry.h" // inc.6: registrar el helper runner
#include "runtime/proceso_runtime.h" // inc.6: acceso a ProcessVM::asm_ctx + vm_mem

#include <atomic> // emulacion portable del efecto (barrera) sin ensamblador
#include "ir/ssa_ir.h"               // inc.6: IrFunction/IrInstr/IrOp del batch
#include "vx/asm/asm_phys_reg.h"     // sustitucion $N -> reg fisico

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#if defined(_WIN32)
#include <windows.h> // RtlAddFunctionTable: desenrollado del codigo generado
#endif

namespace jit {

#if defined(_WIN32)
namespace {

/**
 * @brief Describe el prologo del trampoline para que el sistema sepa
 *        desenrollarlo.
 *
 * En Windows de 64 bits el desenrollado NO se hace siguiendo punteros de marco:
 * se hace por TABLAS.  Para cada trozo de codigo, el sistema busca una entrada
 * que diga cuanto ocupa su prologo y que guardo en la pila; si no la encuentra,
 * da el trozo por hoja y supone que en la cima de la pila esta la direccion de
 * retorno.
 *
 * El codigo que genera el compilador en caliente no esta en ninguna tabla,
 * porque las tablas las trae el ejecutable.  Mientras nadie tenga que
 * desenrollar por encima de el, da igual.  Pero un fallo del procesador DENTRO
 * de un bloque de ensamblador se recoge con un salto largo, y un salto largo en
 * Windows ES un desenrollado: el sistema se ponia a caminar la pila, llegaba al
 * trampoline, no encontraba entrada, lo daba por hoja, leia como direccion de
 * retorno lo que hubiera en la cima -- que en mitad del prologo no lo es -- y se
 * llevaba el proceso por delante.  Como lo que hubiera en la cima depende de
 * como quedo la pila, el mismo programa moria o no segun donde estuviera el
 * binario: se reproducia cambiando la LONGITUD DE LA RUTA del ejecutable.
 *
 * Aqui se registra esa entrada.  El prologo son nueve `push` de tamano fijo, asi
 * que se describe una vez y vale para todos los trampolines.
 *
 * La estructura vive en el propio cache de codigo (vida del proceso) porque el
 * sistema guarda el PUNTERO, no una copia.
 */
struct DesenrolladoTrampoline {
    RUNTIME_FUNCTION funcion;
    /* UNWIND_INFO cabecera (4 bytes) + 9 codigos de 2 bytes.  Se escribe a mano
     * porque la definicion de winnt.h lleva un array flexible. */
    uint8_t info[4 + 9 * 2];
};

/// Codigos de operacion del desenrollado (winnt.h no los expone como enum).
enum : uint8_t { UWOP_PUSH_NONVOL = 0, UWOP_ALLOC_SMALL = 2 };

/**
 * @brief Rellena la descripcion del prologo y la registra en el sistema.
 *
 * @param code   Principio del codigo del trampoline (ya copiado y comiteado).
 * @param n      Tamano del codigo en bytes.
 * @param cc     Cache donde alojar la descripcion (misma vida que el codigo).
 * @return true si quedo registrada.
 */
bool registrar_desenrollado(uint8_t *code, size_t n, CodeCache &cc) {
    /* El prologo que emite `build_asm_trampoline`, byte a byte.  Se COMPRUEBA
     * en vez de darlo por hecho: si algun dia cambia, mentirle al desenrollador
     * es peor que no decirle nada, asi que ante la duda no se registra. */
    static const uint8_t kPrologo[] = {
        0x53,       // push rbx
        0x55,       // push rbp
        0x56,       // push rsi
        0x57,       // push rdi
        0x41, 0x54, // push r12
        0x41, 0x55, // push r13
        0x41, 0x56, // push r14
        0x41, 0x57, // push r15
        0x51,       // push rcx  (el que trae ctx en Win64)
    };
    const size_t kPrologoLen = sizeof(kPrologo);
    if (n < kPrologoLen || std::memcmp(code, kPrologo, kPrologoLen) != 0)
        return false;

    auto *d = reinterpret_cast<DesenrolladoTrampoline *>(
        cc.alloc(sizeof(DesenrolladoTrampoline), 16));
    if (d == nullptr) return false;
    std::memset(d, 0, sizeof(*d));

    /* Los codigos van en orden DESCENDENTE de posicion dentro del prologo: el
     * desenrollador los aplica de atras hacia delante.  La posicion de cada uno
     * es la del byte SIGUIENTE a su instruccion. */
    struct Paso { uint8_t off, op, info; };
    static const Paso kPasos[] = {
        {13, UWOP_ALLOC_SMALL, 0},  // push rcx -> solo devuelve 8 bytes de pila
        {12, UWOP_PUSH_NONVOL, 15}, // push r15
        {10, UWOP_PUSH_NONVOL, 14}, // push r14
        {8, UWOP_PUSH_NONVOL, 13},  // push r13
        {6, UWOP_PUSH_NONVOL, 12},  // push r12
        {4, UWOP_PUSH_NONVOL, 7},   // push rdi
        {3, UWOP_PUSH_NONVOL, 6},   // push rsi
        {2, UWOP_PUSH_NONVOL, 5},   // push rbp
        {1, UWOP_PUSH_NONVOL, 3},   // push rbx
    };
    const uint8_t n_codigos = (uint8_t)(sizeof(kPasos) / sizeof(kPasos[0]));
    d->info[0] = 1;                        // version 1, sin banderas
    d->info[1] = (uint8_t)kPrologoLen;     // tamano del prologo
    d->info[2] = n_codigos;                // numero de codigos
    d->info[3] = 0;                        // sin registro de marco
    for (uint8_t i = 0; i < n_codigos; ++i) {
        d->info[4 + i * 2] = kPasos[i].off;
        d->info[4 + i * 2 + 1] =
            (uint8_t)(kPasos[i].op | (kPasos[i].info << 4));
    }

    /* Las direcciones de la tabla son RELATIVAS a una base que elegimos.  Se
     * toma el propio codigo, asi que todos los desplazamientos son pequenos. */
    const DWORD64 base = (DWORD64)(uintptr_t)code;
    d->funcion.BeginAddress = 0;
    d->funcion.EndAddress = (DWORD)n;
    d->funcion.UnwindData =
        (DWORD)((DWORD64)(uintptr_t)d->info - base);
    return RtlAddFunctionTable(&d->funcion, 1, base) != FALSE;
}

} // namespace
#endif // _WIN32

// =====================================================================
//   AS inc.6: registro global hash(NASM) -> trampoline + helper
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

/* Helper nativo invocado por el bytecode (calln
 * @Method("vrt:inline_asm_exec")). Convencion (argc SIEMPRE 12, slots no usados
 * = 0): arg0  proc  = ProcessVM* (via getproc) arg1  hash  = FNV-1a del NASM
 * (clave del trampoline) arg2  desc  = phys-id (4 bits) por binding, hasta 8
 * bindings arg3  n     = numero de bindings (<=8) arg4..11    = direcciones VM
 * de los slots (alloca) de cada binding
 *
 * Marshalling: rellena @c vm->asm_ctx[phys] desde el slot VM (in), llama al
 * trampoline (que carga los 16 GP host desde ctx, corre el asm, y los
 * guarda de vuelta a ctx), y escribe @c ctx[phys] al slot VM (out).  Cada
 * binding es in+out (conservador, siempre correcto). */
extern "C" uint64_t vrt_inline_asm_exec(uint64_t proc, uint64_t hash,
                                        uint64_t desc, uint64_t n_u,
                                        uint64_t s0, uint64_t s1, uint64_t s2,
                                        uint64_t s3, uint64_t s4, uint64_t s5,
                                        uint64_t s6, uint64_t s7) {
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

    const int n = static_cast<int>(n_u);
    const uint64_t slots[8] = {s0, s1, s2, s3, s4, s5, s6, s7};
    uint64_t *ctx = vm->asm_ctx; // host scratch del proceso

    for (int i = 0; i < 16; ++i)
        ctx[i] = 0;
    /* El hueco de cada operando puede vivir en memoria de la maquina virtual o
     * en la del host; el bit 32+i del descriptor dice cual.  Leerlo por la via
     * equivocada daba basura de entrada y tiraba el resultado a la salida. */
    auto es_host = [&](int i) { return (desc >> (32 + i)) & 1ull; };
    // in-marshalling: hueco -> ctx[phys]
    for (int i = 0; i < n && i < 8; ++i) {
        const int phys = static_cast<int>((desc >> (i * 4)) & 0xF);
        if (es_host(i)) {
            std::memcpy(&ctx[phys], reinterpret_cast<const void *>(slots[i]),
                        sizeof(uint64_t));
        } else {
            ctx[phys] = vm->vm_mem.read_u64(slots[i]);
        }
    }
    tramp(ctx); // ejecuta el asm host
    // out-marshalling: ctx[phys] -> hueco
    for (int i = 0; i < n && i < 8; ++i) {
        const int phys = static_cast<int>((desc >> (i * 4)) & 0xF);
        if (es_host(i)) {
            std::memcpy(reinterpret_cast<void *>(slots[i]), &ctx[phys],
                        sizeof(uint64_t));
        } else {
            vm->vm_mem.write_u64(slots[i], ctx[phys]);
        }
    }
    return 0;
}

/* Ejecuta una ASM_MICRO (asm opaco liftado, SIN operandos de registro) en el
 * interprete, con doble via segun haya ensamblador:
 *
 *   arg0 proc = ProcessVM* (scratch ctx para el trampoline).
 *   arg1 hash = FNV-1a de la plantilla (clave del trampoline nativo).
 *   arg2 eff  = bits de efecto de la DB (bit3 barrera) para la EMULACION.
 *
 * Si existe un trampoline nativo (el loader lo construyo con el ensamblador
 * para el host), se ejecuta la instruccion REAL.  Si NO (host sin ensamblador u
 * otra arch), se EMULA su efecto de forma PORTABLE via los eff bits -> el codigo
 * sigue funcionando en cualquier arch (esta es la ventaja de ASM_MICRO sobre la
 * caja opaca INLINE_ASM, que sin ensamblador no puede hacer NADA). */
extern "C" uint64_t vrt_asm_micro_exec(uint64_t proc, uint64_t hash,
                                       uint64_t eff) {
    AsmTrampolineFn tramp = lookup_inline_asm_trampoline(hash);
    if (tramp != nullptr) {
        // Via nativa (ensamblador presente): ejecuta la instruccion REAL.  Sin
        // operandos de registro -> ctx solo es scratch para el prologo/epilogo
        // generico del trampoline.
        auto *vm = reinterpret_cast<runtime::ProcessVM *>(proc);
        if (vm != nullptr) {
            for (int i = 0; i < 16; ++i)
                vm->asm_ctx[i] = 0;
            tramp(vm->asm_ctx);
        }
        return 0;
    }
    // Via portable (sin ensamblador / otra arch): emular el efecto de la DB.
    if (eff & 0x8u) // barrera de memoria (mfence/lfence/sfence)
        std::atomic_thread_fence(std::memory_order_seq_cst);
    // Sin barrera (pause/nop): no-op.
    return 0;
}

void register_inline_asm_runner() {
    ffi::register_virtual_fn("vrt", "inline_asm_exec",
                             reinterpret_cast<void *>(&vrt_inline_asm_exec));
    ffi::register_virtual_fn("vrt", "asm_micro_exec",
                             reinterpret_cast<void *>(&vrt_asm_micro_exec));
}

void build_and_register_inline_asm_trampolines(
    const std::vector<ir::IrFunction> &fns) {
    if (vx::g_asm_backend == nullptr) return; // sin ensamblador -> no-op
    // CodeCache de vida-proceso: los trampolines deben sobrevivir mientras
    // el programa corra.  Una sola instancia compartida por todos los
    // bloques asm (de cualquier modulo cargado).
    static CodeCache asm_cc;
    for (const auto &fn : fns) {
        for (const auto &blk : fn.blocks) {
            for (const auto &ins : blk.instrs) {
                // Cuerpo asm a registrar como trampoline nativo: INLINE_ASM lo
                // lleva en func_name; ASM_MICRO (asm opaco liftado, caso sin
                // operandos de registro) en asm_micros[imm].tmpl.  El interp
                // ejecuta ambos via el trampoline SOLO si hay ensamblador; si no,
                // no se registra y vrt_inline_asm_exec hace no-op (sigue corriendo).
                const std::string *body = nullptr;
                std::string subst; // vive hasta el registro (: $N -> reg)
                if (ins.op == ir::IrOp::INLINE_ASM) {
                    // sustituir $N por el pick GREEDY del binding (el
                    // interp no tiene RA).  Mismo cuerpo que hashea ir_emitter.
                    subst = vx::asm_body_subst_greedy(ins.func_name,
                                                      fn.asm_reg_bindings);
                    body = &subst;
                } else if (ins.op == ir::IrOp::ASM_MICRO &&
                           ins.imm < fn.asm_micros.size()) {
                    const ir::AsmMicro &am = fn.asm_micros[ins.imm];
                    if (am.operands.empty()) {
                        body = &am.tmpl; // sin operandos: verbatim
                    } else if (vx::asm_micro_subst_phys(am, subst)) {
                        body = &subst;   // fisico fijo sustituido
                    }
                    // operando no fisico (SSA/RA) -> no se trampolinea
                }
                if (body == nullptr) continue;
                const uint64_t hash = fnv1a64_asm(*body);
                if (lookup_inline_asm_trampoline(hash) != nullptr) continue;
                std::string err;
                AsmTrampolineFn tp = build_asm_trampoline(*body, asm_cc, &err);
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
AsmTrampolineFn build_asm_trampoline(const std::string &user_asm, CodeCache &cc,
                                     std::string *err) {
    if (vx::g_asm_backend == nullptr) {
        if (err) *err = "no hay backend de ensamblado (Keystone) registrado";
        return nullptr;
    }

#if defined(_WIN32)
    const char *CTX = "rcx"; // Win64: primer arg en rcx
#else
    const char *CTX = "rdi"; // SysV: primer arg en rdi
#endif

    std::string nasm;
    nasm.reserve(user_asm.size() + 1024);

    /* --- prologo: salvar callee-saved --- */
    nasm += "push rbx\npush rbp\npush rsi\npush rdi\n";
    nasm += "push r12\npush r13\npush r14\npush r15\n";
    /* salvar ctx en la pila (sobrevive cualquier clobber del asm) */
    nasm += "push ";
    nasm += CTX;
    nasm += "\n";

    /* --- cargar los 16 GP host desde ctx ([ctx + id*8]); rsp (id 4) NO se
     * carga (no se pisa el stack pointer).  El registro que TRAE ctx se
     * carga el ULTIMO (ya no se necesita ctx tras eso). --- */
    static const struct {
        const char *r;
        int id;
    } REGS[] = {
        {"rax", 0},  {"rcx", 1},  {"rdx", 2},  {"rbx", 3},  {"rbp", 5},
        {"rsi", 6},  {"rdi", 7},  {"r8", 8},   {"r9", 9},   {"r10", 10},
        {"r11", 11}, {"r12", 12}, {"r13", 13}, {"r14", 14}, {"r15", 15},
    };
    char line[64];
    /* OJO: Keystone (NASM) interpreta los enteros BARE como HEX -> emitir
     * los offsets como 0x%x (el valor en hex) para que el disp sea correcto. */
    for (const auto &e : REGS) {
        if (std::strcmp(e.r, CTX) == 0) continue; // ctx-reg: el ultimo
        std::snprintf(line, sizeof(line), "mov %s, [%s + 0x%x]\n", e.r, CTX,
                      e.id * 8);
        nasm += line;
    }
    /* el reg que trae ctx: cargarlo ahora (su valor logico esta en ctx) */
    {
        int ctx_id = (std::strcmp(CTX, "rcx") == 0) ? 1 : 7; // rcx=1, rdi=7
        std::snprintf(line, sizeof(line), "mov %s, [%s + 0x%x]\n", CTX, CTX,
                      ctx_id * 8);
        nasm += line;
    }

    /* --- cuerpo del usuario --- */
    nasm += user_asm;
    if (!user_asm.empty() && user_asm.back() != '\n') nasm += "\n";

    /* --- epilogo: salvar los 16 GP host de vuelta a ctx --- */
    nasm += "push rax\n";           // salvar rax (resultado) temporal
    nasm += "mov rax, [rsp + 8]\n"; // recargar ctx (rax = ctx)
    for (const auto &e : REGS) {
        if (e.id == 0) continue; // rax se salva al final (via pop)
        std::snprintf(line, sizeof(line), "mov [rax + 0x%x], %s\n", e.id * 8,
                      e.r);
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
        std::fprintf(stderr,
                     "=== trampoline NASM ===\n%s=======================\n",
                     nasm.c_str());
    }

    /* --- ensamblar (Keystone) --- */
    vx::AsmAssembleResult ar =
        vx::g_asm_backend->assemble(nasm, vx::AsmArch::X86_64);
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
#if defined(_WIN32)
    /* Y se dice como desenrollarlo.  Sin esto, un fallo del procesador dentro
     * del ensamblador se puede recoger o llevarse el proceso segun como haya
     * quedado la pila -- ver `registrar_desenrollado`. */
    if (!registrar_desenrollado(code, ar.bytes.size(), cc) &&
        std::getenv("VESTA_ASM_TRAMP_DEBUG")) {
        std::fprintf(stderr, "[asm] trampoline sin info de desenrollado: un "
                             "fallo dentro del asm no sera recuperable\n");
    }
#endif
    return reinterpret_cast<AsmTrampolineFn>(code);
}

} // namespace jit
