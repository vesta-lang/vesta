/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file jit/abi_call_thunk.cpp
 * @brief Hacer cumplir una convencion declarada al llamar desde el interprete.
 *
 * El porque esta en la cabecera.  Aqui hay DOS cosas separadas a proposito:
 *
 *  - lo NEUTRO -- el ayudante que el bytecode invoca, el reparto de cada valor
 *    a su sitio y la cache de thunks --, que no sabe de ninguna arquitectura;
 *  - el GENERADOR, que es lo unico que conoce un banco de registros, un
 *    ensamblador y una ABI.  Hay uno por arquitectura, igual que el resto del
 *    codegen, y una sin generador lo DICE en vez de apanarse.
 */
#include "jit/abi_call_thunk.h"

#include "ffi/virtual_lib_registry.h"
#include "jit/code_cache.h"
#include "runtime/exception_runtime.h"
#include "runtime/proceso_runtime.h" // los valores viven en la memoria de la VM
#include "util/name_pool.h"
#include "vx/asm/asm_backend.h"
#include "vx/asm/asm_phys_reg.h"
#include "vx/asm/instr_db.h"
#include "vx/diag/diag_catalog.h"

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

/**
 * @def VESTA_STACK_BLOCK
 * @brief Un bloque de @p n bytes en el marco de quien lo pide.
 *
 * Se usa para traer los argumentos de una llamada con convencion declarada.
 * Un bloque del MARCO y no del monton porque esto esta en el camino de cada
 * llamada, y porque su tamano lo decide la declaracion: fijar un maximo aqui
 * convertiria una decision de transporte en un limite del lenguaje.  Muere al
 * volver, que es justo lo que hace falta.
 */
#if defined(_MSC_VER)
#include <malloc.h>
#define VESTA_STACK_BLOCK(n) _alloca(n)
#else
#define VESTA_STACK_BLOCK(n) __builtin_alloca(n)
#endif

namespace jit {

/// Codigo con el que se cuenta que una llamada llego sin destino o sin
/// argumentos.  Aqui y no suelto en el sitio: el texto vive en el catalogo.
constexpr const char *kDiagNoTarget = "VXE942";

namespace {

// ===========================================================================
//  Generador x86-64
// ===========================================================================
//
// La forma del codigo, que es lo unico delicado:
//
//   1. Salva los preservados y se queda el contexto en `rbx`.
//   2. Copia el destino a la PILA.  Es lo primero que se hace con el contexto
//      porque al final no queda ningun registro con el que alcanzarlo: todos
//      llevan ya el valor que la convencion les asigno.
//   3. Reserva el marco y coloca ahi los argumentos de pila, al desplazamiento
//      que la plataforma pide -- 0x20 de sombra en Windows, 0 en System V --.
//   4. Carga los generales desde el contexto, y `rbx` el ULTIMO: hasta esa
//      linea es lo unico que sabe donde esta el contexto.
//   5. Salta al destino leyendolo de su hueco de la pila.
//
// ALINEAMIENTO, que es donde esto se rompe sin avisar: al entrar, `rsp` vale 8
// modulo 16 (lo dejo asi el `call` de quien nos llamo).  Ocho `push` no cambian
// esa congruencia, el noveno la lleva a 0, y el marco se redondea a multiplo de
// 16 para conservarla.  Una llamada desalineada no falla aqui: falla dentro, en
// la primera instruccion vectorial alineada que encuentre.

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

/// Identificadores fisicos que el generador trata aparte.  `rsp` no puede ser
/// destino -- el thunk no pisa el puntero de pila --, y `rbx` sostiene el
/// contexto hasta el final, asi que se carga el ultimo.
constexpr int kRspId = 4;
constexpr int kRbxId = 3;
/// El que sostiene la base de los valores mientras se cargan los demas.
constexpr int kBaseId = 11; // r11

/**
 * @brief Como se nombra un general de x86-64 a 64 bits.  Vacio si no existe.
 *
 * La tabla es la CANONICA -- la misma por la que se lee `register("rXX")` en
 * la declaracion --, para que no haya dos opiniones sobre que numero es cada
 * registro.  Y el nombre se INTERNA: se repite en cada linea que se emite y en
 * cada thunk que se genera, asi que se reparte una vez y desde ahi se pasa por
 * puntero.
 */
const std::string &gp_name_x86_64(int phys) {
    return *util::intern_name(vx::asm_phys_reg_name(
        static_cast<uint8_t>(vx::instr_db::Isa::X86), vx::ASM_RC_GP, phys, 64));
}

/// Desplazamientos dentro de @ref AbiCallCtx, derivados del struct y no
/// escritos a mano: si alguien le anade un campo delante, el codigo generado
/// sigue direccionando bien.
constexpr int kOffTarget = static_cast<int>(offsetof(AbiCallCtx, target));
constexpr int kOffArgs = static_cast<int>(offsetof(AbiCallCtx, args));

/// Redondea @p n al siguiente multiplo de 16.
int align16(int n) { return (n + 15) & ~15; }

/// Un argumento que va en registro: a cual, y de que hueco del bloque sale.
struct RegArg {
    int phys = 0;
    size_t at = 0;
};
/// Uno que va por la pila.  Su POSICION en el reparto es su orden, que
/// es justo lo que hay que conservar.
struct StackArg {
    size_t at = 0;
};

/**
 * @brief Donde acaba cada argumento, resuelto una vez al generar.
 *
 * Dos listas con su propio tipo de elemento y no dos vectores de enteros: un
 * `size_t` suelto no dice si es un registro, un hueco o un orden, y las tres
 * cosas conviven aqui.
 */
struct ArgAssignment {
    std::vector<RegArg> in_reg;
    std::vector<StackArg> on_stack;
};

/**
 * @brief Reparte @p slots en registros y pila para x86-64.
 *
 * Los no fijados conservan SU orden entre ellos, que es lo que permite
 * escribir una convencion salteada -- fijar el sexto y dejar sueltos el
 * segundo y el tercero -- sin que el reparto de la pila dependa de cuales se
 * fijaron.
 *
 * @param slots Donde va cada argumento.
 * @param why   Recibe el motivo; @ref AbiCallReason::Ok si el reparto salio.
 * @return El reparto, vacio si @p why quedo distinto de @c Ok.
 */
ArgAssignment assign_args_x86_64(AbiArgSlots slots, AbiCallReason *why) {
    ArgAssignment assigned;
    *why = AbiCallReason::Ok;
    for (size_t i = 0; i < slots.count; ++i) {
        const uint16_t where = slots.slot_of(i);
        if (where == kAbiArgOnStack) {
            assigned.on_stack.push_back(StackArg{i});
            continue;
        }
        const int phys = static_cast<int>(where);
        if (gp_name_x86_64(phys).empty()) {
            *why = AbiCallReason::RegNotInIsa;
            return ArgAssignment();
        }
        if (phys == kRspId) {
            *why = AbiCallReason::RegIsStackPointer;
            return ArgAssignment();
        }
        assigned.in_reg.push_back(RegArg{phys, i});
    }
    return assigned;
}

/**
 * @brief El texto del thunk x86-64 para la convencion @p slots.
 *
 * El reparto va HORNEADO: cada registro se carga de su hueco del bloque de
 * valores, asi que al llamar no queda nada que repartir.
 *
 * @param slots Donde va cada argumento.
 * @param why   Motivo si la convencion no se puede cumplir en esta ISA.
 * @return El texto, o vacio si @p why quedo distinto de @c Ok.
 */
std::string thunk_source_x86_64(AbiArgSlots slots, AbiCallReason *why) {
    const ArgAssignment assigned = assign_args_x86_64(slots, why);
    if (*why != AbiCallReason::Ok) return std::string();

    const int frame =
        align16(kShadow + static_cast<int>(assigned.on_stack.size()) * 8);
    std::string s;
    s.reserve(1024);
    char line[96];

    s += "push rbx\npush rbp\npush rsi\npush rdi\n";
    s += "push r12\npush r13\npush r14\npush r15\n";
    snprintf(line, sizeof(line), "mov rbx, %s\n", kCtxReg);
    s += line;

    /* El destino, a la pila: despues de cargar los generales no quedara
     * ninguno con el que alcanzarlo. */
    snprintf(line, sizeof(line), "mov rax, [rbx + 0x%x]\n", kOffTarget);
    s += line;
    s += "push rax\n";

    snprintf(line, sizeof(line), "sub rsp, 0x%x\n", frame);
    s += line;

    /* La base de los valores a `r11`, y `rbx` deja de hacer falta.  A partir
     * de aqui todo se lee de `[r11 + indice*8]`. */
    snprintf(line, sizeof(line), "mov r11, [rbx + 0x%x]\n", kOffArgs);
    s += line;

    /* Los de pila, a su sitio: su posicion en el reparto es su orden. */
    for (size_t k = 0; k < assigned.on_stack.size(); ++k) {
        snprintf(line, sizeof(line), "mov rax, [r11 + 0x%x]\n",
                 static_cast<int>(assigned.on_stack[k].at) * 8);
        s += line;
        snprintf(line, sizeof(line), "mov [rsp + 0x%x], rax\n",
                 kShadow + static_cast<int>(k) * 8);
        s += line;
    }

    /* Y los de registro.  `rbx` y `r11` van los ULTIMOS y en ese orden: hasta
     * su linea, `r11` es lo unico que sabe donde estan los valores. */
    for (int pass = 0; pass < 3; ++pass) {
        for (const RegArg &a : assigned.in_reg) {
            const bool late = a.phys == kRbxId || a.phys == kBaseId;
            if (pass == 0 && late) continue;
            if (pass == 1 && a.phys != kRbxId) continue;
            if (pass == 2 && a.phys != kBaseId) continue;
            snprintf(line, sizeof(line), "mov %s, [r11 + 0x%x]\n",
                     gp_name_x86_64(a.phys).c_str(), static_cast<int>(a.at) * 8);
            s += line;
        }
    }

    /* Al destino, leido de su hueco.  El `call` empuja 8, asi que el llamado
     * ve los argumentos de pila justo donde su ABI los busca. */
    snprintf(line, sizeof(line), "call qword [rsp + 0x%x]\n", frame);
    s += line;

    snprintf(line, sizeof(line), "add rsp, 0x%x\n", frame + 8);
    s += line;
    s += "pop r15\npop r14\npop r13\npop r12\n";
    s += "pop rdi\npop rsi\npop rbp\npop rbx\n";
    s += "ret\n";
    return s;
}

/**
 * @brief El generador del objetivo activo, o nulo si esa ISA no tiene uno.
 *
 * Hoy solo x86-64.  Cuando arm64 traiga el suyo, este es el unico sitio que
 * cambia -- y mientras tanto, un objetivo sin generador contesta nulo y quien
 * pregunta lo DICE, que es lo unico honesto: llamar sin cumplir la convencion
 * es el fallo que todo esto viene a evitar.
 */
struct ThunkGen {
    std::string (*source)(AbiArgSlots slots, AbiCallReason *why) = nullptr;
    vx::AsmArch arch = vx::AsmArch::X86_64;
};

ThunkGen generator_for_host() {
    ThunkGen g;
#if defined(__x86_64__) || defined(_M_X64)
    g.source = &thunk_source_x86_64;
    g.arch = vx::AsmArch::X86_64;
#endif
    return g;
}

/**
 * @brief Los thunks ya generados, uno por convencion distinta.
 *
 * Un array plano recorrido en orden y no una tabla asociativa: un programa usa
 * un punado de convenciones, asi que el recorrido cabe en cache y -- lo que
 * importa aqui -- BUSCAR NO RESERVA, que es la condicion para que esto pueda
 * estar en el camino de cada llamada.
 */
struct ThunkEntry {
    std::vector<uint16_t> slots; ///< la convencion, copiada.
    AbiCallThunkFn fn = nullptr;
};

struct ThunkTable {
    std::vector<ThunkEntry> entries;
    std::mutex mtx;
};

ThunkTable &table() {
    static ThunkTable t;
    return t;
}

/// Si @p e es la convencion @p slots.
bool same_convention(const ThunkEntry &e, AbiArgSlots slots) {
    if (e.slots.size() != slots.count) return false;
    for (size_t i = 0; i < slots.count; ++i)
        if (e.slots[i] != slots.slot_of(i)) return false;
    return true;
}

} // namespace

const char *abi_call_reason_code(AbiCallReason r) {
    switch (r) {
    case AbiCallReason::Ok: return "";
    case AbiCallReason::NoGenerator: return "VXE936";
    case AbiCallReason::NoAsmBackend: return "VXE937";
    case AbiCallReason::AssembleFailed: return "VXE938";
    case AbiCallReason::NoCodeSpace: return "VXE939";
    case AbiCallReason::RegNotInIsa: return "VXE940";
    case AbiCallReason::RegIsStackPointer: return "VXE941";
    }
    return "";
}

AbiCallThunkFn abi_call_thunk_for(AbiArgSlots slots, AbiCallReason *why) {
    AbiCallReason ignored = AbiCallReason::Ok;
    if (why == nullptr) why = &ignored;
    *why = AbiCallReason::Ok;

    const ThunkGen gen = generator_for_host();
    if (gen.source == nullptr) {
        *why = AbiCallReason::NoGenerator;
        return nullptr;
    }
    ThunkTable &t = table();
    std::lock_guard<std::mutex> lk(t.mtx);
    for (const ThunkEntry &e : t.entries)
        if (same_convention(e, slots)) return e.fn;

    /* El mismo cache que los trampolines del asm en linea, y por lo mismo: el
     * codigo generado tiene que seguir ahi mientras alguien pueda llamarlo. */
    static CodeCache cc;
    if (vx::g_asm_backend == nullptr) {
        *why = AbiCallReason::NoAsmBackend;
        return nullptr;
    }
    const std::string src = gen.source(slots, why);
    if (*why != AbiCallReason::Ok) return nullptr;
    vx::AsmAssembleResult ar = vx::g_asm_backend->assemble(src, gen.arch);
    if (!ar.ok || ar.bytes.empty()) {
        *why = AbiCallReason::AssembleFailed;
        return nullptr;
    }
    uint8_t *code = cc.alloc(ar.bytes.size(), 16);
    if (code == nullptr) {
        *why = AbiCallReason::NoCodeSpace;
        return nullptr;
    }
    std::memcpy(code, ar.bytes.data(), ar.bytes.size());
    cc.commit(code, ar.bytes.size());

    ThunkEntry e;
    e.slots.reserve(slots.count);
    for (size_t i = 0; i < slots.count; ++i)
        e.slots.push_back(slots.slot_of(i));
    e.fn = reinterpret_cast<AbiCallThunkFn>(code);
    t.entries.push_back(std::move(e));
    return t.entries.back().fn;
}

/**
 * @brief Llama a @p fn cumpliendo la convencion que dicen las ranuras.
 *
 * Lo invoca el bytecode.  El reparto llega como un identificador de registro
 * por argumento, y los valores en un bloque contiguo: ninguno de los dos lleva
 * tope, que es lo que permite que la convencion sea la que sea.
 *
 * Los que van por la pila conservan SU ORDEN entre ellos: el primero de los no
 * fijados es el primero de la pila.  Es lo que hace que una convencion se
 * pueda escribir salteada -- fijar el sexto y dejar sueltos el segundo y el
 * tercero -- sin que el reparto de la pila dependa de cuales se fijaron.
 *
 * El reparto ya va horneado en el thunk, que se busca por la convencion, asi
 * que aqui no queda nada que repartir.  Lo unico que hay que hacer es traer
 * los valores: viven en la memoria de la VM, que es PAGINADA -- un puntero
 * suyo solo vale dentro de su pagina --, asi que no se le puede dar al codigo
 * generado y hay que copiarlos.  La copia va en el marco de esta misma
 * funcion, no en el monton: es un qword por argumento, esto es una hoja, y el
 * camino de llamada no puede pagar una reserva.
 *
 * @param proc      ProcessVM actual; entra por la misma puerta que el resto de
 *                  ayudantes para no tener dos formas.
 * @param fn        Direccion nativa del destino.
 * @param slots_addr Direccion VM del reparto (un @c uint16_t por argumento).
 * @param argc      Cuantos argumentos son.
 * @param args_addr Direccion VM del bloque de valores (un qword por argumento).
 * @return Lo que el destino deje en el registro de resultado.
 */
extern "C" uint64_t vrt_call_with_abi(uint64_t proc, uint64_t fn,
                                      uint64_t slots_addr, uint64_t argc,
                                      uint64_t args_addr) {
    runtime::ProcessVM *vm = reinterpret_cast<runtime::ProcessVM *>(proc);
    if (vm == nullptr || fn == 0 || (argc != 0 && args_addr == 0)) {
        runtime::throw_fatal(vm, runtime::FATAL_NULL_POINTER,
                             vx::diag::format(kDiagNoTarget).c_str());
        return 0;
    }

    const size_t n = static_cast<size_t>(argc);
    uint64_t *args =
        static_cast<uint64_t *>(VESTA_STACK_BLOCK(n * sizeof(uint64_t)));
    uint16_t *at =
        static_cast<uint16_t *>(VESTA_STACK_BLOCK(n * sizeof(uint16_t)));
    if (n != 0) {
        vm->vm_mem.read_bytes(args_addr, args, n * sizeof(uint64_t));
        vm->vm_mem.read_bytes(slots_addr, at, n * sizeof(uint16_t));
    }

    AbiArgSlots slots;
    slots.at = at;
    slots.count = n;

    AbiCallReason why = AbiCallReason::Ok;
    AbiCallThunkFn thunk = abi_call_thunk_for(slots, &why);
    if (thunk == nullptr) {
        /* No se puede cumplir la convencion.  Devolver un valor cualquiera
         * seria lo peor posible: para el nucleo NT el cero es
         * @c STATUS_SUCCESS, asi que la llamada pareceria haber ido bien. */
        runtime::throw_fatal(
            vm, runtime::FATAL_INVALID_SYSCALL,
            vx::diag::format(abi_call_reason_code(why)).c_str());
        return 0;
    }

    AbiCallCtx ctx;
    ctx.target = fn;
    ctx.args = args;
    return thunk(&ctx);
}

void register_abi_call_runner() {
    ffi::register_virtual_fn("vrt", "call_with_abi",
                             reinterpret_cast<void *>(&vrt_call_with_abi));
}

} // namespace jit
