/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/ir/test_devirt_known_target.cpp
 * @brief Test de @c ir::ir_devirt_known_targets: toda llamada indirecta de
 *        destino conocido pasa a directa, en la forma que su destino espera.
 *
 * Cada caso construye a mano la llamada y comprueba la FORMA resultante:
 *
 *  - CALLCLOSURE a una lambda sin capturas        -> CALL sin entorno;
 *  - CALLCLOSURE a una que recibe el entorno como
 *    parametro (convencion nativa)                -> CALL con el entorno al final;
 *  - CALLCLOSURE a una que lo lee del registro,
 *    con el destino por MOV                        -> sigue CALLCLOSURE, con el
 *                                                     destino ESCRITO (LABEL_ADDR);
 *  - la misma con el destino ya escrito            -> no se toca;
 *  - CALLIND a una direccion conocida              -> CALL;
 *  - CALLVIRT / CALLITF con el destino demostrado
 *    por la jerarquia                              -> CALL tras `receptor != 0`,
 *                                                     con el despacho original de
 *                                                     respaldo (sobre nulo lanza
 *                                                     lo mismo);
 *  - CALLSUPER                                     -> CALL (el receptor es this);
 *  - y el hecho `proven_callee` sobrevive a la serializacion.
 */

#include "ir/ssa_ir.h"
#include "ir/ssa_ir_serialize.h"
#include "ir/passes/devirt_known_target.h"

#include <cstdio>
#include <string>

using namespace ir;

static int g_checks = 0;
static int g_fails = 0;

/**
 * @brief Anota una comprobacion.
 * @param cond Si se cumple.
 * @param msg  Que se comprueba.
 */
static void check(bool cond, const char *msg) {
    ++g_checks;
    if (!cond) {
        ++g_fails;
        std::printf("  FAIL: %s\n", msg);
    } else {
        std::printf("  ok:   %s\n", msg);
    }
}

/**
 * @brief Anade al bloque @p b de @p fn la instruccion @p in.
 * @param fn Funcion.
 * @param b  Bloque.
 * @param in Instruccion.
 */
static void put(IrFunction &fn, IrBlockId b, IrInstr in) {
    fn.blocks[b].instrs.push_back(std::move(in));
}

/**
 * @brief Una funcion hoja `name(params...) -> i64` que devuelve su primer
 *        parametro; con @p env_reg, antes lee el registro del entorno.
 * @param name     Nombre.
 * @param nparams  Cuantos parametros declara.
 * @param env_reg  Si lee el entorno de la closure del registro.
 * @return La funcion.
 */
static IrFunction make_leaf(const std::string &name, size_t nparams,
                            bool env_reg) {
    IrFunction f;
    f.name = name;
    f.ret_type = IrType::I64;
    for (size_t i = 0; i < nparams; ++i)
        f.params.push_back(f.new_value(IrType::I64));
    const IrBlockId b = f.new_block("entry");
    if (env_reg) {
        IrInstr rr;
        rr.op = IrOp::READ_VM_REG;
        rr.type = IrType::PTR;
        rr.dst = f.new_value(IrType::PTR);
        rr.imm = kClosureEnvVmReg;
        put(f, b, std::move(rr));
    }
    IrInstr ret;
    ret.op = IrOp::RET;
    ret.type = IrType::I64;
    ret.operands.push_back(f.params.front());
    put(f, b, std::move(ret));
    return f;
}

/**
 * @brief Un valor definido por un @c LABEL_ADDR de @p target.
 * @param fn     Funcion donde se emite.
 * @param b      Bloque.
 * @param target Funcion cuya direccion es.
 * @return El valor.
 */
static IrValueId label_of(IrFunction &fn, IrBlockId b,
                          const std::string &target) {
    IrInstr la;
    la.op = IrOp::LABEL_ADDR;
    la.type = IrType::PTR;
    la.dst = fn.new_value(IrType::PTR);
    la.func_name = target;
    const IrValueId v = la.dst;
    put(fn, b, std::move(la));
    return v;
}

/**
 * @brief Una @c CALLCLOSURE a traves de @p fptr con entorno @p env y un
 *        argumento @p arg.
 * @return La instruccion.
 */
static IrInstr closure_call(IrFunction &fn, IrValueId fptr, IrValueId env,
                            IrValueId arg) {
    IrInstr cc;
    cc.op = IrOp::CALLCLOSURE;
    cc.type = IrType::I64;
    cc.dst = fn.new_value(IrType::I64);
    cc.func_ptr = fptr;
    cc.operands.push_back(env);
    cc.operands.push_back(arg);
    return cc;
}

/**
 * @brief Un despacho de forma @p op con destino demostrado @p proven.
 * @param fn       Funcion.
 * @param op       CALLVIRT, CALLITF o CALLSUPER.
 * @param operands Sus operandos, en la forma de esa operacion.
 * @param proven   Destino demostrado.
 * @return La instruccion.
 */
static IrInstr proven_dispatch(IrFunction &fn, IrOp op, IrOperands operands,
                               const std::string &proven) {
    IrInstr d;
    d.op = op;
    d.type = IrType::I64;
    d.dst = fn.new_value(IrType::I64);
    d.operands = std::move(operands);
    d.imm = 3;
    d.proven_callee = util::InternedName::intern(proven);
    return d;
}

int main() {
    /* Sin bufer: si algo casca, se ve hasta que comprobacion llego. */
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== test_devirt_known_target ===\n");
    IrModule mod;
    mod.functions.push_back(make_leaf("lam_sin_entorno", 1, false));
    mod.functions.push_back(make_leaf("lam_entorno_param", 2, false));
    mod.functions.push_back(make_leaf("lam_entorno_reg", 1, true));
    mod.functions.push_back(make_leaf("Clase__metodo", 2, false));

    IrFunction caller;
    caller.name = "caller";
    caller.ret_type = IrType::I64;
    const IrValueId obj = caller.new_value(IrType::PTR);
    const IrValueId arg = caller.new_value(IrType::I64);
    const IrValueId env = caller.new_value(IrType::PTR);
    caller.params = {obj, arg, env};
    const IrBlockId b = caller.new_block("entry");

    const IrValueId l_sin = label_of(caller, b, "lam_sin_entorno");
    const IrValueId l_param = label_of(caller, b, "lam_entorno_param");
    const IrValueId l_reg = label_of(caller, b, "lam_entorno_reg");
    const IrValueId l_ind = label_of(caller, b, "Clase__metodo");
    IrInstr mv;
    mv.op = IrOp::MOV;
    mv.type = IrType::PTR;
    mv.dst = caller.new_value(IrType::PTR);
    mv.operands.push_back(l_reg);
    const IrValueId l_reg_copia = mv.dst;
    put(caller, b, std::move(mv));

    put(caller, b, closure_call(caller, l_sin, env, arg));       // [0]
    put(caller, b, closure_call(caller, l_param, env, arg));     // [1]
    put(caller, b, closure_call(caller, l_reg_copia, env, arg)); // [2]
    put(caller, b, closure_call(caller, l_reg, env, arg));       // [3]
    IrInstr ci;
    ci.op = IrOp::CALLIND;
    ci.type = IrType::I64;
    ci.dst = caller.new_value(IrType::I64);
    ci.func_ptr = l_ind;
    ci.operands.push_back(obj);
    ci.operands.push_back(arg);
    put(caller, b, std::move(ci)); // [4]
    put(caller, b,
        proven_dispatch(caller, IrOp::CALLVIRT, {obj, arg},
                        "Clase__metodo")); // [5]
    put(caller, b,
        proven_dispatch(caller, IrOp::CALLITF, {obj, env, arg},
                        "Clase__metodo")); // [6]
    put(caller, b,
        proven_dispatch(caller, IrOp::CALLSUPER, {env, obj, arg},
                        "Clase__metodo")); // [7]
    IrInstr ret;
    ret.op = IrOp::RET;
    ret.type = IrType::I64;
    ret.operands.push_back(arg);
    put(caller, b, std::move(ret));

    /* El hecho viaja por el formato serializado antes de optimizar. */
    {
        util::ByteBuffer buf;
        util::byte_buffer_init(buf, &kIrSectionKind);
        serialize_function(caller, buf);
        util::ByteCursor cur = util::byte_cursor_at_start(buf);
        IrFunction back;
        check(deserialize_function(cur, back), "deserializa");
        size_t proven = 0;
        for (const IrInstr &in : back.blocks[0].instrs)
            if (!in.proven_callee.empty() &&
                in.proven_callee.str() == "Clase__metodo")
                ++proven;
        check(proven == 3, "proven_callee sobrevive a la serializacion");
    }

    mod.functions.push_back(std::move(caller));
    check(ir_devirt_known_targets(mod), "el pase cambia algo");

    /* El pase parte bloques (la guarda de nulo): se mira la funcion entera. */
    std::vector<IrInstr> ins;
    for (const IrBlock &blk : mod.functions.back().blocks)
        ins.insert(ins.end(), blk.instrs.begin(), blk.instrs.end());
    size_t calls = 0;
    size_t closures = 0;
    size_t fallbacks = 0;
    size_t null_guards = 0;
    for (const IrInstr &in : ins) {
        if (in.op == IrOp::CALL) ++calls;
        if (in.op == IrOp::CALLCLOSURE) ++closures;
        if ((in.op == IrOp::CALLVIRT || in.op == IrOp::CALLITF) &&
            in.proven_callee.empty())
            ++fallbacks;
        if (in.op == IrOp::CMP_NE && in.operands.size() == 2 &&
            in.operands[0] == obj)
            ++null_guards;
    }
    check(calls == 6, "seis llamadas pasan a CALL directo");
    check(closures == 2, "dos siguen CALLCLOSURE (leen el entorno)");
    /* El despacho comprobaba el receptor (sobre nulo lanza, capturable); la
     * llamada directa va tras `receptor != 0` y el despacho ORIGINAL queda de
     * respaldo.  CALLVIRT y CALLITF si; CALLSUPER no (es `this`). */
    check(null_guards == 2, "CALLVIRT y CALLITF: guarda de nulo");
    check(fallbacks == 2, "y el despacho original de respaldo, sin el hecho");

    for (const IrInstr &in : ins) {
        if (in.op == IrOp::CALL && in.func_name == "lam_sin_entorno")
            check(in.operands.size() == 1 && in.operands[0] == arg,
                  "sin capturas: CALL sin entorno");
        if (in.op == IrOp::CALL && in.func_name == "lam_entorno_param")
            check(in.operands.size() == 2 && in.operands[0] == arg &&
                      in.operands[1] == env,
                  "entorno como parametro: CALL con el entorno al final");
        if (in.op == IrOp::CALL && in.func_name == "Clase__metodo")
            check(in.operands.size() == 2 && in.operands[0] == obj &&
                      in.operands[1] == arg && in.func_ptr == IR_NO_VALUE &&
                      in.proven_callee.empty(),
                  "CALLIND/CALLVIRT/CALLITF/CALLSUPER: [receptor, arg]");
        if (in.op == IrOp::CALLCLOSURE)
            check(in.func_ptr != l_reg_copia,
                  "entorno por registro: el destino queda ESCRITO");
    }
    const LabelAddrIndex labels(mod.functions.back());
    for (const IrInstr &in : ins)
        if (in.op == IrOp::CALLCLOSURE)
            check(known_closure_target(in, labels).str() == "lam_entorno_reg",
                  "la closure directa apunta a su lambda");

    check(!ir_devirt_known_targets(mod), "segunda pasada: nada que hacer");

    std::printf("=== test_devirt_known_target: %d pasos OK, %d fallidos ===\n",
                g_checks - g_fails, g_fails);
    return g_fails == 0 ? 0 : 1;
}
