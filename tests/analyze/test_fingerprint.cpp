/**
 * @file test_fingerprint.cpp
 * @brief Tests de la huella computacional por-funcion (recursos + efectos +
 *        composicion interprocedural).  Ver analyze/fingerprint.h.
 */
#include "analyze/fingerprint.h"
#include "ir/ssa_ir.h"

#include <cstdio>
#include <string>

using namespace analyze;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(cond)) {                                                          \
            ++g_fail;                                                           \
            std::printf("  FAIL: %s (linea %d)\n", (msg), __LINE__);            \
        }                                                                       \
    } while (0)

// Construye una funcion con un unico bloque conteniendo las instrucciones dadas.
static ir::IrFunction fn_with(const std::string &name,
                              std::vector<ir::IrInstr> instrs) {
    ir::IrFunction f;
    f.name = name;
    f.ret_type = ir::IrType::I64;
    ir::IrBlock bb;
    bb.name = "entry";
    bb.instrs = std::move(instrs);
    f.blocks.push_back(std::move(bb));
    return f;
}

static ir::IrInstr op(ir::IrOp o) {
    ir::IrInstr i{};
    i.op = o;
    return i;
}
static ir::IrInstr call(const std::string &name) {
    ir::IrInstr i{};
    i.op = ir::IrOp::CALL;
    i.func_name = name;
    return i;
}
static ir::IrInstr alloca_of(ir::IrType t, uint64_t count) {
    ir::IrInstr i{};
    i.op = ir::IrOp::ALLOCA;
    i.type = t;
    i.imm = count;
    return i;
}
static ir::IrInstr binop(ir::IrOp o) {
    ir::IrInstr i{};
    i.op = o;
    return i;
}

static const FunctionFingerprint *find(const std::vector<FunctionFingerprint> &v,
                                       const std::string &n) {
    for (const auto &f : v)
        if (f.function == n) return &f;
    return nullptr;
}

int main() {
    std::printf("=== test_fingerprint ===\n");

    ir::IrModule mod;
    mod.functions.push_back(fn_with("leaf", {op(ir::IrOp::RET)}));
    mod.functions.push_back(
        fn_with("allocs", {op(ir::IrOp::GC_ALLOC), op(ir::IrOp::RET)}));
    mod.functions.push_back(
        fn_with("thrower", {op(ir::IrOp::THROW), op(ir::IrOp::RET)}));
    mod.functions.push_back(
        fn_with("caller", {call("allocs"), call("thrower"), op(ir::IrOp::RET)}));
    mod.functions.push_back(fn_with("rec", {call("rec"), op(ir::IrOp::RET)}));
    mod.functions.push_back(
        fn_with("dynamic", {op(ir::IrOp::CALLVIRT), op(ir::IrOp::RET)}));
    mod.functions.push_back(fn_with(
        "stackuser", {alloca_of(ir::IrType::I64, 3), op(ir::IrOp::RET)}));
    // pure_math: solo aritmetica -> pura.
    mod.functions.push_back(fn_with(
        "pure_math", {binop(ir::IrOp::ADD), binop(ir::IrOp::MUL),
                      op(ir::IrOp::RET)}));
    // writer: tiene STORE -> impura local.
    mod.functions.push_back(
        fn_with("writer", {op(ir::IrOp::STORE), op(ir::IrOp::RET)}));
    // pure_caller: llama a pure_math -> pura por composicion.
    mod.functions.push_back(
        fn_with("pure_caller", {call("pure_math"), op(ir::IrOp::RET)}));
    // impure_caller: llama a writer -> impura por composicion.
    mod.functions.push_back(
        fn_with("impure_caller", {call("writer"), op(ir::IrOp::RET)}));

    auto fps = compute_module_fingerprints(mod);
    CHECK(fps.size() == 11, "11 huellas");

    // -- Locales -------------------------------------------------------------
    const auto *leaf = find(fps, "leaf");
    CHECK(leaf && leaf->alloc_sites == 0 && !leaf->throws && !leaf->panics,
          "leaf: sin alloc/throw/panic");
    const auto *allocs = find(fps, "allocs");
    CHECK(allocs && allocs->alloc_sites == 1, "allocs: 1 sitio de alloc");
    const auto *thrower = find(fps, "thrower");
    CHECK(thrower && thrower->throws, "thrower: throws local");
    const auto *stackuser = find(fps, "stackuser");
    CHECK(stackuser && stackuser->stack_bytes == 24,
          "stackuser: 3*i64 = 24 bytes de stack");
    const auto *rec0 = find(fps, "rec");
    CHECK(rec0 && rec0->self_recursive, "rec: self_recursive local");
    const auto *dyn0 = find(fps, "dynamic");
    CHECK(dyn0 && dyn0->has_dynamic_call, "dynamic: llamada dinamica");

    // -- Composicion interprocedural -----------------------------------------
    compose_fingerprints(fps);

    const auto *caller = find(fps, "caller");
    CHECK(caller && caller->alloc_sites_total == 1,
          "caller: alloc_sites_total=1 (via allocs)");
    CHECK(caller && caller->throws_total,
          "caller: throws_total=true (via thrower)");
    CHECK(caller && !caller->panics_total, "caller: panics_total=false");
    CHECK(caller && caller->effects_known,
          "caller: efectos conocidos (callees estaticos)");
    CHECK(caller && !caller->recursive, "caller: no recursivo");

    const auto *rec = find(fps, "rec");
    CHECK(rec && rec->recursive, "rec: recursive tras compose");

    const auto *dyn = find(fps, "dynamic");
    CHECK(dyn && !dyn->effects_known,
          "dynamic: effects_known=false (llamada opaca)");
    CHECK(dyn && dyn->throws_total,
          "dynamic: throws_total CONSERVADOR=true (no se puede probar ausencia)");

    const auto *leaf2 = find(fps, "leaf");
    CHECK(leaf2 && leaf2->alloc_sites_total == 0 && !leaf2->throws_total &&
              leaf2->effects_known,
          "leaf: probablemente @alloc(0) @nothrow (sound)");

    // -- Pureza (local + compuesta) ------------------------------------------
    const auto *pm = find(fps, "pure_math");
    CHECK(pm && pm->pure_local && pm->pure, "pure_math: pura");
    const auto *wr = find(fps, "writer");
    CHECK(wr && !wr->pure_local && !wr->pure, "writer: impura (STORE)");
    const auto *pc = find(fps, "pure_caller");
    CHECK(pc && pc->pure, "pure_caller: pura por composicion");
    const auto *ic = find(fps, "impure_caller");
    CHECK(ic && !ic->pure, "impure_caller: impura por composicion");
    const auto *dyn2 = find(fps, "dynamic");
    CHECK(dyn2 && !dyn2->pure, "dynamic: no se puede probar pura");
    const auto *allocs2 = find(fps, "allocs");
    CHECK(allocs2 && allocs2->pure,
          "allocs: allocar es puro (construye su retorno)");

    if (g_fail == 0)
        std::printf("=== test_fingerprint: %d checks OK, 0 fallidos ===\n",
                    g_checks);
    else
        std::printf("=== test_fingerprint: %d checks, %d FALLIDOS ===\n",
                    g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
