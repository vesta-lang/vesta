/**
 * @file instr_db_test.cpp
 * @brief Tests del emparejador texto->FormID sobre la DB de instrucciones
 *        embebida (ver vx/instr_db.h).
 */
#include "vx/instr_db.h"

#include <cstdio>

using namespace vx::instr_db;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(cond)) {                                                          \
            ++g_fail;                                                           \
            std::printf("  FAIL: %s (linea %d)\n", (msg), __LINE__);            \
        }                                                                       \
    } while (0)

static ParsedOp reg(uint16_t w) { return ParsedOp{OP_REG, w}; }
static ParsedOp mem(uint16_t w) { return ParsedOp{OP_MEM, w}; }

int main() {
    std::printf("=== instr_db_test ===\n");

    // La DB x86 se cargo (tablas embebidas, sin archivos externos).
    CHECK(form_count(Isa::X86) == 22252, "x86: 22252 formas embebidas");

    // add reg64, reg64 -> una forma ADD.
    int32_t add = match(Isa::X86, "add", {reg(64), reg(64)});
    CHECK(add >= 0, "match add reg64,reg64");
    CHECK(std::string(iclass_name(Isa::X86, add)) == "ADD",
          "add -> iclass ADD");

    // minuscula/mayuscula indiferente.
    CHECK(match(Isa::X86, "ADD", {reg(64), reg(64)}) == add,
          "ADD == add (case-insensitive)");

    // mov reg32, mem32 casa una forma distinta (aridad/tipos).
    int32_t mov = match(Isa::X86, "mov", {reg(32), mem(32)});
    CHECK(mov >= 0 && std::string(iclass_name(Isa::X86, mov)) == "MOV",
          "match mov reg32,[mem32]");

    // mfence: barrera/serializante (overlay derivado).
    int32_t mf = match(Isa::X86, "mfence", {});
    CHECK(mf >= 0, "match mfence");
    uint16_t ov = overlay_of(Isa::X86, mf);
    CHECK(ov & (OVL_BARRIER | OVL_SERIALIZING | OVL_MEM_SEQ_CST),
          "mfence: overlay barrera/serializante");

    // syscall: overlay syscall.
    int32_t sc = match(Isa::X86, "syscall", {});
    CHECK(sc >= 0 && (overlay_of(Isa::X86, sc) & OVL_SYSCALL),
          "syscall: overlay syscall");

    // mnemonico inexistente -> -1.
    CHECK(match(Isa::X86, "frobnicate", {}) < 0,
          "mnemonico inexistente -> -1");

    // --- ARM64 (AArch64) ---
    CHECK(form_count(Isa::ARM64) == 4619, "arm64: 4619 formas embebidas");
    int32_t aadd = match(Isa::ARM64, "add", {reg(0), reg(0), reg(0)});
    CHECK(aadd >= 0 && std::string(iclass_name(Isa::ARM64, aadd)) == "ADD",
          "arm64: match add x,x,x");
    // ldaxr (LL/SC) -> overlay ll_sc; dmb -> barrera.
    int32_t ldaxr = match(Isa::ARM64, "ldaxr", {reg(0), mem(0)});
    CHECK(ldaxr >= 0 && (overlay_of(Isa::ARM64, ldaxr) & OVL_LL_SC),
          "arm64: ldaxr overlay ll_sc");
    int32_t dmb = match(Isa::ARM64, "dmb", {});
    CHECK(dmb >= 0 && (overlay_of(Isa::ARM64, dmb) & OVL_BARRIER),
          "arm64: dmb overlay barrera");

    // --- RISC-V ---
    CHECK(form_count(Isa::RISCV) == 1867, "riscv: 1867 formas embebidas");
    // amoadd.w -> overlay atomic.
    int32_t amo = match(Isa::RISCV, "amoadd.w", {reg(0), reg(0), reg(0), mem(0)});
    CHECK(amo >= 0 && (overlay_of(Isa::RISCV, amo) & OVL_ATOMIC),
          "riscv: amoadd.w overlay atomic");
    // fence -> overlay barrera.
    int32_t fen = match(Isa::RISCV, "fence", {ParsedOp{OP_IMM, 0}, ParsedOp{OP_IMM, 0}});
    CHECK(fen >= 0 && (overlay_of(Isa::RISCV, fen) & OVL_BARRIER),
          "riscv: fence overlay barrera");
    // la misma DB no confunde ISAs: 'ldaxr' no existe en x86.
    CHECK(match(Isa::X86, "ldaxr", {}) < 0, "x86 no tiene ldaxr");

    // --- capa de COSTE (latencia + puertos) ---
    CHECK(microarch_count(Isa::X86) == 21, "x86: 21 microarq con coste");
    int32_t skl = microarch_by_name(Isa::X86, "intel-skylake");
    CHECK(skl >= 0, "x86: intel-skylake presente");
    // add reg,reg en skylake: latencia 1, throughput alto, con puertos.
    AsmCost ca = cost(Isa::X86, add, (uint32_t)skl);
    CHECK(ca.found, "coste add en skylake encontrado");
    CHECK(ca.latency >= 1.0f && ca.latency <= 1.5f, "add skylake latencia ~1");
    CHECK(ca.ports_count > 0 && ca.port_names, "add skylake usa puertos (paralelo)");
    // microarq inexistente / forma sin coste -> found=false.
    CHECK(!cost(Isa::X86, add, 999).found, "microarq fuera de rango -> no found");
    // arm64: coste en neoverse-n2.
    int32_t n2 = microarch_by_name(Isa::ARM64, "neoverse-n2");
    CHECK(n2 >= 0, "arm64: neoverse-n2 presente");
    AsmCost cn = cost(Isa::ARM64, aadd, (uint32_t)n2);
    CHECK(cn.found && cn.latency > 0.0f, "arm64: coste add en neoverse-n2");
    // riscv: coste en sifive-p670.
    int32_t p6 = microarch_by_name(Isa::RISCV, "sifive-p670");
    CHECK(p6 >= 0 && cost(Isa::RISCV, amo, (uint32_t)p6).found,
          "riscv: coste amoadd.w en sifive-p670");

    // --- capa de FEATURES (que admite cada CPU) ---
    CHECK(cpu_count(Isa::X86) == 128, "x86: 128 CPU con features");
    int32_t hsw = cpu_by_name(Isa::X86, "haswell");
    CHECK(hsw >= 0, "x86: haswell presente");
    CHECK(cpu_has_feature(Isa::X86, (uint32_t)hsw, "AVX2"),
          "haswell tiene AVX2");
    CHECK(!cpu_has_feature(Isa::X86, (uint32_t)hsw, "AVX512F"),
          "haswell NO tiene AVX512");
    // arm64 y arm32 comparten features (misma tabla ARM).
    int32_t n2c = cpu_by_name(Isa::ARM64, "neoverse-n2");
    CHECK(n2c >= 0 && cpu_has_feature(Isa::ARM64, (uint32_t)n2c, "SVE2"),
          "arm64: neoverse-n2 tiene SVE2");
    CHECK(cpu_by_name(Isa::ARM32, "neoverse-n2") == n2c,
          "arm32 comparte tabla de features con arm64");
    // riscv: sifive-x280 tiene vector.
    int32_t x280 = cpu_by_name(Isa::RISCV, "sifive-x280");
    CHECK(x280 >= 0 && cpu_has_feature(Isa::RISCV, (uint32_t)x280, "StdExtZve32x"),
          "riscv: sifive-x280 tiene Zve32x (vector)");

    if (g_fail == 0)
        std::printf("=== instr_db_test: %d checks OK, 0 fallidos ===\n",
                    g_checks);
    else
        std::printf("=== instr_db_test: %d checks, %d FALLIDOS ===\n",
                    g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
