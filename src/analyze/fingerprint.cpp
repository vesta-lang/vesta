/**
 * @file fingerprint.cpp
 * @brief Implementacion de la huella computacional por-funcion (ver
 *        analyze/fingerprint.h).
 */
#include "analyze/fingerprint.h"

#include <functional>
#include <unordered_map>
#include <unordered_set>

#include "analysis/memory/memory_access.h" // quien decide si una op toca memoria
#include "vx/diag/diag_catalog.h" // el detalle de cada veredicto, por idioma
#include "vx/module/namespace_flatten.h" // el nombre que escribio el usuario
#include "ir/ssa_ir.h"
#include "ir/ir_type_info.h" // vocabulario UNICO de anchura/clase de un IrType
#include "vx/asm/asm_analyze.h"

namespace analyze {

namespace {

/* Los bytes que un tipo ocupa en el marco (el tamano de un ALLOCA) los contesta
 * el vocabulario unico (ir/ir_type_info.h) -- aqui vivia otra copia de esa
 * tabla.  El eje es el de ALMACENAMIENTO, y es justo el que necesita un
 * resumen que SUMA bytes reservados: void ocupa cero. */

/// @c true si @p op puede ALLOCAR en heap (para @c @alloc: contar todo lo que
/// pueda allocar es sound -- @c @alloc(0) solo pasa si no hay ninguno).
bool is_alloc_op(ir::IrOp op) {
    using Op = ir::IrOp;
    switch (op) {
    case Op::RAW_ALLOC:
    case Op::GC_ALLOC:
    case Op::GC_ALLOCP:
    case Op::NEWOBJ:
    case Op::NEWOBJS:
    case Op::ARRAY_ALLOC:
    case Op::STRMAKE:
    case Op::STRCAT:
    case Op::STRCONV:
    case Op::STRFLAT:
    case Op::STRRESERVE:
    case Op::STRINTERN: return true;
    default: return false;
    }
}

/// @c true si @p op NO tiene efectos de dato observables (whitelist).  Lo que
/// no esta aqui (ni es CALL/TAILCALL estatico, que se compone) se considera
/// IMPURO -> soundness: un op nuevo/desconocido bloquea @c @pure hasta
/// anadirlo. THROW/PANIC son efectos de CONTROL (se rastrean aparte), no de
/// dato -> puros. Las allocaciones son puras (una funcion pura puede construir
/// su retorno).
bool is_pure_op(ir::IrOp op) {
    using Op = ir::IrOp;
    switch (op) {
    // Constantes / movimientos / direcciones.
    case Op::CONST:
    case Op::MOV:
    case Op::NOP:
    case Op::STR_LIT_ADDR:
    case Op::LABEL_ADDR:
    case Op::SECTION_REF:
    // Aritmetica entera.
    case Op::ADD:
    case Op::SUB:
    case Op::MUL:
    case Op::DIV:
    case Op::MOD:
    case Op::NEG:
    case Op::IABS:
    case Op::IMIN:
    case Op::IMAX:
    case Op::IMINU:
    case Op::IMAXU:
    case Op::ILOG2:
    // Aritmetica float.
    case Op::FADD:
    case Op::FSUB:
    case Op::FMUL:
    case Op::FDIV:
    case Op::FNEG:
    case Op::FABS:
    case Op::FSQRT:
    case Op::FMIN:
    case Op::FMAX:
    case Op::FFLOOR:
    case Op::FCEIL:
    case Op::FROUND:
    case Op::FTRUNC:
    /* Vector.  Solo las que se quedan DENTRO de la funcion.
     *
     * VEC_UNOP / VEC_BINOP / VEC_BINOP_S / VEC_FMA estaban aqui y NO son puras:
     * su primer operando es el puntero DESTINO del bucle vectorizado, memoria
     * que viene de fuera.  El vocabulario de acceso siempre dijo que escriben;
     * esta lista decia que no, y ganaba esta -- una funcion cuyo unico trabajo
     * era rellenar el vector del llamante salia `pure_local`, y un @pure
     * declarado sobre ella se aprobaba como "puro" en vez de marcarse violado.
     *
     * Las VEC_ACC_* si se quedan: escriben en el acumulador, que el
     * vectorizador crea como un ALLOCA de la propia funcion (ver
     * `acc_slot` en vectorize.cpp).  Escribir en un local propio no es un
     * efecto que nadie de fuera pueda observar, que es justo lo que "reduccion
     * local" queria decir.  VEC_BCAST tampoco toca memoria: difunde un escalar
     * a un registro. */
    case Op::VEC_BCAST:
    case Op::VEC_ACC_ZERO:
    case Op::VEC_ACC_ADD:
    case Op::VEC_ACC_FMA:
    case Op::VEC_ACC_STORE:
    case Op::VEC_ACC_COMBINE:
    // Bitwise.
    case Op::AND:
    case Op::OR:
    case Op::XOR:
    case Op::NOT:
    case Op::SHL:
    case Op::SHR:
    case Op::SAR:
    case Op::CLZ:
    case Op::CTZ:
    case Op::POPCNT:
    case Op::BYTESWAP:
    case Op::ROTL:
    case Op::ROTR:
    // Comparaciones.
    case Op::CMP_EQ:
    case Op::CMP_NE:
    case Op::CMP_LT:
    case Op::CMP_GT:
    case Op::CMP_LE:
    case Op::CMP_GE:
    case Op::CMP_ULT:
    case Op::CMP_UGT:
    case Op::CMP_ULE:
    case Op::CMP_UGE:
    case Op::FCMP_EQ:
    case Op::FCMP_NE:
    case Op::FCMP_LT:
    case Op::FCMP_GT:
    case Op::FCMP_LE:
    case Op::FCMP_GE:
    // Casts.
    case Op::CAST:
    case Op::ZEXT:
    case Op::SEXT:
    case Op::TRUNC:
    case Op::ITOF:
    case Op::UITOF:
    case Op::FTOI:
    case Op::FTOUI:
    case Op::F32TOF64:
    case Op::F64TOF32:
    case Op::BITCAST:
    // Control de flujo (los efectos de control se rastrean aparte).
    case Op::BR:
    case Op::BR_COND:
    case Op::RET:
    case Op::UNREACHABLE:
    case Op::PHI:
    case Op::SWITCH_DENSE:
    case Op::MATCH_VARIANT:
    case Op::THROW:
    case Op::RETHROW:
    case Op::PANIC:
    case Op::TRYENTER:
    case Op::TRYLEAVE:
    case Op::LANDINGPAD:
    // Lecturas (puras; para determinismo se afinara aparte).
    case Op::LOAD:
    case Op::GETFIELD:
    case Op::ARRAY_LOAD:
    case Op::ARRAY_LEN:
    case Op::GETSTATIC:
    case Op::GEP:
    case Op::GCDEREF_IR:
    case Op::GC_DEREF_HOST:
    case Op::GC_HANDLE_FOR_PTR:
    case Op::INSTANCEOF:
    case Op::CHECKCAST:
    case Op::ISNULL:
    case Op::UNWRAP:
    case Op::REFLECT_COUNT:
    case Op::REFLECT_AT:
    case Op::FINDCLASS:
    case Op::FINDMETHOD:
    case Op::FINDFIELD:
    case Op::SHARED_STAT:
    case Op::READ_VM_REG:
    case Op::GETPROC:
    case Op::GETVM:
    case Op::GETMGR:
    case Op::GETPID:
    case Op::GETARGC:
    case Op::GETARG:
    // Alloc local + construccion de valores (allocar es puro).
    case Op::ALLOCA:
    case Op::RAW_ALLOC:
    case Op::GC_ALLOC:
    case Op::GC_ALLOCP:
    case Op::NEWOBJ:
    case Op::NEWOBJS:
    case Op::ARRAY_ALLOC:
    case Op::MAKE_VARIANT:
    case Op::MAKE_CLOSURE:
    case Op::STRMAKE:
    case Op::STRCAT:
    case Op::STRSLICE:
    case Op::STRFLAT:
    case Op::STRHASH:
    case Op::STRINTERN:
    case Op::STRRAW:
    case Op::STRCONV:
    case Op::STRRESERVE:
    case Op::STRLEN:
    case Op::STRCMP:
    case Op::STRGETBYTES:
    case Op::SPECIALIZE: return true;
    default:
        // STORE/SETFIELD/ARRAY_STORE/SETSTATIC/MEMCPY/*_FREE/ATOMIC_*/CALLN/
        // dinamicas/monitor/spawn/msg/future/DEF*/DL*/asm/... -> IMPURO.
        return false;
    }
}

} // namespace

FunctionFingerprint compute_fingerprint(const ir::IrFunction &fn,
                                        const std::string &arch) {
    FunctionFingerprint fp;
    fp.function = fn.name;
    fp.key = util::InternedName::from_interned(fn.name_key());

    fp.pure_local = true; // hasta encontrar un op impuro.
    /* El mismo recorrido lleva los DOS ejes.  Todo lo que rompe la pureza la
     * rompe en los dos, salvo la llamada a una nativa: ese es el unico caso que
     * se puede recuperar preguntando lo que se haya declarado de ella. */
    fp.pure_local_ignoring_natives = true;
    using Op = ir::IrOp;
    for (const auto &bb : fn.blocks) {
        for (const auto &ins : bb.instrs) {
            // Allocaciones (todo lo que pueda allocar).
            if (is_alloc_op(ins.op)) ++fp.alloc_sites;
            if (ins.op == Op::MAKE_CLOSURE && (ins.imm & 0x1u))
                ++fp.alloc_sites; // env GC_HEAP.

            switch (ins.op) {
            case Op::ALLOCA:
                fp.stack_bytes += ins.imm * ir::type_storage_bytes(ins.type);
                break;
            case Op::THROW:
            case Op::RETHROW: fp.throws = true; break;
            case Op::PANIC: fp.panics = true; break;
            case Op::CALL:
            case Op::TAILCALL:
            case Op::CALLN:
                // Callgraph estatico (CALLN externo no resolvera -> conservador
                // en compose).  Neutro para la pureza LOCAL (se compone).
                if (!ins.func_name.empty()) {
                    fp.calls.push_back(ins.func_name);
                    if (ins.func_name == fn.name) fp.self_recursive = true;
                }
                // CALLN es nativo: efecto local impuro.
                if (ins.op == Op::CALLN) fp.pure_local = false;
                break;
            case Op::CALLVIRT:
            case Op::CALLM:
            case Op::CALLITF:
            case Op::CALLCLOSURE:
            case Op::CALLIND:
                fp.has_dynamic_call = true;
                fp.pure_local = false; // efecto opaco.
                fp.pure_local_ignoring_natives = false;
                break;
            case Op::INLINE_ASM: {
                // `asm { }` nativo: se ANALIZA el cuerpo (efectos exactos) en
                // vez de tratarlo como caja negra total.  El cuerpo NASM viaja
                // en
                // @c func_name (lo pone el lowering de  AS).
                /* Sin clases a proposito: de este bloque solo se preguntan
                 * el marco explicito y la pureza, y ninguna de las dos depende
                 * de QUE memoria se toca.  El nombre lo dice para que se vea
                 * que es una eleccion y no un olvido. */
                const vx::AsmBlockEffects e =
                    vx::asm_analyze_block_no_classes(ins.func_name, arch);
                // El marco EXPLICITO (push/pop/sub rsp con inmediato) SI se ve
                // en el texto -> se cuenta en el parcial medido.
                if (e.explicit_stack_bytes > 0)
                    fp.stack_bytes +=
                        static_cast<uint64_t>(e.explicit_stack_bytes);
                // Pureza AFINADA: un asm que no toca memoria, no llama y no es
                // atomico conserva la pureza local (p.ej. popcnt/aritmetica
                // sobre registros).  Un mnemonico desconocido -> conservador
                // (impuro).
                if (e.touches_mem || e.is_call || e.has_atomic || !e.known()) {
                    fp.pure_local = false;
                    fp.pure_local_ignoring_natives = false;
                }
                // Un `call` externo dentro del asm hace el efecto no acotable.
                if (e.is_call) fp.has_dynamic_call = true;
                // El marco IMPLICITO de los enlaces register() (los spills y el
                // guardado de callee-saved clobbered que decide el backend) NO
                // es visible en el texto -> el TOTAL de los callers sigue
                // usando el
                // @stack DECLARADO.  Retirarlo requiere el reporte del backend.
                fp.frame_opaque = true;
                break;
            }
            default:
                // Cualquier op no-pura y no-CALL rompe la pureza local.
                if (!is_pure_op(ins.op)) {
                    fp.pure_local = false;
                    fp.pure_local_ignoring_natives = false;
                }
                break;
            }
        }
    }
    // Totales por defecto = valores locales (se recalculan en compose).
    fp.alloc_sites_total = fp.alloc_sites;
    fp.stack_bytes_total = fp.stack_bytes;
    fp.throws_total = fp.throws;
    fp.panics_total = fp.panics;
    fp.recursive = fp.self_recursive;
    fp.effects_known = !fp.has_dynamic_call;
    fp.pure = fp.pure_local && fp.effects_known && fp.calls.empty();
    return fp;
}

std::vector<FunctionFingerprint>
compute_module_fingerprints(const ir::IrModule &mod, const std::string &arch) {
    std::vector<FunctionFingerprint> out;
    out.reserve(mod.functions.size());
    for (const auto &fn : mod.functions)
        out.push_back(compute_fingerprint(fn, arch));
    return out;
}

void compose_fingerprints(
    std::vector<FunctionFingerprint> &fps,
    const FunctionContractMap *contracts, const ir::IrModule *mod) {
    const size_t n = fps.size();
    if (n == 0) return;
    std::unordered_map<std::string, uint32_t> idx;
    idx.reserve(n * 2 + 1);
    for (uint32_t i = 0; i < n; ++i)
        idx.emplace(fps[i].function, i);

    // Marco de pila PROPIO a efectos del TOTAL: normalmente el medido
    // (`stack_bytes`), pero para una fn de marco OPACO (`asm { }`, cuyo frame
    // no se ve en el IR) se usa su @stack DECLARADO -- asi el total de sus
    // callers refleja la pila real de la primitiva de asm.  No toca el parcial
    // medido (la verificacion del parcial sigue siendo cota superior sobre 0).
    auto frame_para_total = [&](uint32_t v) -> uint64_t {
        const auto &f = fps[v];
        if (f.frame_opaque && contracts) {
            // El contrato va con el simbolo con el que la funcion baja, asi
            // que se busca por el nombre exacto de la huella.
            const auto it = contracts->find(f.key);
            if (it != contracts->end()) {
                const FunctionContracts *c = &it->second;
                if (c->stack_partial >= 0)
                    return static_cast<uint64_t>(c->stack_partial);
                if (c->stack_total >= 0)
                    return static_cast<uint64_t>(c->stack_total);
            }
        }
        return f.stack_bytes;
    };

    // Para cada funcion, DFS del cierre transitivo por el callgraph estatico.
    // O(F*(V+E)) -- aceptable para tamanos de modulo tipicos.
    std::vector<char> visited(n, 0);
    std::vector<uint32_t> stack;
    stack.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        std::fill(visited.begin(), visited.end(), 0);
        stack.clear();
        stack.push_back(i);
        uint32_t alloc_total = 0;
        bool throws_t = false, panics_t = false, recursive = false;
        bool known = true, all_pure_local = true;
        /* Quien lo hace opaco.  Se queda el PRIMERO que se encuentra: decir
         * uno concreto es accionable, y una lista de doce solo abruma -- si al
         * declarar ese sigue habiendo mas, el aviso vuelve a salir con el
         * siguiente, que es como se arregla una cadena. */
        std::string opaque_callee;
        bool opaque_dynamic = false;
        while (!stack.empty()) {
            const uint32_t v = stack.back();
            stack.pop_back();
            if (visited[v]) continue;
            visited[v] = 1;
            const auto &f = fps[v];
            alloc_total += f.alloc_sites;
            throws_t = throws_t || f.throws;
            panics_t = panics_t || f.panics;
            /* Con el modulo delante, las nativas dejan de ser un motivo de
             * impureza POR SI SOLAS: son aristas del grafo como cualquier otra
             * y se resuelven abajo, consultando lo declarado.  Sin modulo se
             * usa el eje conservador, que es el de siempre.
             *
             * La seguridad no depende de esta eleccion: una nativa sin declarar
             * pone `known` en falso mas abajo, y `pure` exige las dos cosas. */
            all_pure_local =
                all_pure_local &&
                (mod != nullptr ? f.pure_local_ignoring_natives : f.pure_local);
            if (f.has_dynamic_call) {
                known = false; // efecto opaco alcanzable.
                opaque_dynamic = true;
            }
            for (const auto &callee : f.calls) {
                auto it = idx.find(callee);
                if (it == idx.end()) {
                    /* No esta en el programa.  Antes eso bastaba para dar el
                     * cierre por opaco; ahora se PREGUNTA primero si alguien
                     * dijo lo que hace.
                     *
                     * Lo declarado se aporta como lo que es -- una afirmacion
                     * de quien importa la funcion --, y el cierre sigue siendo
                     * conocido.  Cada eje que la declaracion NO cubre se queda
                     * en su valor conservador y no en el permisivo: un campo
                     * que nadie puso vale `false`, y `false` aqui significa
                     * "no aporta ese efecto", asi que solo puede aprobar de mas
                     * si el que declara MINTIO -- que es su responsabilidad, no
                     * un descuido nuestro. */
                    const ir::IrNativeEffects *d =
                        mod != nullptr ? mod->native_effects_of(callee)
                                       : nullptr;
                    if (d == nullptr) {
                        known = false; // nadie ha dicho nada -> conservador.
                        if (opaque_callee.empty()) opaque_callee = callee;
                        continue;
                    }
                    throws_t = throws_t || d->may_throw;
                    panics_t = panics_t || d->may_panic;
                    if (d->allocates) ++alloc_total;
                    /* La pureza cae con cualquier efecto de dato observable.
                     * Una nativa que solo lee sus argumentos y no toca nada mas
                     * SI puede ser pura, que es justo lo que permite demostrar
                     * un `@pure` sobre codigo que llama a `strlen`. */
                    if (d->writes_pointee != 0 || d->writes_global ||
                        d->reads_global || d->io || d->nondeterministic)
                        all_pure_local = false;
                    continue;
                }
                const uint32_t j = it->second;
                if (j == i)
                    recursive = true; // arista de vuelta al inicio -> ciclo.
                if (!visited[j]) stack.push_back(j);
            }
        }
        fps[i].alloc_sites_total = alloc_total;
        fps[i].recursive = recursive || fps[i].self_recursive;
        fps[i].effects_known = known;
        fps[i].opaque_callee = std::move(opaque_callee);
        fps[i].opaque_dynamic = opaque_dynamic;
        // Si no conocemos todos los efectos alcanzables, los totales de efecto
        // se vuelven CONSERVADORES: no podemos PROBAR la ausencia.
        fps[i].throws_total = known ? throws_t : true;
        fps[i].panics_total = known ? panics_t : true;
        // Pura sii TODA funcion alcanzable es localmente pura Y conocemos todos
        // los efectos (sin dinamica ni externos no resueltos).
        fps[i].pure = all_pure_local && known;
    }

    // stack_bytes_total = profundidad de pila PEOR CASO = frame propio + el
    // MAXIMO de los callees (la cadena de llamadas mas honda), NO la suma del
    // conjunto alcanzable (a diferencia de alloc_total): la pila se libera al
    // volver, asi que solo importa el camino mas profundo.  Un ciclo del
    // callgraph (recursion) o un callee externo hacen la profundidad NO
    // acotable -> sentinela STACK_UNBOUNDED, que verify trata como
    // inverificable.  DFS post-orden ITERATIVO (pila explicita, NO recursion
    // de C++: un modulo con una cadena de llamadas muy honda -- p.ej. codigo
    // generado en comptime -- desbordaria la pila del proceso).  Gris (en
    // pila) = deteccion de ciclo (arista de vuelta).
    std::vector<uint64_t> memo(n, 0);
    std::vector<char> st(n, 0); // 0=blanco, 1=gris (en pila), 2=negro (hecho)
    // Cada marco: (nodo, fase).  fase 0 = ENTRAR (marcar gris + apilar hijos);
    // fase 1 = SALIR (todos los hijos hechos -> componer el maximo).
    std::vector<std::pair<uint32_t, uint8_t>> dfs;
    dfs.reserve(n);
    for (uint32_t s = 0; s < n; ++s) {
        if (st[s] != 0) continue;
        dfs.push_back({s, 0});
        while (!dfs.empty()) {
            const uint32_t v = dfs.back().first;
            const uint8_t fase = dfs.back().second;
            if (fase == 0) {
                if (st[v] == 2) {
                    dfs.pop_back();
                    continue;
                }
                st[v] = 1;             // gris (en pila)
                dfs.back().second = 1; // al desapilar, componer
                for (const auto &callee : fps[v].calls) {
                    auto it = idx.find(callee);
                    if (it == idx.end()) continue; // externo -> se ve en SALIR
                    const uint32_t j = it->second;
                    if (st[j] == 0) dfs.push_back({j, 0});
                }
            } else {
                dfs.pop_back();
                uint64_t best = 0; // maximo de los callees
                for (const auto &callee : fps[v].calls) {
                    auto it = idx.find(callee);
                    if (it == idx.end()) {
                        best = STACK_UNBOUNDED;
                        break;
                    }
                    const uint32_t j = it->second;
                    // Callee gris = arista de vuelta (ciclo); negro = hecho.
                    const uint64_t d = (st[j] == 1) ? STACK_UNBOUNDED : memo[j];
                    if (d == STACK_UNBOUNDED) {
                        best = STACK_UNBOUNDED;
                        break;
                    }
                    if (d > best) best = d;
                }
                memo[v] = (best == STACK_UNBOUNDED)
                              ? STACK_UNBOUNDED
                              : frame_para_total(v) + best;
                st[v] = 2; // negro (hecho)
            }
        }
    }
    for (uint32_t i = 0; i < n; ++i)
        fps[i].stack_bytes_total = memo[i];
}

namespace {

/* Los contratos por su nombre en el fuente: es sintaxis del lenguaje, igual
 * en todos los idiomas, y cada uno se usaba en varios sitios. */
constexpr const char *kPure = "@pure";
constexpr const char *kNothrow = "@nothrow";
constexpr const char *kNopanic = "@nopanic";
constexpr const char *kAlloc = "@alloc";
constexpr const char *kStack = "@stack";
constexpr const char *kPod = "@pod";
constexpr const char *kNoHeap = "@no_heap";
constexpr const char *kSize = "@size";

/**
 * @brief Los contratos que una funcion declara, por su nombre.
 * @param c Los contratos.
 * @return Sus nombres, en orden fijo.
 */
std::vector<const char *> declared_contracts(const FunctionContracts &c) {
    std::vector<const char *> out;
    if (c.pure) out.push_back(kPure);
    if (c.nothrow) out.push_back(kNothrow);
    if (c.nopanic) out.push_back(kNopanic);
    if (c.alloc_partial >= 0 || c.alloc_total >= 0) out.push_back(kAlloc);
    if (c.stack_partial >= 0 || c.stack_total >= 0) out.push_back(kStack);
    return out;
}

/**
 * @brief Apunta un veredicto.
 * @param out      Donde.
 * @param function La funcion.
 * @param contract El contrato, por su nombre.
 * @param status   El veredicto.
 * @param detail   Por que, ya en el idioma activo.
 */
void add_check(std::vector<ContractCheck> &out, util::InternedName function,
               const char *contract, ContractCheck::Status status,
               std::string detail) {
    ContractCheck ck;
    ck.function = function;
    ck.contract = contract;
    ck.status = status;
    ck.detail = std::move(detail);
    out.push_back(std::move(ck));
}

/**
 * @brief Un numero como argumento de un mensaje del catalogo.
 * @param v El numero.
 * @return Su texto.
 */
std::string arg(uint64_t v) { return std::to_string(v); }

/**
 * @brief Verifica los contratos de UNA funcion contra su huella.
 *
 * SOUND/ASIMETRICO: VIOLATED solo cuando la violacion es demostrable; si los
 * efectos no se conocen del todo, UNVERIFIABLE.
 *
 * @param out  Donde se apuntan los veredictos.
 * @param name La funcion.
 * @param c    Lo que declara.
 * @param fp   Su huella, ya compuesta.
 */
void check_function(std::vector<ContractCheck> &out, util::InternedName name,
                    const FunctionContracts &c, const FunctionFingerprint &fp) {
    using St = ContractCheck::Status;
    // @pure: probado puro -> OK; probado impuro (efectos conocidos) ->
    // VIOLATED; si no se conocen los efectos -> UNVERIFIABLE.
    if (c.pure) {
        if (fp.pure)
            add_check(out, name, kPure, St::OK, vx::diag::format("VXT100"));
        else if (fp.effects_known)
            add_check(out, name, kPure, St::VIOLATED,
                      vx::diag::format("VXT101"));
        else
            add_check(out, name, kPure, St::UNVERIFIABLE,
                      vx::diag::format("VXT102"));
    }
    if (c.nothrow) {
        if (fp.effects_known && !fp.throws_total)
            add_check(out, name, kNothrow, St::OK, vx::diag::format("VXT103"));
        else if (fp.effects_known && fp.throws_total)
            add_check(out, name, kNothrow, St::VIOLATED,
                      vx::diag::format("VXT104"));
        else
            add_check(out, name, kNothrow, St::UNVERIFIABLE,
                      vx::diag::format("VXT105"));
    }
    if (c.nopanic) {
        if (fp.effects_known && !fp.panics_total)
            add_check(out, name, kNopanic, St::OK, vx::diag::format("VXT106"));
        else if (fp.effects_known && fp.panics_total)
            add_check(out, name, kNopanic, St::VIOLATED,
                      vx::diag::format("VXT107"));
        else
            add_check(out, name, kNopanic, St::UNVERIFIABLE,
                      vx::diag::format("VXT105"));
    }
    // @alloc: PARCIAL = sitios PROPIOS (exacto); TOTAL = cierre alcanzable
    // (conservador si hay efectos desconocidos).  Se declara cualquiera de las
    // dos (o ambas).  La forma corta `@alloc(N)` fija el TOTAL.
    if (c.alloc_partial >= 0) {
        const uint64_t got = fp.alloc_sites;
        const uint64_t want = static_cast<uint64_t>(c.alloc_partial);
        add_check(out, name, kAlloc, got > want ? St::VIOLATED : St::OK,
                  vx::diag::format("VXT108", {arg(want), arg(got)}));
    }
    if (c.alloc_total >= 0) {
        const uint64_t got = fp.alloc_sites_total;
        const uint64_t want = static_cast<uint64_t>(c.alloc_total);
        std::string d = vx::diag::format("VXT109", {arg(want), arg(got)});
        if (got > want)
            add_check(out, name, kAlloc, St::VIOLATED, std::move(d));
        else if (fp.effects_known)
            add_check(out, name, kAlloc, St::OK, std::move(d));
        else
            add_check(out, name, kAlloc, St::UNVERIFIABLE,
                      vx::diag::format("VXT110", {d}));
    }
    // @stack: PARCIAL = marco PROPIO (exacto, siempre verificable); TOTAL =
    // profundidad de pila peor caso del arbol de llamadas.  Si el total no es
    // acotable (recursion o llamada externa) queda INVERIFICABLE.  La forma
    // corta `@stack(N)` es el TOTAL.
    if (c.stack_partial >= 0) {
        const uint64_t got = fp.stack_bytes;
        const uint64_t want = static_cast<uint64_t>(c.stack_partial);
        add_check(out, name, kStack, got > want ? St::VIOLATED : St::OK,
                  vx::diag::format("VXT111", {arg(want), arg(got)}));
    }
    if (c.stack_total >= 0) {
        const uint64_t got = fp.stack_bytes_total;
        const uint64_t want = static_cast<uint64_t>(c.stack_total);
        if (got == STACK_UNBOUNDED)
            add_check(out, name, kStack, St::UNVERIFIABLE,
                      vx::diag::format("VXT112"));
        else
            add_check(out, name, kStack, got > want ? St::VIOLATED : St::OK,
                      vx::diag::format("VXT113", {arg(want), arg(got)}));
    }
}

} // namespace

std::vector<ContractCheck> verify_contracts(
    const std::vector<FunctionFingerprint> &fps,
    const FunctionContractMap &contracts) {
    std::vector<ContractCheck> out;
    if (contracts.empty()) return out;
    /* Indice por el simbolo exacto.  El contrato se recogio con el mismo
     * simbolo con el que la funcion baja (ver `collect_function_contracts`),
     * asi que no hace falta adivinar: ni el nombre simple de una funcion con
     * namespace, ni el sufijo de un metodo.  Aquellas heuristicas no
     * encontraban el metodo SOBRECARGADO -- baja con su discriminante detras
     * -- y el contrato se descartaba sin decir nada.  Cada instanciacion de
     * una plantilla es una funcion con su propio simbolo y su propia copia
     * del contrato, asi que se verifica por separado sin caso especial. */
    util::NamedMap<util::InternedName, const FunctionFingerprint *,
                   scratch::FunctionContractMap, util::InternedNameHash>
        byname;
    byname.reserve(fps.size() + 1);
    for (const auto &f : fps)
        byname.emplace(f.key, &f);

    using St = ContractCheck::Status;
    for (const auto &kv : contracts) {
        const util::InternedName name = kv.first;
        const FunctionContracts &c = kv.second;
        if (!c.any()) continue;

        const size_t first = out.size();
        const auto it = byname.find(kv.first);
        if (it == byname.end()) {
            /* Se verifica contra el intermedio PREVIO a optimizar, donde toda
             * funcion declarada existe: que no este es que la recogida y el
             * bajado no se ponen de acuerdo en el simbolo.  Callarlo daria el
             * contrato por comprobado, asi que cada contrato declarado se dice
             * indecidible, con su nombre. */
            const std::string why = vx::diag::format("VXT121");
            for (const char *declared : declared_contracts(c))
                add_check(out, name, declared, St::UNVERIFIABLE, why);
        } else {
            check_function(out, name, c, *it->second);
        }
        // Los veredictos de esta funcion llevan como se ensena y donde esta.
        for (size_t k = first; k < out.size(); ++k) {
            out[k].shown = c.shown;
            out[k].where = c.where;
        }
    }
    return out;
}

std::vector<ContractCheck> verify_type_contracts(
    const std::vector<TypeFingerprint> &fps,
    const std::unordered_map<std::string, TypeContracts> &contracts) {
    std::vector<ContractCheck> out;
    if (contracts.empty()) return out;
    // Indice por nombre de tipo (el nombre del contrato es el nombre
    // declarado).
    std::unordered_map<std::string, const TypeFingerprint *> byname;
    byname.reserve(fps.size() * 2 + 1);
    for (const auto &f : fps)
        byname.emplace(f.type_name, &f);

    using St = ContractCheck::Status;
    for (const auto &kv : contracts) {
        const std::string &name = kv.first;
        const TypeContracts &c = kv.second;
        if (!c.any()) continue;
        auto it = byname.find(name);
        if (it == byname.end()) continue; // el tipo no llego al layout.
        const TypeFingerprint &fp = *it->second;
        // Una vez por tipo con contratos: los veredictos llevan el nombre
        // internado, no una copia por veredicto.
        const util::InternedName type_key = util::InternedName::intern(name);

        // @pod: tipo por valor trivialmente copiable (sin destructor ni campos
        // gestionados).  Decidible del layout -> OK / VIOLATED (nunca
        // UNVERIFIABLE).
        if (c.pod) {
            if (fp.is_pod) {
                add_check(out, type_key, kPod, St::OK,
                          vx::diag::format("VXT114"));
            } else {
                const char *why = fp.is_reference     ? "VXT115"
                                  : fp.has_destructor ? "VXT116"
                                                      : "VXT117";
                add_check(out, type_key, kPod, St::VIOLATED,
                          vx::diag::format(why));
            }
        }
        // @no_heap: ningun campo referencia el heap gestionado.
        if (c.no_heap) {
            add_check(out, type_key, kNoHeap,
                      fp.no_heap ? St::OK : St::VIOLATED,
                      vx::diag::format(fp.no_heap ? "VXT118" : "VXT119"));
        }
        // @size(N): tamano EXACTO (estabilidad de ABI).  Decidible del layout.
        if (c.size >= 0) {
            const uint64_t got = fp.size_bytes;
            const uint64_t want = static_cast<uint64_t>(c.size);
            add_check(out, type_key, kSize, got == want ? St::OK : St::VIOLATED,
                      vx::diag::format("VXT120", {arg(want), arg(got)}));
        }
    }
    return out;
}

size_t report_native_effect_conflicts(const ir::IrModule &mod,
                                      const std::string &file,
                                      vx::Diagnostics &diags) {
    size_t n = 0;
    for (const ir::IrNativeImport &ni : mod.native_imports) {
        if (!ni.effects_conflict) continue;
        ++n;
        // Se nombra QUE eje choca, no solo que hay choque: "difieren" sin decir
        // en que obliga a ir a leer las dos declaraciones para saber si importa
        // -- y muchas veces no importa.
        std::string ejes;
        const ir::IrNativeEffects &e = ni.effects;
        auto anota = [&ejes](const char *nombre) {
            if (!ejes.empty()) ejes += ", ";
            ejes += nombre;
        };
        // Se listan los ejes que la union acabo ATRIBUYENDO: son exactamente
        // los que alguna de las dos afirmo y por los que el llamante paga.
        if (e.writes_pointee != 0) anota("writes_pointee");
        if (e.reads_pointee != 0) anota("reads_pointee");
        if (e.writes_global) anota("writes_global");
        if (e.reads_global) anota("reads_global");
        if (e.io) anota("io");
        if (e.may_throw) anota("may_throw");
        if (e.may_panic) anota("may_panic");
        if (e.allocates) anota("allocates");
        if (e.nondeterministic) anota("nondeterministic");
        diags.diag(vx::SourceLoc{}, vx::DiagLevel::WARN, "VXT008",
                   {ni.lib, ni.name, ejes});
    }
    (void)file;
    return n;
}

ContractReport report_contract_checks(const std::vector<ContractCheck> &checks,
                                      const std::string &file,
                                      vx::Diagnostics &diags) {
    ContractReport r;
    for (const ContractCheck &ck : checks) {
        /* Donde se declaro, si se sabe; si no, el fichero. */
        vx::SourceLoc loc = ck.where;
        if (loc.file().empty()) loc.set_file(file);
        /* El simbolo IDENTIFICA la funcion; al usuario se le ensena lo que
         * escribio.  Si quien recogio el contrato no lo dejo apuntado, la
         * inversa del aplanado es lo mejor que se puede reconstruir. */
        const std::string shown = ck.shown.empty()
                                      ? vx::demangle_symbol(ck.function.str())
                                      : ck.shown.str();
        switch (ck.status) {
        case ContractCheck::VIOLATED:
            /* Demostrado que no se cumple: es un error del programa, y por eso
             * aborta la construccion.  El llamante decide -- en `--analyze` se
             * mide y se ensena, no se construye. */
            ++r.violated;
            diags.diag(loc, vx::DiagLevel::ERR, "VXT004",
                       {shown, ck.contract, ck.detail});
            break;
        case ContractCheck::UNVERIFIABLE:
            /* Y este es el que se descartaba en silencio.  NO es un error: el
             * programa puede estar perfectamente bien y ser el ANALISIS el que
             * no llega.  Pero callarselo deja un contrato que nadie comprueba y
             * que parece comprobado, asi que se dice -- con el motivo dentro,
             * que es lo que lo vuelve accionable. */
            ++r.unverified;
            diags.diag(loc, vx::DiagLevel::WARN, "VXW001",
                       {shown, ck.contract, ck.detail});
            break;
        case ContractCheck::OK:
        default:
            /* Se cumple y esta demostrado: no hay nada que decir.  Decirlo
             * seria ruido en cada compilacion, y el sitio donde SI se ensena es
             * el volcado del ASA, que para eso ensena tambien lo verde. */
            break;
        }
    }
    return r;
}

} // namespace analyze
