/**
 * @file fingerprint.cpp
 * @brief Implementacion de la huella computacional por-funcion (ver
 *        analyze/fingerprint.h).
 */
#include "analyze/fingerprint.h"

#include <unordered_map>
#include <unordered_set>

#include "ir/ssa_ir.h"

namespace analyze {

namespace {

/// Bytes de un @c IrType escalar (para el tamano de un ALLOCA).
uint64_t type_size_bytes(ir::IrType t) {
    switch (t) {
    case ir::IrType::I8:
    case ir::IrType::U8:
    case ir::IrType::BOOL:
        return 1;
    case ir::IrType::I16:
    case ir::IrType::U16:
        return 2;
    case ir::IrType::I32:
    case ir::IrType::U32:
    case ir::IrType::F32:
    case ir::IrType::HANDLE:
        return 4;
    case ir::IrType::I64:
    case ir::IrType::U64:
    case ir::IrType::F64:
    case ir::IrType::PTR:
        return 8;
    case ir::IrType::VOID:
    default:
        return 0;
    }
}

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
    case Op::STRINTERN:
        return true;
    default:
        return false;
    }
}

/// @c true si @p op NO tiene efectos de dato observables (whitelist).  Lo que
/// no esta aqui (ni es CALL/TAILCALL estatico, que se compone) se considera
/// IMPURO -> soundness: un op nuevo/desconocido bloquea @c @pure hasta anadirlo.
/// THROW/PANIC son efectos de CONTROL (se rastrean aparte), no de dato -> puros.
/// Las allocaciones son puras (una funcion pura puede construir su retorno).
bool is_pure_op(ir::IrOp op) {
    using Op = ir::IrOp;
    switch (op) {
    // Constantes / movimientos / direcciones.
    case Op::CONST: case Op::MOV: case Op::NOP:
    case Op::STR_LIT_ADDR: case Op::LABEL_ADDR: case Op::SECTION_REF:
    // Aritmetica entera.
    case Op::ADD: case Op::SUB: case Op::MUL: case Op::DIV: case Op::MOD:
    case Op::NEG: case Op::IABS: case Op::IMIN: case Op::IMAX:
    case Op::IMINU: case Op::IMAXU: case Op::ILOG2:
    // Aritmetica float.
    case Op::FADD: case Op::FSUB: case Op::FMUL: case Op::FDIV:
    case Op::FNEG: case Op::FABS: case Op::FSQRT: case Op::FMIN: case Op::FMAX:
    case Op::FFLOOR: case Op::FCEIL: case Op::FROUND: case Op::FTRUNC:
    // Vector (compute + reduccion local).
    case Op::VEC_UNOP: case Op::VEC_BINOP: case Op::VEC_FMA:
    case Op::VEC_BINOP_S: case Op::VEC_BCAST:
    case Op::VEC_ACC_ZERO: case Op::VEC_ACC_ADD: case Op::VEC_ACC_FMA:
    case Op::VEC_ACC_STORE: case Op::VEC_ACC_COMBINE:
    // Bitwise.
    case Op::AND: case Op::OR: case Op::XOR: case Op::NOT:
    case Op::SHL: case Op::SHR: case Op::SAR:
    case Op::CLZ: case Op::CTZ: case Op::POPCNT: case Op::BYTESWAP:
    case Op::ROTL: case Op::ROTR:
    // Comparaciones.
    case Op::CMP_EQ: case Op::CMP_NE: case Op::CMP_LT: case Op::CMP_GT:
    case Op::CMP_LE: case Op::CMP_GE: case Op::CMP_ULT: case Op::CMP_UGT:
    case Op::CMP_ULE: case Op::CMP_UGE:
    case Op::FCMP_EQ: case Op::FCMP_NE: case Op::FCMP_LT: case Op::FCMP_GT:
    case Op::FCMP_LE: case Op::FCMP_GE:
    // Casts.
    case Op::CAST: case Op::ZEXT: case Op::SEXT: case Op::TRUNC:
    case Op::ITOF: case Op::UITOF: case Op::FTOI: case Op::FTOUI:
    case Op::F32TOF64: case Op::F64TOF32: case Op::BITCAST:
    // Control de flujo (los efectos de control se rastrean aparte).
    case Op::BR: case Op::BR_COND: case Op::RET: case Op::UNREACHABLE:
    case Op::PHI: case Op::SWITCH_DENSE: case Op::MATCH_VARIANT:
    case Op::THROW: case Op::RETHROW: case Op::PANIC:
    case Op::TRYENTER: case Op::TRYLEAVE: case Op::LANDINGPAD:
    // Lecturas (puras; para determinismo se afinara aparte).
    case Op::LOAD: case Op::GETFIELD: case Op::ARRAY_LOAD: case Op::ARRAY_LEN:
    case Op::GETSTATIC: case Op::GEP: case Op::GCDEREF_IR:
    case Op::GC_DEREF_HOST: case Op::GC_HANDLE_FOR_PTR:
    case Op::INSTANCEOF: case Op::CHECKCAST: case Op::ISNULL: case Op::UNWRAP:
    case Op::REFLECT_COUNT: case Op::REFLECT_AT:
    case Op::FINDCLASS: case Op::FINDMETHOD: case Op::FINDFIELD:
    case Op::SHARED_STAT: case Op::READ_VM_REG:
    case Op::GETPROC: case Op::GETVM: case Op::GETMGR:
    case Op::GETPID: case Op::GETARGC: case Op::GETARG:
    // Alloc local + construccion de valores (allocar es puro).
    case Op::ALLOCA:
    case Op::RAW_ALLOC: case Op::GC_ALLOC: case Op::GC_ALLOCP:
    case Op::NEWOBJ: case Op::NEWOBJS: case Op::ARRAY_ALLOC:
    case Op::MAKE_VARIANT: case Op::MAKE_CLOSURE:
    case Op::STRMAKE: case Op::STRCAT: case Op::STRSLICE: case Op::STRFLAT:
    case Op::STRHASH: case Op::STRINTERN: case Op::STRRAW: case Op::STRCONV:
    case Op::STRRESERVE: case Op::STRLEN: case Op::STRCMP: case Op::STRGETBYTES:
    case Op::SPECIALIZE:
        return true;
    default:
        // STORE/SETFIELD/ARRAY_STORE/SETSTATIC/MEMCPY/*_FREE/ATOMIC_*/CALLN/
        // dinamicas/monitor/spawn/msg/future/DEF*/DL*/asm/... -> IMPURO.
        return false;
    }
}

} // namespace

FunctionFingerprint compute_fingerprint(const ir::IrFunction &fn) {
    FunctionFingerprint fp;
    fp.function = fn.name;
    fp.pure_local = true; // hasta encontrar un op impuro.
    using Op = ir::IrOp;
    for (const auto &bb : fn.blocks) {
        for (const auto &ins : bb.instrs) {
            // Allocaciones (todo lo que pueda allocar).
            if (is_alloc_op(ins.op)) ++fp.alloc_sites;
            if (ins.op == Op::MAKE_CLOSURE && (ins.imm & 0x1u))
                ++fp.alloc_sites; // env GC_HEAP.

            switch (ins.op) {
            case Op::ALLOCA:
                fp.stack_bytes += ins.imm * type_size_bytes(ins.type);
                break;
            case Op::THROW:
            case Op::RETHROW:
                fp.throws = true;
                break;
            case Op::PANIC:
                fp.panics = true;
                break;
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
                break;
            default:
                // Cualquier op no-pura y no-CALL rompe la pureza local.
                if (!is_pure_op(ins.op)) fp.pure_local = false;
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
compute_module_fingerprints(const ir::IrModule &mod) {
    std::vector<FunctionFingerprint> out;
    out.reserve(mod.functions.size());
    for (const auto &fn : mod.functions)
        out.push_back(compute_fingerprint(fn));
    return out;
}

void compose_fingerprints(std::vector<FunctionFingerprint> &fps) {
    const size_t n = fps.size();
    if (n == 0) return;
    std::unordered_map<std::string, uint32_t> idx;
    idx.reserve(n * 2 + 1);
    for (uint32_t i = 0; i < n; ++i) idx.emplace(fps[i].function, i);

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
        while (!stack.empty()) {
            const uint32_t v = stack.back();
            stack.pop_back();
            if (visited[v]) continue;
            visited[v] = 1;
            const auto &f = fps[v];
            alloc_total += f.alloc_sites;
            throws_t = throws_t || f.throws;
            panics_t = panics_t || f.panics;
            all_pure_local = all_pure_local && f.pure_local;
            if (f.has_dynamic_call) known = false; // efecto opaco alcanzable.
            for (const auto &callee : f.calls) {
                auto it = idx.find(callee);
                if (it == idx.end()) {
                    known = false; // callee externo/no presente -> conservador.
                    continue;
                }
                const uint32_t j = it->second;
                if (j == i) recursive = true; // arista de vuelta al inicio -> ciclo.
                if (!visited[j]) stack.push_back(j);
            }
        }
        fps[i].alloc_sites_total = alloc_total;
        fps[i].recursive = recursive || fps[i].self_recursive;
        fps[i].effects_known = known;
        // Si no conocemos todos los efectos alcanzables, los totales de efecto
        // se vuelven CONSERVADORES: no podemos PROBAR la ausencia.
        fps[i].throws_total = known ? throws_t : true;
        fps[i].panics_total = known ? panics_t : true;
        // Pura sii TODA funcion alcanzable es localmente pura Y conocemos todos
        // los efectos (sin dinamica ni externos no resueltos).
        fps[i].pure = all_pure_local && known;
        // stack_bytes_total: por ahora el frame propio (la agregacion por
        // camino mas profundo es un refinamiento posterior).
        fps[i].stack_bytes_total = fps[i].stack_bytes;
    }
}

std::vector<ContractCheck> verify_contracts(
    const std::vector<FunctionFingerprint> &fps,
    const std::unordered_map<std::string, FunctionContracts> &contracts) {
    std::vector<ContractCheck> out;
    if (contracts.empty()) return out;
    // Nombre SIMPLE (ultimo segmento): el IR puede venir mangled por namespace
    // (ns__f) o cualificado (ns.f); el contrato se declara con el nombre simple.
    auto simple = [](const std::string &q) -> std::string {
        size_t p = q.rfind("__");
        if (p != std::string::npos) return q.substr(p + 2);
        p = q.rfind('.');
        return (p == std::string::npos) ? q : q.substr(p + 1);
    };
    // Indice por nombre COMPLETO y por nombre simple.  Los contratos de un
    // METODO se declaran como `Tipo__metodo` -- el nombre simple no vale como
    // clave porque dos tipos pueden tener un metodo homonimo --, mientras que
    // los de una funcion libre se declaran con el nombre simple aunque el IR la
    // traiga mangled por namespace.  Indexando por los dos, una sola busqueda
    // cubre ambos.  ANTES solo se indexaba el simple: `simple("S__mal")` da
    // "mal", la clave del contrato es "S__mal", la busqueda fallaba y el
    // `continue` se tragaba el contrato EN SILENCIO -- o sea que ningun
    // @pure/@alloc/@stack/@nothrow/@nopanic sobre un metodo se verificaba nunca.
    std::unordered_map<std::string, const FunctionFingerprint *> byname;
    byname.reserve(fps.size() * 4 + 1);
    for (const auto &f : fps) {
        byname.emplace(f.function, &f);
        byname.emplace(simple(f.function), &f);
    }

    using St = ContractCheck::Status;
    for (const auto &kv : contracts) {
        const std::string &name = kv.first;
        const FunctionContracts &c = kv.second;
        if (!c.any()) continue;
        const FunctionFingerprint *fpp = nullptr;
        auto it = byname.find(name);
        if (it != byname.end()) {
            fpp = it->second;
        } else {
            // Ni completo ni simple.  Queda el caso del metodo DENTRO de un
            // namespace: la clave es `Tipo__metodo` pero el IR lo trae como
            // `ns__Tipo__metodo`.  Buscamos por sufijo, y solo aceptamos si el
            // resultado es unico (si no, seria adivinar).
            const std::string suf = "__" + name;
            for (const auto &f : fps) {
                if (f.function.size() > suf.size() &&
                    f.function.compare(f.function.size() - suf.size(),
                                       suf.size(), suf) == 0) {
                    if (fpp) { fpp = nullptr; break; } // ambiguo: no adivinar
                    fpp = &f;
                }
            }
        }
        if (!fpp) continue; // la funcion no llego al IR (inline/DCE).
        const FunctionFingerprint &fp = *fpp;

        auto add = [&](const char *cn, St st, std::string detail) {
            out.push_back({name, cn, st, std::move(detail)});
        };

        // @pure: probado puro -> OK; probado impuro (efectos conocidos) ->
        // VIOLATED; si no se conocen los efectos -> UNVERIFIABLE.
        if (c.pure) {
            if (fp.pure)
                add("@pure", St::OK, "puro");
            else if (fp.effects_known)
                add("@pure", St::VIOLATED, "la funcion tiene efectos de dato");
            else
                add("@pure", St::UNVERIFIABLE,
                    "efectos desconocidos (llamada dinamica/externa)");
        }
        // @nothrow.
        if (c.nothrow) {
            if (fp.effects_known && !fp.throws_total)
                add("@nothrow", St::OK, "no lanza");
            else if (fp.effects_known && fp.throws_total)
                add("@nothrow", St::VIOLATED, "puede lanzar (throw alcanzable)");
            else
                add("@nothrow", St::UNVERIFIABLE, "efectos desconocidos");
        }
        // @nopanic.
        if (c.nopanic) {
            if (fp.effects_known && !fp.panics_total)
                add("@nopanic", St::OK, "no hace panic");
            else if (fp.effects_known && fp.panics_total)
                add("@nopanic", St::VIOLATED, "puede hacer panic");
            else
                add("@nopanic", St::UNVERIFIABLE, "efectos desconocidos");
        }
        // @alloc(N): si hay AL MENOS mas de N sitios conocidos -> VIOLATED.
        if (c.alloc >= 0) {
            const uint64_t got = fp.alloc_sites_total;
            const uint64_t want = static_cast<uint64_t>(c.alloc);
            std::string d = "esperado <=" + std::to_string(want) +
                            ", inferido " + std::to_string(got);
            if (got > want)
                add("@alloc", St::VIOLATED, std::move(d));
            else if (fp.effects_known)
                add("@alloc", St::OK, std::move(d));
            else
                add("@alloc", St::UNVERIFIABLE,
                    d + " (mas posibles: efectos desconocidos)");
        }
        // @stack(N): el frame PROPIO es exacto -> verificable siempre.
        if (c.stack >= 0) {
            const uint64_t got = fp.stack_bytes;
            const uint64_t want = static_cast<uint64_t>(c.stack);
            std::string d = "esperado <=" + std::to_string(want) + "B, inferido " +
                            std::to_string(got) + "B (frame propio)";
            add("@stack", got > want ? St::VIOLATED : St::OK, std::move(d));
        }
    }
    return out;
}

std::vector<ContractCheck> verify_type_contracts(
    const std::vector<TypeFingerprint> &fps,
    const std::unordered_map<std::string, TypeContracts> &contracts) {
    std::vector<ContractCheck> out;
    if (contracts.empty()) return out;
    // Indice por nombre de tipo (el nombre del contrato es el nombre declarado).
    std::unordered_map<std::string, const TypeFingerprint *> byname;
    byname.reserve(fps.size() * 2 + 1);
    for (const auto &f : fps) byname.emplace(f.type_name, &f);

    using St = ContractCheck::Status;
    for (const auto &kv : contracts) {
        const std::string &name = kv.first;
        const TypeContracts &c = kv.second;
        if (!c.any()) continue;
        auto it = byname.find(name);
        if (it == byname.end()) continue; // el tipo no llego al layout.
        const TypeFingerprint &fp = *it->second;

        auto add = [&](const char *cn, St st, std::string detail) {
            out.push_back({name, cn, st, std::move(detail)});
        };

        // @pod: value-type trivialmente copiable (sin dtor ni campos gestionados).
        // Decidible del layout -> OK / VIOLATED (nunca UNVERIFIABLE).
        if (c.pod) {
            if (fp.is_pod) {
                add("@pod", St::OK, "value-type trivialmente copiable");
            } else {
                std::string why =
                    fp.is_reference
                        ? "es un tipo por referencia (clase), no un value-type"
                    : fp.has_destructor
                        ? "tiene destructor (~Tipo) -> carril move-only"
                        : "tiene algun campo gestionado (heap/GC/smart-pointer)";
                add("@pod", St::VIOLATED, std::move(why));
            }
        }
        // @no_heap: ningun campo referencia el heap gestionado.
        if (c.no_heap) {
            add("@no_heap", fp.no_heap ? St::OK : St::VIOLATED,
                fp.no_heap ? "sin campos en el heap gestionado"
                           : "algun campo referencia el heap gestionado");
        }
        // @size(N): tamano EXACTO (estabilidad de ABI).  Decidible del layout.
        if (c.size >= 0) {
            const uint64_t got = fp.size_bytes;
            const uint64_t want = static_cast<uint64_t>(c.size);
            std::string d = "esperado " + std::to_string(want) + "B, inferido " +
                            std::to_string(got) + "B";
            add("@size", got == want ? St::OK : St::VIOLATED, std::move(d));
        }
    }
    return out;
}

} // namespace analyze
