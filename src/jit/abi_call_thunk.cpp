/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file jit/abi_call_thunk.cpp
 * @brief El thunk que hace cumplir una convencion declarada.
 *
 * El porque esta en la cabecera.  Aqui va la FORMA del codigo generado, que es
 * lo unico delicado:
 *
 *   1. Salva los preservados y se queda el contexto en @c rbx.
 *   2. Copia el destino a la PILA.  Es lo primero que se hace con el contexto
 *      porque al final no queda ningun registro con el que alcanzarlo: todos
 *      llevan ya el valor que la convencion les asigno.
 *   3. Reserva el marco y coloca ahi los argumentos de pila, al desplazamiento
 *      que la plataforma pide -- 0x20 de sombra en Windows, 0 en System V --.
 *   4. Carga los generales desde el contexto, y @c rbx el ULTIMO: hasta esa
 *      linea es lo unico que sabe donde esta el contexto.
 *   5. Salta al destino leyendolo de su hueco de la pila.
 *
 * ALINEAMIENTO, que es donde esto se rompe sin avisar: al entrar, @c rsp vale
 * 8 modulo 16 (lo dejo asi el @c call de quien nos llamo).  Ocho `push` no
 * cambian esa congruencia, el noveno la lleva a 0, y el marco se redondea a
 * multiplo de 16 para conservarla.  Asi el destino recibe la pila como manda
 * su ABI.  Una llamada desalineada no falla aqui: falla dentro, en la primera
 * instruccion vectorial alineada que encuentre.
 */
#include "jit/abi_call_thunk.h"

#include "jit/code_cache.h"
#include "jit/inline_asm_trampoline.h"
#include "ffi/virtual_lib_registry.h"
#include "vx/asm/asm_backend.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace jit {

namespace {

/// Registro que trae el primer argumento, que es por donde llega el contexto.
#if defined(_WIN32)
constexpr const char *kCtxReg = "rcx";
/// Hueco de sombra que el llamado puede usar; los argumentos de pila van
/// DETRAS.  System V no lo tiene.
constexpr int kShadow = 0x20;
#else
constexpr const char *kCtxReg = "rdi";
constexpr int kShadow = 0;
#endif

/// Desplazamientos dentro de @ref AbiCallCtx, en bytes.  Se derivan del struct
/// y no se escriben a mano: si alguien le anade un campo delante, el codigo
/// generado sigue direccionando bien.
constexpr int kOffTarget = static_cast<int>(offsetof(AbiCallCtx, target));
constexpr int kOffStack = static_cast<int>(offsetof(AbiCallCtx, stack));

/// Los generales por identificador fisico.  @c rsp (4) no esta: el thunk no
/// pisa el puntero de pila, y @c rbx va el ultimo porque hasta entonces es el
/// que sostiene el contexto.
struct GpSlot {
    const char *name;
    int id;
};
const GpSlot kGp[] = {
    {"rax", 0},  {"rcx", 1},  {"rdx", 2},  {"rbp", 5},  {"rsi", 6},
    {"rdi", 7},  {"r8", 8},   {"r9", 9},   {"r10", 10}, {"r11", 11},
    {"r12", 12}, {"r13", 13}, {"r14", 14}, {"r15", 15},
};

/// Redondea @p n al siguiente multiplo de 16.
int align16(int n) {
    return (n + 15) & ~15;
}

/// El texto del thunk para @p n_stack argumentos de pila.
std::string thunk_source(size_t n_stack) {
    const int frame = align16(kShadow + static_cast<int>(n_stack) * 8);
    std::string s;
    s.reserve(1024);
    char line[96];

    s += "push rbx\npush rbp\npush rsi\npush rdi\n";
    s += "push r12\npush r13\npush r14\npush r15\n";
    std::snprintf(line, sizeof(line), "mov rbx, %s\n", kCtxReg);
    s += line;

    /* El destino, a la pila: despues de cargar los generales no quedara
     * ninguno con el que alcanzarlo. */
    std::snprintf(line, sizeof(line), "mov rax, [rbx + 0x%x]\n", kOffTarget);
    s += line;
    s += "push rax\n";

    std::snprintf(line, sizeof(line), "sub rsp, 0x%x\n", frame);
    s += line;

    /* Los de pila, a su sitio.  `rax` de paso porque todavia no lleva su valor
     * definitivo -- se carga abajo, con el resto --. */
    for (size_t i = 0; i < n_stack; ++i) {
        std::snprintf(line, sizeof(line), "mov rax, [rbx + 0x%x]\n",
                      kOffStack + static_cast<int>(i) * 8);
        s += line;
        std::snprintf(line, sizeof(line), "mov [rsp + 0x%x], rax\n",
                      kShadow + static_cast<int>(i) * 8);
        s += line;
    }

    /* Y ahora los generales.  `rbx` el ultimo: es lo unico que sabe donde esta
     * el contexto. */
    for (const GpSlot &g : kGp) {
        std::snprintf(line, sizeof(line), "mov %s, [rbx + 0x%x]\n", g.name,
                      g.id * 8);
        s += line;
    }
    s += "mov rbx, [rbx + 0x18]\n"; // 3*8: el propio rbx

    /* Al destino, leido de su hueco.  El `call` empuja 8, asi que el llamado
     * ve los argumentos de pila justo donde su ABI los busca. */
    std::snprintf(line, sizeof(line), "call qword [rsp + 0x%x]\n", frame);
    s += line;

    std::snprintf(line, sizeof(line), "add rsp, 0x%x\n", frame + 8);
    s += line;
    s += "pop r15\npop r14\npop r13\npop r12\n";
    s += "pop rdi\npop rsi\npop rbp\npop rbx\n";
    s += "ret\n";
    return s;
}

/// Uno por cuenta de argumentos de pila.  Viven lo que el proceso.
struct ThunkTable {
    AbiCallThunkFn fn[kAbiCallMaxArgs + 1] = {nullptr};
    std::mutex mtx;
};

ThunkTable &table() {
    static ThunkTable t;
    return t;
}

} // namespace

AbiCallThunkFn abi_call_thunk_for(size_t n_stack, std::string *err) {
    if (n_stack > kAbiCallMaxArgs) {
        if (err) *err = "demasiados argumentos por pila";
        return nullptr;
    }
    ThunkTable &t = table();
    std::lock_guard<std::mutex> lk(t.mtx);
    if (t.fn[n_stack] != nullptr) return t.fn[n_stack];

    /* El mismo caché que los trampolines del asm en linea, y por lo mismo: el
     * codigo generado tiene que seguir ahi mientras alguien pueda llamarlo. */
    static CodeCache cc;
    const std::string src = thunk_source(n_stack);
    /* Se reutiliza el constructor del asm en linea solo para ENSAMBLAR y
     * alojar?  No: aquel envuelve el cuerpo con su propio prologo y carga los
     * generales ANTES, con lo que el contexto se pierde y no se podrian
     * colocar los de pila.  Aqui el cuerpo ES el prologo. */
    if (vx::g_asm_backend == nullptr) {
        if (err) *err = "no hay backend de ensamblado registrado";
        return nullptr;
    }
    vx::AsmAssembleResult ar =
        vx::g_asm_backend->assemble(src, vx::AsmArch::X86_64);
    if (!ar.ok || ar.bytes.empty()) {
        if (err) *err = ar.ok ? "ensamblado vacio" : ar.error;
        return nullptr;
    }
    uint8_t *code = cc.alloc(ar.bytes.size(), 16);
    if (code == nullptr) {
        if (err) *err = "no cabe en el cache de codigo";
        return nullptr;
    }
    std::memcpy(code, ar.bytes.data(), ar.bytes.size());
    cc.commit(code, ar.bytes.size());

    t.fn[n_stack] = reinterpret_cast<AbiCallThunkFn>(code);
    return t.fn[n_stack];
}

/**
 * @brief Llama a @p fn cumpliendo la convencion que dice @p desc.
 *
 * Lo invoca el bytecode (`calln @Method("vrt:call_with_abi")`) cuando el
 * destino declara una convencion propia.  El reparto va en @p desc -- cinco
 * bits por argumento: el registro, o @ref kAbiArgOnStack -- y los valores en
 * un bloque contiguo, que es como el bajado los deja.
 *
 * Los que van por la pila conservan SU ORDEN entre ellos: el que ocupa la
 * posicion mas baja de los no-fijados es el primero, y asi sucesivamente.  Es
 * lo que hace que la convencion se pueda escribir salteada -- fijar el sexto
 * argumento y dejar sueltos el segundo y el tercero -- sin que el reparto de
 * la pila dependa de cuales se fijaron.
 *
 * @param proc     ProcessVM actual; no se usa todavia, pero entra por la misma
 *                 puerta que el resto de ayudantes para no tener dos formas.
 * @param fn       Direccion nativa del destino.
 * @param desc     Donde va cada argumento.
 * @param argc     Cuantos son.
 * @param args_ptr Puntero del ANFITRION al bloque de valores.
 * @return Lo que el destino deje en el registro de resultado.
 */
extern "C" uint64_t vrt_call_with_abi(uint64_t proc, uint64_t fn, uint64_t desc,
                                      uint64_t argc, uint64_t args_ptr) {
    (void)proc;
    if (fn == 0 || args_ptr == 0) return 0;
    if (argc > kAbiCallMaxArgs) return 0;

    const uint64_t *args = reinterpret_cast<const uint64_t *>(args_ptr);
    AbiCallCtx ctx;
    ctx.target = fn;
    size_t n_stack = 0;
    for (size_t i = 0; i < static_cast<size_t>(argc); ++i) {
        const unsigned slot = abi_arg_slot(desc, i);
        if (slot == kAbiArgOnStack) {
            ctx.stack[n_stack++] = args[i];
        } else if (slot < 16) {
            ctx.gp[slot] = args[i];
        }
    }

    std::string err;
    AbiCallThunkFn thunk = abi_call_thunk_for(n_stack, &err);
    if (thunk == nullptr) return 0;
    return thunk(&ctx);
}

void register_abi_call_runner() {
    ffi::register_virtual_fn("vrt", "call_with_abi",
                             reinterpret_cast<void *>(&vrt_call_with_abi));
}

} // namespace jit
