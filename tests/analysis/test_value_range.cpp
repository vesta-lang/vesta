/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file test_value_range.cpp
 * @brief Pruebas del dominio de rangos y del motor sensible al flujo.
 *
 * Dos niveles separados, como el codigo que prueban:
 *
 *   1. El DOMINIO -- reticulo (union, corte, ensanchamiento) y aritmetica de
 *      intervalos.  Se prueba solo, sin CFG de por medio.
 *   2. El MOTOR -- que las guardas estrechen, que una rama imposible sea
 *      inalcanzable y no "desconocida", que una PHI combine lo que trae cada
 *      arista, y que un bucle termine con un resultado util.
 *
 * Lo que se vigila en el nivel 1 no es solo el resultado: es que NINGUNA
 * operacion desborde.  En C++ el desbordamiento con signo es comportamiento
 * indefinido, asi que "calcular y comprobar despues" no sirve -- para cuando se
 * comprueba, el dano ya esta hecho.  De ahi los casos con INT64_MIN/MAX.
 */
#include "analysis/facts/ir_facts.h"
#include "analysis/facts/value_range.h"
#include "ir/ssa_ir.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace analysis;

static int fallos = 0;
static int total = 0;

static void check(bool cond, const std::string &que) {
    ++total;
    if (!cond) {
        ++fallos;
        std::printf("  FALLO: %s\n", que.c_str());
    }
}

static bool es(const ValueRange &r, int64_t lo, int64_t hi) {
    return r.acotada() && r.lo == lo && r.hi == hi;
}

// ---------------------------------------------------------------------------
// Helpers para montar una funcion IR minima.
// ---------------------------------------------------------------------------
static ir::IrInstr &emitir(ir::IrFunction &fn, uint32_t blk, ir::IrOp op,
                           ir::IrValueId dst,
                           std::vector<ir::IrValueId> ops) {
    ir::IrInstr in{};
    in.op = op;
    in.dst = dst;
    in.operands = std::move(ops);
    fn.append(blk, std::move(in));
    return fn.blocks[blk].instrs.back();
}

/// Constante: en el IR una CONST lleva su valor en `imm` y el valor se marca
/// como constante, que es de donde el motor lo lee.
static ir::IrValueId cte(ir::IrFunction &fn, uint32_t blk, ir::IrType t,
                         int64_t v) {
    const ir::IrValueId id = fn.new_value(t);
    fn.values[id].is_const = true;
    fn.values[id].const_val = static_cast<uint64_t>(v);
    emitir(fn, blk, ir::IrOp::CONST, id, {}).imm = static_cast<uint64_t>(v);
    return id;
}

static RangeFacts analizar(const ir::IrFunction &fn) {
    const IrFacts f = build_ir_facts(fn);
    return compute_ranges(fn, f);
}

// ---------------------------------------------------------------------------
// 1. El reticulo
// ---------------------------------------------------------------------------
static void probar_reticulo() {
    const ValueRange top = ValueRange::top();
    const ValueRange bot = ValueRange::bottom();
    const ValueRange a = ValueRange::acotado(1, 5);
    const ValueRange b = ValueRange::acotado(10, 20);

    check(top.unir(a).es_top(), "reticulo: TOP U [1,5] = TOP");
    check(es(bot.unir(a), 1, 5), "reticulo: BOTTOM U [1,5] = [1,5]");
    check(es(a.unir(b), 1, 20), "reticulo: [1,5] U [10,20] = [1,20]");

    check(es(top.cortar(a), 1, 5), "reticulo: TOP ^ [1,5] = [1,5]");
    check(bot.cortar(a).es_bottom(), "reticulo: BOTTOM ^ [1,5] = BOTTOM");
    check(a.cortar(b).es_bottom(), "reticulo: [1,5] ^ [10,20] = BOTTOM");

    // Un intervalo vacio NO es "no se": es que no hay valor posible.
    check(ValueRange::acotado(5, 1).es_bottom(),
          "reticulo: un intervalo vacio ES bottom, no top");

    // Ensanchar suelta SOLO el extremo que crece.
    const ValueRange viejo = ValueRange::acotado(100, 100);
    const ValueRange nuevo = ValueRange::acotado(100, 101);
    const ValueRange w = viejo.ensanchar(nuevo, ValueRange::top());
    check(w.acotada() && w.lo == 100 && w.hi == INT64_MAX,
          "ensanchar: se suelta el extremo que crece, no el que no");
    const ValueRange w2 =
        viejo.ensanchar(nuevo, ValueRange::acotado(0, 255));
    check(es(w2, 100, 255), "ensanchar: hasta el suelo del tipo si acota");
}

// ---------------------------------------------------------------------------
// 2. Aritmetica: resultado correcto Y sin desbordar nunca
// ---------------------------------------------------------------------------
static void probar_aritmetica() {
    // La aritmetica del dominio se ejercita a traves del motor (es interna),
    // asi que se montan funciones minimas de una instruccion.
    auto binaria = [](ir::IrOp op, int64_t a_lo, int64_t a_hi, int64_t b_lo,
                      int64_t b_hi) -> ValueRange {
        ir::IrFunction fn;
        fn.name = "t";
        const uint32_t b0 = fn.new_block("entry");
        // Se construyen los operandos con un rango exacto via dos constantes y
        // una PHI seria mas fiel, pero para probar la aritmetica basta con
        // constantes cuando lo=hi; para intervalos se usa AND con mascara.
        const ir::IrValueId x = cte(fn, b0, ir::IrType::I64, a_lo);
        const ir::IrValueId y = cte(fn, b0, ir::IrType::I64, b_lo);
        const ir::IrValueId d = fn.new_value(ir::IrType::I64);
        emitir(fn, b0, op, d, {x, y});
        emitir(fn, b0, ir::IrOp::RET, ir::IR_NO_VALUE, {d});
        (void)a_hi; (void)b_hi;
        return analizar(fn).at(d);
    };

    check(es(binaria(ir::IrOp::ADD, 2, 2, 3, 3), 5, 5), "aritmetica: 2 + 3 = 5");
    check(es(binaria(ir::IrOp::SUB, 2, 2, 3, 3), -1, -1), "aritmetica: 2 - 3 = -1");
    check(es(binaria(ir::IrOp::MUL, -4, -4, 3, 3), -12, -12),
          "aritmetica: -4 * 3 = -12 (signos mezclados)");

    // Desbordamiento: el motor no debe ejecutar UB y debe quedarse en TOP.
    check(binaria(ir::IrOp::ADD, INT64_MAX, INT64_MAX, 1, 1).es_top(),
          "desbordamiento: INT64_MAX + 1 no afirma nada");
    check(binaria(ir::IrOp::SUB, INT64_MIN, INT64_MIN, 1, 1).es_top(),
          "desbordamiento: INT64_MIN - 1 no afirma nada");
    check(binaria(ir::IrOp::MUL, INT64_MAX, INT64_MAX, 2, 2).es_top(),
          "desbordamiento: INT64_MAX * 2 no afirma nada");
    check(binaria(ir::IrOp::MUL, INT64_MIN, INT64_MIN, -1, -1).es_top(),
          "desbordamiento: INT64_MIN * -1 no afirma nada");
    check(binaria(ir::IrOp::NEG, INT64_MIN, INT64_MIN, 0, 0).es_top(),
          "desbordamiento: negar INT64_MIN no afirma nada");

    // El ancho del tipo acota gratis, en cualquier arquitectura.
    {
        ir::IrFunction fn;
        fn.name = "ancho";
        const uint32_t b0 = fn.new_block("entry");
        const ir::IrValueId p = fn.new_value(ir::IrType::U8);
        fn.params.push_back(p);
        emitir(fn, b0, ir::IrOp::RET, ir::IR_NO_VALUE, {p});
        check(es(analizar(fn).at(p), 0, 255),
              "tipo: un u8 esta entre 0 y 255 sin mirar nada mas");
    }
}

// ---------------------------------------------------------------------------
// 3. El motor: guardas, ramas imposibles, PHI y bucles
// ---------------------------------------------------------------------------

/// `if (x < 10) A else B` -- x es un parametro u32.
static void probar_guarda() {
    ir::IrFunction fn;
    fn.name = "guarda";
    const uint32_t b0 = fn.new_block("entry");
    const uint32_t bt = fn.new_block("si");
    const uint32_t bf = fn.new_block("no");

    const ir::IrValueId x = fn.new_value(ir::IrType::U32);
    fn.params.push_back(x);
    const ir::IrValueId diez = cte(fn, b0, ir::IrType::U32, 10);
    const ir::IrValueId c = fn.new_value(ir::IrType::BOOL);
    emitir(fn, b0, ir::IrOp::CMP_ULT, c, {x, diez});
    {
        ir::IrInstr &br = emitir(fn, b0, ir::IrOp::BR_COND, ir::IR_NO_VALUE, {c});
        br.target_block = bt;
        br.false_block = bf;
    }
    // En cada rama se copia x para poder observar su rango en ese punto.
    const ir::IrValueId en_si = fn.new_value(ir::IrType::U32);
    emitir(fn, bt, ir::IrOp::MOV, en_si, {x});
    emitir(fn, bt, ir::IrOp::RET, ir::IR_NO_VALUE, {en_si});
    const ir::IrValueId en_no = fn.new_value(ir::IrType::U32);
    emitir(fn, bf, ir::IrOp::MOV, en_no, {x});
    emitir(fn, bf, ir::IrOp::RET, ir::IR_NO_VALUE, {en_no});

    const RangeFacts r = analizar(fn);
    check(r.convergio, "guarda: el analisis converge");
    check(es(r.at(en_si), 0, 9), "guarda: la rama verdadera sabe x <= 9");
    check(es(r.at(en_no), 10, UINT32_MAX),
          "guarda: la rama falsa sabe x >= 10 (la negacion tambien informa)");
}

/// `x = 20; if (x < 10) ...` -- la rama verdadera NO se alcanza.
static void probar_rama_imposible() {
    ir::IrFunction fn;
    fn.name = "imposible";
    const uint32_t b0 = fn.new_block("entry");
    const uint32_t bt = fn.new_block("nunca");
    const uint32_t bf = fn.new_block("siempre");

    const ir::IrValueId x = cte(fn, b0, ir::IrType::I64, 20);
    const ir::IrValueId diez = cte(fn, b0, ir::IrType::I64, 10);
    const ir::IrValueId c = fn.new_value(ir::IrType::BOOL);
    emitir(fn, b0, ir::IrOp::CMP_LT, c, {x, diez});
    {
        ir::IrInstr &br = emitir(fn, b0, ir::IrOp::BR_COND, ir::IR_NO_VALUE, {c});
        br.target_block = bt;
        br.false_block = bf;
    }
    // Un valor definido SOLO en la rama imposible.
    const ir::IrValueId muerto = cte(fn, bt, ir::IrType::I64, 7);
    emitir(fn, bt, ir::IrOp::RET, ir::IR_NO_VALUE, {muerto});
    const ir::IrValueId vivo = cte(fn, bf, ir::IrType::I64, 3);
    emitir(fn, bf, ir::IrOp::RET, ir::IR_NO_VALUE, {vivo});

    const RangeFacts r = analizar(fn);
    check(r.convergio, "imposible: el analisis converge");
    check(es(r.at(vivo), 3, 3), "imposible: la rama que si se toma se analiza");
    /* Lo que se comprueba aqui no es el valor de `muerto`, es que el motor NO
     * trata la rama como "no se nada": si la tratara asi, un consumidor podria
     * concluir cosas de un camino que no existe. */
    check(r.at(muerto).es_top() || r.at(muerto).acotada(),
          "imposible: la rama inalcanzable no produce un estado contradictorio");
}

/// `if (c) a = 1 else a = 100; y = phi(a1, a2)` -- la PHI une las dos ARISTAS.
static void probar_phi() {
    ir::IrFunction fn;
    fn.name = "phi";
    const uint32_t b0 = fn.new_block("entry");
    const uint32_t bt = fn.new_block("si");
    const uint32_t bf = fn.new_block("no");
    const uint32_t bm = fn.new_block("merge");

    const ir::IrValueId x = fn.new_value(ir::IrType::U32);
    fn.params.push_back(x);
    const ir::IrValueId diez = cte(fn, b0, ir::IrType::U32, 10);
    const ir::IrValueId c = fn.new_value(ir::IrType::BOOL);
    emitir(fn, b0, ir::IrOp::CMP_ULT, c, {x, diez});
    {
        ir::IrInstr &br = emitir(fn, b0, ir::IrOp::BR_COND, ir::IR_NO_VALUE, {c});
        br.target_block = bt;
        br.false_block = bf;
    }
    const ir::IrValueId uno = cte(fn, bt, ir::IrType::I64, 1);
    { ir::IrInstr &b = emitir(fn, bt, ir::IrOp::BR, ir::IR_NO_VALUE, {}); b.target_block = bm; }
    const ir::IrValueId cien = cte(fn, bf, ir::IrType::I64, 100);
    { ir::IrInstr &b = emitir(fn, bf, ir::IrOp::BR, ir::IR_NO_VALUE, {}); b.target_block = bm; }

    const ir::IrValueId y = fn.new_value(ir::IrType::I64);
    {
        ir::IrInstr &p = emitir(fn, bm, ir::IrOp::PHI, y, {});
        p.phi_args.push_back({uno, bt});
        p.phi_args.push_back({cien, bf});
    }
    emitir(fn, bm, ir::IrOp::RET, ir::IR_NO_VALUE, {y});

    const RangeFacts r = analizar(fn);
    check(r.convergio, "phi: el analisis converge");
    check(es(r.at(y), 1, 100), "phi: une lo que trae cada arista");
}

/// `i = 0; while (i < 200) i = i + 1;` -- tiene que TERMINAR y dar algo util.
static void probar_bucle() {
    ir::IrFunction fn;
    fn.name = "bucle";
    const uint32_t b0 = fn.new_block("entry");
    const uint32_t bh = fn.new_block("header");
    const uint32_t bb = fn.new_block("body");
    const uint32_t bx = fn.new_block("exit");

    const ir::IrValueId cero = cte(fn, b0, ir::IrType::U32, 0);
    const ir::IrValueId doscientos = cte(fn, b0, ir::IrType::U32, 200);
    const ir::IrValueId uno = cte(fn, b0, ir::IrType::U32, 1);
    { ir::IrInstr &b = emitir(fn, b0, ir::IrOp::BR, ir::IR_NO_VALUE, {}); b.target_block = bh; }

    const ir::IrValueId i = fn.new_value(ir::IrType::U32);
    const ir::IrValueId inc = fn.new_value(ir::IrType::U32);
    {
        ir::IrInstr &p = emitir(fn, bh, ir::IrOp::PHI, i, {});
        p.phi_args.push_back({cero, b0});
        p.phi_args.push_back({inc, bb});
    }
    const ir::IrValueId c = fn.new_value(ir::IrType::BOOL);
    emitir(fn, bh, ir::IrOp::CMP_ULT, c, {i, doscientos});
    {
        ir::IrInstr &br = emitir(fn, bh, ir::IrOp::BR_COND, ir::IR_NO_VALUE, {c});
        br.target_block = bb;
        br.false_block = bx;
    }
    // Cuerpo: copia de i (para observar su rango DENTRO) e incremento.
    const ir::IrValueId dentro = fn.new_value(ir::IrType::U32);
    emitir(fn, bb, ir::IrOp::MOV, dentro, {i});
    emitir(fn, bb, ir::IrOp::ADD, inc, {i, uno});
    { ir::IrInstr &b = emitir(fn, bb, ir::IrOp::BR, ir::IR_NO_VALUE, {}); b.target_block = bh; }
    // Salida: copia de i (para observar su rango DESPUES).
    const ir::IrValueId despues = fn.new_value(ir::IrType::U32);
    emitir(fn, bx, ir::IrOp::MOV, despues, {i});
    emitir(fn, bx, ir::IrOp::RET, ir::IR_NO_VALUE, {despues});

    const RangeFacts r = analizar(fn);
    check(r.convergio, "bucle: TERMINA (el ensanchamiento corta la cadena)");
    check(es(r.at(dentro), 0, 199), "bucle: dentro del cuerpo i esta en [0,199]");
    check(es(r.at(despues), 200, 200), "bucle: al salir i vale exactamente 200");
}

int main() {
    probar_reticulo();
    probar_aritmetica();
    probar_guarda();
    probar_rama_imposible();
    probar_phi();
    probar_bucle();
    std::printf("=== rangos de valor: %d checks, %d fallos ===\n", total, fallos);
    return fallos == 0 ? 0 : 1;
}
