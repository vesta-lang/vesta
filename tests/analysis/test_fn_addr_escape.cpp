/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file test_fn_addr_escape.cpp
 * @brief Que direcciones de funcion tienen que ser REALES, y por donde cruzan.
 *
 * Una funcion de Vesta tiene dos direcciones posibles en la maquina: la del
 * bytecode (gratis, pero codigo nativo no puede saltar ahi) y la nativa (se
 * compila al vuelo, asi que cuesta).  Cual hace falta lo decide el USO.
 *
 * LO QUE SE PRUEBA, y por que asi.  Los casos se montan sobre el IR a mano y
 * no desde `.vx`: lo que decide es a DoNDE va la direccion, no como se
 * escribio en el fuente.  Construyendo el IR directamente el fuente
 * literalmente no existe, asi que ningun caso puede pasar por la grafia -- que
 * es exactamente el fallo que este analisis viene a sustituir, porque hoy la
 * eleccion es sintactica (`(cfn)nombre` si, `&nombre` no) y se equivoca cuando
 * el valor se lava por un entero.
 *
 * Y se prueban las DOS respuestas.  Un analisis que contestara "cruza" siempre
 * seria correcto y no serviria de nada: el defecto ya es la nativa, asi que su
 * valor esta entero en los casos que sabe abaratar.
 */
#include "analysis/escape/fn_addr_escape.h"
#include "analysis/facts/ir_facts.h"
#include "ir/ssa_ir.h"
#include "util/name_pool.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace analysis;

static int g_fail = 0;
static int g_checks = 0;

static void check(bool cond, const std::string &what) {
    ++g_checks;
    if (!cond) {
        ++g_fail;
        std::printf("  FAIL: %s\n", what.c_str());
    }
}

// ---------------------------------------------------------------------------
// Construccion del IR
// ---------------------------------------------------------------------------

/// Emite una instruccion y devuelve el `dst` que se le dio.
static ir::IrValueId emit(ir::IrFunction &fn, ir::IrBlockId blk, ir::IrOp op,
                          ir::IrValueId dst, std::vector<ir::IrValueId> ops) {
    ir::IrInstr in{};
    in.op = op;
    in.dst = dst;
    in.operands = std::move(ops);
    fn.append(blk, std::move(in));
    return dst;
}

/// `%v = label_addr @nombre` -- la direccion de una funcion NUESTRA.
static ir::IrValueId function_address(ir::IrFunction &fn, ir::IrBlockId blk,
                                      const char *nombre) {
    const ir::IrValueId v = fn.new_value(ir::IrType::PTR);
    ir::IrInstr in{};
    in.op = ir::IrOp::LABEL_ADDR;
    in.type = ir::IrType::PTR;
    in.dst = v;
    in.func_name = nombre;
    fn.append(blk, std::move(in));
    return v;
}

/// Una llamada NATIVA con @p args.
static void native_call(ir::IrFunction &fn, ir::IrBlockId blk,
                        const char *lib_fn, std::vector<ir::IrValueId> args) {
    ir::IrInstr in{};
    in.op = ir::IrOp::CALLN;
    in.type = ir::IrType::VOID;
    in.dst = ir::IR_NO_VALUE;
    in.func_name = lib_fn;
    in.operands = std::move(args);
    fn.append(blk, std::move(in));
}

/// Lo que el analisis contesta para @p v en @p fn, con los facts recien
/// construidos y sin oracle (ningun callee deja cruzar nada).
static FnAddrCrossing verdict(const ir::IrFunction &fn, ir::IrValueId v,
                              CalleeCrossesParam oracle = {}) {
    const IrFacts facts = build_ir_facts(fn);
    const PointsTo pt = compute_points_to(fn, facts);
    const EscapeInfo esc;    // nada escapa salvo lo que el caso diga
    const NativeReach reach; // no se le entrega ningun global a nadie
    /* Los nombres que los casos usan en sus `LABEL_ADDR` SON funciones: sin
     * decirlo, el analisis los tomaria por etiquetas de datos y no miraria
     * ninguno -- y entonces los siete casos pasarian por la razon equivocada,
     * que es la peor forma de pasar. */
    ModuleFunctionNames fns;
    for (const char *name : {"doblar", "comparar", "triplicar"})
        fns.sorted.push_back(util::intern_name(name));
    std::sort(fns.sorted.begin(), fns.sorted.end());
    const FnAddrEscape r =
        compute_fn_addr_escape(fn, facts, esc, pt, reach, fns, oracle);
    return r.why(v);
}

/// Oraculo de prueba: ningun callee saca nada.
static bool no_callee_crosses(void *, const std::string &, int32_t) {
    return false;
}

/// Oraculo de prueba: todos los callees lo sacan.
static bool every_callee_crosses(void *, const std::string &, int32_t) {
    return true;
}

/// Empaqueta uno de los dos de arriba.
static CalleeCrossesParam oracle_of(bool (*ask)(void *, const std::string &,
                                                int32_t)) {
    CalleeCrossesParam o;
    o.ask = ask;
    return o;
}

// ---------------------------------------------------------------------------
// Casos
// ---------------------------------------------------------------------------

/// Solo se llama desde Vesta: NO cruza, y por tanto se puede abaratar.
static void case_stays_inside() {
    ir::IrFunction fn;
    fn.name = "solo_dentro";
    const ir::IrBlockId b = fn.new_block("entry");
    const ir::IrValueId f = function_address(fn, b, "doblar");
    // %r = callind %f()  -- la llamada indirecta DE LA MAQUINA
    const ir::IrValueId r = fn.new_value(ir::IrType::I64);
    {
        ir::IrInstr in{};
        in.op = ir::IrOp::CALLIND;
        in.type = ir::IrType::I64;
        in.dst = r;
        in.func_ptr = f;
        fn.append(b, std::move(in));
    }
    emit(fn, b, ir::IrOp::RET, ir::IR_NO_VALUE, {});
    check(verdict(fn, f) == FnAddrCrossing::None,
          "an address only called from Vesta need not be a real one");
}

/// Se pasa a una nativa: cruza.  Es el `qsort` que recibe nuestro comparador.
static void case_passed_to_native() {
    ir::IrFunction fn;
    fn.name = "va_a_qsort";
    const ir::IrBlockId b = fn.new_block("entry");
    const ir::IrValueId f = function_address(fn, b, "comparar");
    native_call(fn, b, "msvcrt.dll:qsort", {f});
    emit(fn, b, ir::IrOp::RET, ir::IR_NO_VALUE, {});
    check(
        verdict(fn, f) == FnAddrCrossing::NativeCall,
        "an address passed as an argument to a native call MUST be a real one");
}

/// La MISMA direccion, lavada por una copia antes de cruzar.  Sigue cruzando:
/// lo que decide es a donde va, no por cuantas manos pasa.  Este es el caso
/// que la eleccion sintactica de hoy pierde.
static void case_laundered_by_a_copy() {
    ir::IrFunction fn;
    fn.name = "lavada";
    const ir::IrBlockId b = fn.new_block("entry");
    const ir::IrValueId f = function_address(fn, b, "comparar");
    const ir::IrValueId copy = fn.new_value(ir::IrType::I64);
    emit(fn, b, ir::IrOp::BITCAST, copy, {f});
    native_call(fn, b, "msvcrt.dll:qsort", {copy});
    emit(fn, b, ir::IrOp::RET, ir::IR_NO_VALUE, {});
    check(verdict(fn, copy) == FnAddrCrossing::NativeCall,
          "laundering the address through a copy still hands it out");
}

/// Se devuelve: donde acaba no se ve desde aqui, asi que cruza.  El modo de
/// fallo cae del lado bueno -- de mas, no de menos.
static void case_returned() {
    ir::IrFunction fn;
    fn.name = "la_devuelve";
    const ir::IrBlockId b = fn.new_block("entry");
    const ir::IrValueId f = function_address(fn, b, "doblar");
    emit(fn, b, ir::IrOp::RET, ir::IR_NO_VALUE, {f});
    check(verdict(fn, f) == FnAddrCrossing::Returned,
          "an address that is returned must be a real one");
}

/// Se pasa a una funcion NUESTRA que no la saca: no cruza.  Es lo que
/// distingue este analisis de suponer que todo lo que se pasa escapa.
static void case_to_ours_that_keeps_it() {
    ir::IrFunction fn;
    fn.name = "se_la_pasa_a_los_nuestros";
    const ir::IrBlockId b = fn.new_block("entry");
    const ir::IrValueId f = function_address(fn, b, "doblar");
    {
        ir::IrInstr in{};
        in.op = ir::IrOp::CALL;
        in.type = ir::IrType::VOID;
        in.dst = ir::IR_NO_VALUE;
        in.func_name = "aplicar";
        in.operands = {f};
        fn.append(b, std::move(in));
    }
    emit(fn, b, ir::IrOp::RET, ir::IR_NO_VALUE, {});
    check(verdict(fn, f, oracle_of(&no_callee_crosses)) == FnAddrCrossing::None,
          "passing it to one of ours that keeps it does not make it cross");
}

/// Y la misma, cuando el callee SI la saca: cruza por el.  Sin esto el punto
/// fijo del modulo no serviria de nada.
static void case_to_ours_that_hands_it_out() {
    ir::IrFunction fn;
    fn.name = "se_la_pasa_a_uno_que_la_saca";
    const ir::IrBlockId b = fn.new_block("entry");
    const ir::IrValueId f = function_address(fn, b, "doblar");
    {
        ir::IrInstr in{};
        in.op = ir::IrOp::CALL;
        in.type = ir::IrType::VOID;
        in.dst = ir::IR_NO_VALUE;
        in.func_name = "registra_callback";
        in.operands = {f};
        fn.append(b, std::move(in));
    }
    emit(fn, b, ir::IrOp::RET, ir::IR_NO_VALUE, {});
    check(verdict(fn, f, oracle_of(&every_callee_crosses)) ==
              FnAddrCrossing::ViaCallee,
          "if the callee hands it out, it crosses through the callee");
}

/// Lo que NO es una direccion de funcion no se mira, aunque vaya a una
/// nativa.  Un analisis que marcara cualquier cosa que cruza estaria
/// contestando otra pregunta.
static void case_not_a_function_address() {
    ir::IrFunction fn;
    fn.name = "un_numero_cualquiera";
    const ir::IrBlockId b = fn.new_block("entry");
    const ir::IrValueId n = fn.new_value(ir::IrType::I64);
    {
        ir::IrInstr in{};
        in.op = ir::IrOp::CONST;
        in.type = ir::IrType::I64;
        in.dst = n;
        in.imm = 42;
        fn.append(b, std::move(in));
    }
    native_call(fn, b, "msvcrt.dll:exit", {n});
    emit(fn, b, ir::IrOp::RET, ir::IR_NO_VALUE, {});
    check(verdict(fn, n) == FnAddrCrossing::None,
          "a number passed to a native call is not a function address");
}

int main() {
    std::printf("== function addresses that must be real ones ==\n");
    case_stays_inside();
    case_passed_to_native();
    case_laundered_by_a_copy();
    case_returned();
    case_to_ours_that_keeps_it();
    case_to_ours_that_hands_it_out();
    case_not_a_function_address();
    std::printf("%d of %d OK\n", g_checks - g_fail, g_checks);
    return g_fail == 0 ? 0 : 1;
}
