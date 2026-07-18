/**
 * @file test_arm64_select.cpp
 * @brief Tests del selector IR -> AArch64 (ver jit/arm64/arm64_select.h): emite
 *        el texto ensamblador del subconjunto entero de linea recta.
 */
#include "jit/arm64/arm64_select.h"

#include <cstdio>
#include <fstream>
#include <string>

static int g_checks = 0, g_fail = 0;
#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(cond)) {                                                          \
            ++g_fail;                                                           \
            std::printf("  FAIL: %s (linea %d)\n", (msg), __LINE__);            \
        }                                                                       \
    } while (0)

static bool has(const std::string &s, const std::string &sub) {
    return s.find(sub) != std::string::npos;
}

/// Anade una instruccion simple a un bloque.
static ir::IrInstr mk(ir::IrOp op, ir::IrType t, ir::IrValueId dst) {
    ir::IrInstr i;
    i.op = op;
    i.type = t;
    i.dst = dst;
    return i;
}

/// Construye la funcion `add(a,b) = a + b` y devuelve su asm AArch64.
static std::string emit_add_fn(bool &uns) {
    ir::IrFunction fn;
    fn.name = "add";
    fn.ret_type = ir::IrType::I64;
    const ir::IrValueId a = fn.new_value(ir::IrType::I64);
    const ir::IrValueId b = fn.new_value(ir::IrType::I64);
    fn.params.push_back(a);
    fn.params.push_back(b);
    const ir::IrValueId s = fn.new_value(ir::IrType::I64);
    ir::IrBlock e;
    e.id = 0;
    e.name = "entry";
    ir::IrInstr add = mk(ir::IrOp::ADD, ir::IrType::I64, s);
    add.operands = {a, b};
    e.instrs.push_back(add);
    ir::IrInstr r = mk(ir::IrOp::RET, ir::IrType::I64, ir::IR_NO_VALUE);
    r.operands.push_back(s);
    e.instrs.push_back(r);
    fn.blocks.push_back(e);
    return jit::arm64::arm64_emit_asm(fn, uns);
}

/// Modo `boot <out.s>`: escribe un programa AArch64 bare-metal que llama a la
/// funcion `add` GENERADA por el selector con add(3,4) y sale por semihosting con
/// el resultado (7) como codigo.  Lo consume tests/aot/qemu_arm64_codegen_test.py.
static int emit_boot(const char *path) {
    bool uns = false;
    const std::string body = emit_add_fn(uns);
    if (uns)
        return 1;
    std::ofstream o(path);
    if (!o)
        return 1;
    // Harness: sp por encima del DTB de virt; carga args, llama add, exit(x0).
    o << "movz x20, #0x4030, lsl #16\n";
    o << "mov sp, x20\n";
    o << "movz x0, #3\n";
    o << "movz x1, #4\n";
    o << "bl add_fn\n";
    o << "mov x21, x0\n";
    o << "sub sp, sp, #16\n";
    o << "movz x2, #0x26\n";
    o << "movk x2, #0x2, lsl #16\n";
    o << "str x2, [sp]\n";
    o << "str x21, [sp, #8]\n";
    o << "mov x1, sp\n";
    o << "movz x0, #0x18\n";
    o << "hlt #0xf000\n";
    o << "add_fn:\n";
    o << body;
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 3 && std::string(argv[1]) == "boot")
        return emit_boot(argv[2]);

    std::printf("=== test_arm64_select ===\n");

    // --- u64 f() { return 42; } ---
    {
        ir::IrFunction fn;
        fn.name = "ret42";
        fn.ret_type = ir::IrType::I64;
        const ir::IrValueId v = fn.new_value(ir::IrType::I64);
        ir::IrBlock e;
        e.id = 0;
        e.name = "entry";
        ir::IrInstr c = mk(ir::IrOp::CONST, ir::IrType::I64, v);
        c.imm = 42;
        e.instrs.push_back(c);
        ir::IrInstr r = mk(ir::IrOp::RET, ir::IrType::I64, ir::IR_NO_VALUE);
        r.operands.push_back(v);
        e.instrs.push_back(r);
        fn.blocks.push_back(e);

        bool uns = false;
        std::string a = jit::arm64::arm64_emit_asm(fn, uns);
        CHECK(!uns, "ret42: soportado");
        CHECK(has(a, "sub sp, sp, #"), "ret42: prologo reserva marco");
        CHECK(has(a, "movz x9, #42"), "ret42: carga el inmediato 42");
        CHECK(has(a, "ldr x0, [sp,"), "ret42: retorno carga x0");
        CHECK(has(a, "add sp, sp, #"), "ret42: epilogo libera marco");
        CHECK(has(a, "ret"), "ret42: ret final");
    }

    // --- u64 add(u64 a, u64 b) { return a + b; } ---
    {
        ir::IrFunction fn;
        fn.name = "add";
        fn.ret_type = ir::IrType::I64;
        const ir::IrValueId a = fn.new_value(ir::IrType::I64);
        const ir::IrValueId b = fn.new_value(ir::IrType::I64);
        fn.params.push_back(a);
        fn.params.push_back(b);
        const ir::IrValueId s = fn.new_value(ir::IrType::I64);
        ir::IrBlock e;
        e.id = 0;
        e.name = "entry";
        ir::IrInstr add = mk(ir::IrOp::ADD, ir::IrType::I64, s);
        add.operands = {a, b};
        e.instrs.push_back(add);
        ir::IrInstr r = mk(ir::IrOp::RET, ir::IrType::I64, ir::IR_NO_VALUE);
        r.operands.push_back(s);
        e.instrs.push_back(r);
        fn.blocks.push_back(e);

        bool uns = false;
        std::string asmtxt = jit::arm64::arm64_emit_asm(fn, uns);
        CHECK(!uns, "add: soportado");
        CHECK(has(asmtxt, "str x0, [sp,"), "add: guarda el param 0");
        CHECK(has(asmtxt, "str x1, [sp,"), "add: guarda el param 1");
        CHECK(has(asmtxt, "add x9, x9, x10"), "add: suma");
        CHECK(has(asmtxt, "ldr x0, [sp,"), "add: retorno en x0");
        CHECK(has(asmtxt, "ret"), "add: ret");
    }

    // --- Op no soportada aun (varios bloques / rama) -> out_unsupported. ---
    {
        ir::IrFunction fn;
        fn.name = "branchy";
        fn.ret_type = ir::IrType::I64;
        ir::IrBlock e;
        e.id = 0;
        e.name = "entry";
        ir::IrInstr br = mk(ir::IrOp::BR, ir::IrType::VOID, ir::IR_NO_VALUE);
        br.target_block = 1;
        e.instrs.push_back(br);
        ir::IrBlock e2;
        e2.id = 1;
        e2.name = "b1";
        fn.blocks.push_back(e);
        fn.blocks.push_back(e2);

        bool uns = false;
        (void)jit::arm64::arm64_emit_asm(fn, uns);
        CHECK(uns, "multi-bloque -> out_unsupported (H.2b)");
    }

    std::printf("=== test_arm64_select: %d checks OK, %d fallidos ===\n",
                g_checks - g_fail, g_fail);
    return g_fail ? 1 : 0;
}
