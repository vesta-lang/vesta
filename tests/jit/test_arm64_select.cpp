/**
 * @file test_arm64_select.cpp
 * @brief Tests del selector IR -> AArch64 (ver jit/arm64/arm64_select.h): emite
 *        el texto ensamblador del subconjunto entero de linea recta.
 */
#include "jit/arm64/arm64_select.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

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

/// Construye `caller() = add(3, 4)` (una LLAMADA `bl add`) y devuelve su asm.
static std::string emit_caller_fn(bool &uns) {
    ir::IrFunction fn;
    fn.name = "caller";
    fn.ret_type = ir::IrType::I64;
    const ir::IrValueId c3 = fn.new_value(ir::IrType::I64);
    const ir::IrValueId c4 = fn.new_value(ir::IrType::I64);
    const ir::IrValueId r = fn.new_value(ir::IrType::I64);
    ir::IrBlock e;
    e.id = 0;
    e.name = "entry";
    ir::IrInstr k3 = mk(ir::IrOp::CONST, ir::IrType::I64, c3);
    k3.imm = 3;
    e.instrs.push_back(k3);
    ir::IrInstr k4 = mk(ir::IrOp::CONST, ir::IrType::I64, c4);
    k4.imm = 4;
    e.instrs.push_back(k4);
    ir::IrInstr call = mk(ir::IrOp::CALL, ir::IrType::I64, r);
    call.func_name = "add";
    call.operands = {c3, c4};
    e.instrs.push_back(call);
    ir::IrInstr ret = mk(ir::IrOp::RET, ir::IrType::I64, ir::IR_NO_VALUE);
    ret.operands.push_back(r);
    e.instrs.push_back(ret);
    fn.blocks.push_back(e);
    return jit::arm64::arm64_emit_asm(fn, uns);
}

/// Construye `sum(n) = 1+2+...+n` (bucle con PHI + CMP + BR_COND) y devuelve su
/// asm AArch64.
static std::string emit_sum_fn(bool &uns) {
    ir::IrFunction fn;
    fn.name = "sum";
    fn.ret_type = ir::IrType::I64;
    const ir::IrValueId n = fn.new_value(ir::IrType::I64);
    fn.params.push_back(n);
    const ir::IrValueId s0 = fn.new_value(ir::IrType::I64);
    const ir::IrValueId i0 = fn.new_value(ir::IrType::I64);
    const ir::IrValueId s = fn.new_value(ir::IrType::I64);
    const ir::IrValueId i = fn.new_value(ir::IrType::I64);
    const ir::IrValueId cond = fn.new_value(ir::IrType::BOOL);
    const ir::IrValueId s2 = fn.new_value(ir::IrType::I64);
    const ir::IrValueId one = fn.new_value(ir::IrType::I64);
    const ir::IrValueId i2 = fn.new_value(ir::IrType::I64);

    ir::IrBlock b0;
    b0.id = 0;
    b0.name = "entry";
    ir::IrInstr c0 = mk(ir::IrOp::CONST, ir::IrType::I64, s0);
    c0.imm = 0;
    b0.instrs.push_back(c0);
    ir::IrInstr c1 = mk(ir::IrOp::CONST, ir::IrType::I64, i0);
    c1.imm = 1;
    b0.instrs.push_back(c1);
    ir::IrInstr br0 = mk(ir::IrOp::BR, ir::IrType::VOID, ir::IR_NO_VALUE);
    br0.target_block = 1;
    b0.instrs.push_back(br0);

    ir::IrBlock b1;
    b1.id = 1;
    b1.name = "header";
    ir::IrInstr ps = mk(ir::IrOp::PHI, ir::IrType::I64, s);
    ps.phi_args = {{s0, 0}, {s2, 2}};
    b1.instrs.push_back(ps);
    ir::IrInstr pi = mk(ir::IrOp::PHI, ir::IrType::I64, i);
    pi.phi_args = {{i0, 0}, {i2, 2}};
    b1.instrs.push_back(pi);
    ir::IrInstr cm = mk(ir::IrOp::CMP_ULE, ir::IrType::BOOL, cond);
    cm.operands = {i, n};
    b1.instrs.push_back(cm);
    ir::IrInstr brc = mk(ir::IrOp::BR_COND, ir::IrType::VOID, ir::IR_NO_VALUE);
    brc.operands = {cond};
    brc.target_block = 2;
    brc.false_block = 3;
    b1.instrs.push_back(brc);

    ir::IrBlock b2;
    b2.id = 2;
    b2.name = "body";
    ir::IrInstr as = mk(ir::IrOp::ADD, ir::IrType::I64, s2);
    as.operands = {s, i};
    b2.instrs.push_back(as);
    ir::IrInstr co = mk(ir::IrOp::CONST, ir::IrType::I64, one);
    co.imm = 1;
    b2.instrs.push_back(co);
    ir::IrInstr ai = mk(ir::IrOp::ADD, ir::IrType::I64, i2);
    ai.operands = {i, one};
    b2.instrs.push_back(ai);
    ir::IrInstr br1 = mk(ir::IrOp::BR, ir::IrType::VOID, ir::IR_NO_VALUE);
    br1.target_block = 1;
    b2.instrs.push_back(br1);

    ir::IrBlock b3;
    b3.id = 3;
    b3.name = "exit";
    ir::IrInstr r = mk(ir::IrOp::RET, ir::IrType::I64, ir::IR_NO_VALUE);
    r.operands.push_back(s);
    b3.instrs.push_back(r);

    fn.blocks = {b0, b1, b2, b3};
    return jit::arm64::arm64_emit_asm(fn, uns);
}

/// Construye `do_atomics(p)`: sobre la celda en @c *p (inicializada a 10 por el
/// harness) hace  a=fetch_add(p,5) (=10, celda->15);  b=cas(p,15,42) (=15,
/// celda->42);  c=fetch_add(p,0) (=42);  return c.  Ejercita ATOMIC_ADD (fetch +
/// store) y ATOMIC_CAS (match + store) y relee la celda -> el resultado 42 solo
/// sale si TODA la cadena atomica es correcta.
static std::string emit_atomics_fn(bool &uns) {
    ir::IrFunction fn;
    fn.name = "atomics";
    fn.ret_type = ir::IrType::I64;
    const ir::IrValueId p = fn.new_value(ir::IrType::I64);
    fn.params.push_back(p);
    const ir::IrValueId c5 = fn.new_value(ir::IrType::I64);
    const ir::IrValueId c15 = fn.new_value(ir::IrType::I64);
    const ir::IrValueId c42 = fn.new_value(ir::IrType::I64);
    const ir::IrValueId c0 = fn.new_value(ir::IrType::I64);
    const ir::IrValueId a = fn.new_value(ir::IrType::I64);
    const ir::IrValueId b = fn.new_value(ir::IrType::I64);
    const ir::IrValueId c = fn.new_value(ir::IrType::I64);
    ir::IrBlock e;
    e.id = 0;
    e.name = "entry";
    auto konst = [&](ir::IrValueId d, uint64_t v) {
        ir::IrInstr k = mk(ir::IrOp::CONST, ir::IrType::I64, d);
        k.imm = v;
        e.instrs.push_back(k);
    };
    konst(c5, 5);
    konst(c15, 15);
    konst(c42, 42);
    konst(c0, 0);
    ir::IrInstr add1 = mk(ir::IrOp::ATOMIC_ADD_I64, ir::IrType::I64, a);
    add1.operands = {p, c5};
    e.instrs.push_back(add1);
    ir::IrInstr cas = mk(ir::IrOp::ATOMIC_CAS_I64, ir::IrType::I64, b);
    cas.operands = {p, c15, c42};
    e.instrs.push_back(cas);
    ir::IrInstr add0 = mk(ir::IrOp::ATOMIC_ADD_I64, ir::IrType::I64, c);
    add0.operands = {p, c0};
    e.instrs.push_back(add0);
    ir::IrInstr ret = mk(ir::IrOp::RET, ir::IrType::I64, ir::IR_NO_VALUE);
    ret.operands.push_back(c);
    e.instrs.push_back(ret);
    fn.blocks.push_back(e);
    return jit::arm64::arm64_emit_asm(fn, uns);
}

/// Escribe un programa bare-metal que llama a `fn_body` (etiqueta @p entry) con
/// UN argumento @p arg y sale por semihosting con el resultado como codigo.
static void write_boot(std::ofstream &o, const std::string &fn_body, int arg) {
    o << "movz x20, #0x4030, lsl #16\n";
    o << "mov sp, x20\n";
    o << "movz x0, #" << arg << "\n";
    o << "bl fn_body\n";
    o << "mov x21, x0\n";
    o << "sub sp, sp, #16\n";
    o << "movz x2, #0x26\n";
    o << "movk x2, #0x2, lsl #16\n";
    o << "str x2, [sp]\n";
    o << "str x21, [sp, #8]\n";
    o << "mov x1, sp\n";
    o << "movz x0, #0x18\n";
    o << "hlt #0xf000\n";
    o << "fn_body:\n";
    o << fn_body;
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
    if (argc >= 3 && std::string(argv[1]) == "bootsum") {
        bool uns = false;
        const std::string body = emit_sum_fn(uns);
        if (uns)
            return 1;
        std::ofstream o(argv[2]);
        if (!o)
            return 1;
        write_boot(o, body, 4); // sum(4) = 10
        return 0;
    }
    if (argc >= 3 && std::string(argv[1]) == "bootatomic") {
        bool uns = false;
        const std::string body = emit_atomics_fn(uns);
        if (uns)
            return 1;
        std::ofstream o(argv[2]);
        if (!o)
            return 1;
        // Harness: reserva una celda en la pila, la inicializa a 10, pasa su
        // direccion en x0 y llama.  El resultado (42) sale por semihosting.
        o << "movz x20, #0x4030, lsl #16\n";
        o << "mov sp, x20\n";
        o << "sub sp, sp, #16\n";
        o << "movz x0, #10\n";
        o << "str x0, [sp]\n";  // celda = 10
        o << "mov x0, sp\n";    // x0 = &celda
        o << "bl fn_body\n";
        o << "mov x21, x0\n";
        o << "sub sp, sp, #16\n";
        o << "movz x2, #0x26\n";
        o << "movk x2, #0x2, lsl #16\n";
        o << "str x2, [sp]\n";
        o << "str x21, [sp, #8]\n";
        o << "mov x1, sp\n";
        o << "movz x0, #0x18\n";
        o << "hlt #0xf000\n";
        o << "fn_body:\n" << body;
        return 0;
    }
    if (argc >= 3 && std::string(argv[1]) == "bootcall") {
        bool u1 = false, u2 = false;
        const std::string caller = emit_caller_fn(u1);
        const std::string add = emit_add_fn(u2);
        if (u1 || u2)
            return 1;
        std::ofstream o(argv[2]);
        if (!o)
            return 1;
        // Harness: llama caller() (sin args), que a su vez hace `bl add`.
        o << "movz x20, #0x4030, lsl #16\n";
        o << "mov sp, x20\n";
        o << "bl caller_fn\n";
        o << "mov x21, x0\n";
        o << "sub sp, sp, #16\n";
        o << "movz x2, #0x26\n";
        o << "movk x2, #0x2, lsl #16\n";
        o << "str x2, [sp]\n";
        o << "str x21, [sp, #8]\n";
        o << "mov x1, sp\n";
        o << "movz x0, #0x18\n";
        o << "hlt #0xf000\n";
        o << "caller_fn:\n" << caller << "add:\n" << add;
        return 0;
    }

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

    // --- Bucle sum(n): multi-bloque + PHI + CMP + BR_COND. ---
    {
        bool uns = false;
        std::string a = emit_sum_fn(uns);
        CHECK(!uns, "sum: soportado (multi-bloque + ramas)");
        CHECK(has(a, ".Lsum_b1:"), "sum: etiqueta del header (prefijada)");
        CHECK(has(a, "cmp x9, x10"), "sum: comparacion");
        CHECK(has(a, "cset x9, ls"), "sum: cset con la condicion ULE (ls)");
        CHECK(has(a, "cbnz x9"), "sum: rama condicional");
        CHECK(has(a, "b .Lsum_b1"), "sum: back-edge al header");
        CHECK(has(a, "str x11,"), "sum: copias de PHI en los predecesores");
    }

    // --- Llamada: caller() = add(3,4).  bl + salvar/restaurar LR. ---
    {
        bool uns = false;
        std::string a = emit_caller_fn(uns);
        CHECK(!uns, "caller: soportado (CALL)");
        CHECK(has(a, "bl add"), "caller: bl a la funcion add");
        CHECK(has(a, "str x30,"), "caller: salva LR (hay llamada)");
        CHECK(has(a, "ldr x30,"), "caller: restaura LR antes del ret");
        CHECK(has(a, "str x0,") && has(a, "ldr x0,"),
              "caller: marshalling de args/resultado por x0");
    }
    // Una funcion LEAF (add) NO salva LR.
    {
        bool uns = false;
        std::string a = emit_add_fn(uns);
        CHECK(!has(a, "str x30,"), "add (leaf): no salva LR");
    }

    // --- Atomicos: ATOMIC_CAS / ATOMIC_ADD via bucle LL/SC (cierra ASA.3). ---
    {
        bool uns = false;
        std::string a = emit_atomics_fn(uns);
        CHECK(!uns, "atomics: soportado");
        CHECK(has(a, "ldaxr x12, [x9]"), "atomics: ldaxr del CAS");
        CHECK(has(a, "stlxr w13, x11, [x9]"), "atomics: stlxr del CAS");
        CHECK(has(a, "b.ne .Latomics_acd"), "atomics: salida del CAS en mismatch");
        CHECK(has(a, "ldaxr x11, [x9]"), "atomics: ldaxr del ADD");
        CHECK(has(a, "add x12, x11, x10"), "atomics: suma del fetch-and-add");
        CHECK(has(a, "cbnz w13, .Latomics_aar"), "atomics: reintento del ADD");
    }

    // --- Op no soportada aun (LOAD de memoria) -> out_unsupported. ---
    {
        ir::IrFunction fn;
        fn.name = "loady";
        fn.ret_type = ir::IrType::I64;
        const ir::IrValueId p = fn.new_value(ir::IrType::I64);
        fn.params.push_back(p);
        const ir::IrValueId v = fn.new_value(ir::IrType::I64);
        ir::IrBlock e;
        e.id = 0;
        e.name = "entry";
        ir::IrInstr ld = mk(ir::IrOp::LOAD, ir::IrType::I64, v);
        ld.operands.push_back(p);
        e.instrs.push_back(ld);
        fn.blocks.push_back(e);

        bool uns = false;
        (void)jit::arm64::arm64_emit_asm(fn, uns);
        CHECK(uns, "LOAD -> out_unsupported (H.3+)");
    }

    std::printf("=== test_arm64_select: %d checks OK, %d fallidos ===\n",
                g_checks - g_fail, g_fail);
    return g_fail ? 1 : 0;
}
