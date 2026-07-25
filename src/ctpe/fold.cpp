/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file fold.cpp
 * @brief Implementacion del pase de plegado CTPE (ver fold.h).
 */

#include "ctpe/fold.h"

#include "ctpe/evaluable.h"
#include "ir/ssa_ir_serialize.h"
#include "vx/comptime/comptime_vm.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ctpe {

namespace {

// FNV-1a 64: huella determinista del IR del modulo -> clave de cache.
uint64_t fnv1a(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

std::string cache_path_for(uint64_t key) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)key);
    return std::string(".cache/ctpe/") + buf + ".bin";
}

// Entradas cacheadas: nombre de fn -> (tipo de retorno, valor precomputado).
using CacheMap =
    std::unordered_map<std::string, std::pair<ir::IrType, uint64_t>>;

// Formato binario simple: [u32 count] { [u32 name_len][name][u8 type][u64 val] }.
void load_cache(uint64_t key, CacheMap &out) {
    std::ifstream f(cache_path_for(key), std::ios::binary);
    if (!f) return;
    uint32_t count = 0;
    f.read(reinterpret_cast<char *>(&count), 4);
    if (!f) return;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t nlen = 0;
        f.read(reinterpret_cast<char *>(&nlen), 4);
        if (!f || nlen > (1u << 20)) return;
        std::string name(nlen, '\0');
        f.read(&name[0], nlen);
        uint8_t ty = 0;
        uint64_t val = 0;
        f.read(reinterpret_cast<char *>(&ty), 1);
        f.read(reinterpret_cast<char *>(&val), 8);
        if (!f) return;
        out[name] = {static_cast<ir::IrType>(ty), val};
    }
}

void save_cache(uint64_t key, const CacheMap &m) {
    std::error_code ec;
    std::filesystem::create_directories(".cache/ctpe", ec); // mkdir -p portable.
    std::ofstream f(cache_path_for(key), std::ios::binary | std::ios::trunc);
    if (!f) return;
    uint32_t count = static_cast<uint32_t>(m.size());
    f.write(reinterpret_cast<const char *>(&count), 4);
    for (const auto &kv : m) {
        uint32_t nlen = static_cast<uint32_t>(kv.first.size());
        uint8_t ty = static_cast<uint8_t>(kv.second.first);
        uint64_t val = kv.second.second;
        f.write(reinterpret_cast<const char *>(&nlen), 4);
        f.write(kv.first.data(), nlen);
        f.write(reinterpret_cast<const char *>(&ty), 1);
        f.write(reinterpret_cast<const char *>(&val), 8);
    }
}

// Reescribe el cuerpo de @p fn a un unico bloque `%r = CONST value; ret %r`.
// La funcion es zero-param evaluable con retorno escalar; su computacion entera
// se sustituye por el valor precomputado.
void replace_body_with_const(ir::IrFunction &fn, uint64_t value, ir::IrType t) {
    fn.blocks.clear();
    fn.values.clear();
    fn.params.clear();

    const ir::IrValueId cst = fn.new_value(t);
    fn.values[cst].is_const = true;
    fn.values[cst].const_val = value;

    ir::IrBlock entry;
    entry.name = "entry_0";

    ir::IrInstr c{};
    c.op = ir::IrOp::CONST;
    c.type = t;
    c.dst = cst;
    c.imm = value;
    entry.instrs.push_back(std::move(c));

    ir::IrInstr r{};
    r.op = ir::IrOp::RET;
    r.type = t;
    r.operands.push_back(cst);
    entry.instrs.push_back(std::move(r));

    fn.blocks.push_back(std::move(entry));
}

} // namespace

int fold(ir::IrModule &mod, vx::ComptimeRuntime &rt, const FoldBudget &budget) {
    const Evaluability ev = compute_evaluability(mod);
    const std::vector<Candidate> cands = find_candidates(mod, ev);
    if (cands.empty()) return 0;

    // Clave de cache: huella del IR del modulo ANTES de plegar (determinista).
    const std::vector<uint8_t> irbytes = ir::emit_ir_module_cache(mod);
    const uint64_t key = fnv1a(irbytes.data(), irbytes.size());
    CacheMap cache;
    load_cache(key, cache);

    std::unordered_map<std::string, ir::IrFunction *> by_name;
    for (auto &fn : mod.functions) by_name[fn.name] = &fn;

    vx::ComptimeRuntime::CtpeBudget b;
    b.millis = budget.millis;
    b.max_heap_bytes = budget.max_heap_bytes;

    int folded = 0;
    bool dirty = false;
    for (const auto &cand : cands) {
        auto fit = by_name.find(cand.fn);
        if (fit == by_name.end()) continue;

        uint64_t value = 0;
        auto cit = cache.find(cand.fn);
        if (cit != cache.end() && cit->second.first == cand.ret_type) {
            value = cit->second.second; // cache hit: mismo IR -> mismo resultado.
        } else {
            uint64_t r0 = 0;
            // Ejecuta la funcion ENTERA en modo CTPE restringido (sandbox +
            // budget).  Si aborta (trap/timeout/oom) -> NO plegar (fallback).
            if (!rt.try_invoke_ctpe(cand.fn, {}, b, r0)) continue;
            value = r0;
            cache[cand.fn] = {cand.ret_type, value};
            dirty = true;
        }

        replace_body_with_const(*fit->second, value, cand.ret_type);
        ++folded;
    }

    if (dirty) save_cache(key, cache);
    return folded;
}

} // namespace ctpe
