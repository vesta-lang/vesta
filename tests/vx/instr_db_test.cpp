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

    if (g_fail == 0)
        std::printf("=== instr_db_test: %d checks OK, 0 fallidos ===\n",
                    g_checks);
    else
        std::printf("=== instr_db_test: %d checks, %d FALLIDOS ===\n",
                    g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
