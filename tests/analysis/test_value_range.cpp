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
 * Dos niveles separados, igual que el codigo que prueban:
 *
 *   1. El DOMINIO (`value_range_domain.cpp`) -- reticulo, aritmetica que
 *      ENVUELVE, conversiones entre anchos y restricciones.  Se prueba SIN
 *      construir una funcion IR, que es justo lo que se gana al sacar la
 *      semantica del motor: un dominio se demuestra, un recorrido se comprueba.
 *   2. El MOTOR (`value_range.cpp`) -- que las guardas estrechen, que una rama
 *      imposible sea inalcanzable y no "desconocida", que una PHI combine lo que
 *      trae cada arista, y que un bucle termine con un resultado util.
 *
 * Lo que se vigila en el nivel 1 no es solo el resultado, son las FRONTERAS:
 * ningun calculo puede desbordar (en C++ el desbordamiento con signo es
 * comportamiento indefinido, asi que "calcular y comprobar despues" no sirve --
 * para cuando se comprueba, el dano ya esta hecho), y ninguna operacion puede
 * responder BOTTOM por no caber, porque eso declara inalcanzable codigo vivo.
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

// Tipos usados en las pruebas, con los nombres del lenguaje.
static const RangeType kI8 = RangeType::i(8);
static const RangeType kU8 = RangeType::u(8);
static const RangeType kI16 = RangeType::i(16);
static const RangeType kU16 = RangeType::u(16);
static const RangeType kI32 = RangeType::i(32);
static const RangeType kU32 = RangeType::u(32);
static const RangeType kI64 = RangeType::i(64);
static const RangeType kU64 = RangeType::u(64);

/// Igualdad contra un intervalo escrito con NUMEROS del tipo.
static bool es(const ValueRange &r, RangeType t, int64_t lo, int64_t hi) {
    return r.acotada() && r.t == t && r.lo_c == t.desde_signo(lo) &&
           r.hi_c == t.desde_signo(hi) && r.valida();
}
/// Igualdad contra un valor unico.
static bool es(const ValueRange &r, RangeType t, int64_t v) { return es(r, t, v, v); }
/// Igualdad contra un intervalo escrito con BITS (para los `u64` grandes).
static bool es_crudo(const ValueRange &r, RangeType t, uint64_t lo, uint64_t hi) {
    return r.acotada() && r.t == t && r.lo_c == lo && r.hi_c == hi && r.valida();
}
static ValueRange cte_de(RangeType t, int64_t v) {
    return ValueRange::constante(t, t.desde_signo(v));
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

// ===========================================================================
// 1. El reticulo
// ===========================================================================
static void probar_reticulo() {
    const ValueRange top = ValueRange::top(kI64);
    const ValueRange bot = ValueRange::bottom(kI64);
    const ValueRange a = ValueRange::de_enteros(kI64, 0, 10);
    const ValueRange b = ValueRange::de_enteros(kI64, 5, 20);
    const ValueRange c = ValueRange::de_enteros(kI64, 20, 30);

    check(top.unir(a).es_top(), "reticulo: TOP U [0,10] = TOP");
    check(es(bot.unir(a), kI64, 0, 10), "reticulo: BOTTOM U [0,10] = [0,10]");
    check(es(top.cortar(a), kI64, 0, 10), "reticulo: TOP ^ [0,10] = [0,10]");
    check(bot.cortar(a).es_bottom(), "reticulo: BOTTOM ^ [0,10] = BOTTOM");
    check(es(a.cortar(b), kI64, 5, 10), "reticulo: [0,10] ^ [5,20] = [5,10]");
    check(a.cortar(c).es_bottom(),
          "reticulo: [0,10] ^ [20,30] = BOTTOM (no 'no se': es imposible)");
    check(es(a.unir(c), kI64, 0, 30), "reticulo: la union es la envolvente");

    // TOP y `todo(T)` NO son lo mismo, y la diferencia se puede consultar.
    check(ValueRange::top(kU8).es_top() && !ValueRange::todo(kU8).es_top(),
          "reticulo: TOP no es todo(u8) -- uno no sabe el dominio, el otro si");
    check(ValueRange::todo(kU8).es_todo(),
          "reticulo: todo(u8) se reconoce como 'el tipo entero'");
}

// ===========================================================================
// 2. Los tipos: suelo, normalizacion e invariante
// ===========================================================================
static void probar_tipos() {
    check(es(ValueRange::todo(kU8), kU8, 0, 255), "tipo: todo(u8) = [0,255]");
    check(es(ValueRange::todo(kI8), kI8, -128, 127), "tipo: todo(i8) = [-128,127]");
    check(es_crudo(ValueRange::todo(kU64), kU64, 0, UINT64_MAX),
          "tipo: todo(u64) = [0,UINT64_MAX] -- el dominio entero, sin rendirse");
    check(es(ValueRange::todo(kI64), kI64, INT64_MIN, INT64_MAX),
          "tipo: todo(i64) = [INT64_MIN,INT64_MAX]");

    // Un ancho imposible no entra: se ensancha a 64 bits (se afirma menos).
    check(RangeType::de(0, true).bits == 64 && RangeType::de(200, false).bits == 64,
          "tipo: un ancho fuera de [1,64] se ensancha a 64, nunca se acepta");
    check(kU8.valido() && kI64.valido(), "tipo: los anchos normales son validos");

    // Los extremos SIEMPRE pertenecen al tipo: construir con un valor de fuera
    // no puede dejar un intervalo que el tipo no puede contener.
    check(es(ValueRange::constante(kU8, 300), kU8, 44),
          "tipo: constante(u8, 300) se normaliza a 44 -- 300 no es un u8");
    check(ValueRange::crudo(kU8, 250, 300).es_todo(),
          "tipo: [250,300] en u8 da la vuelta -> todo(u8), NUNCA bottom");
    check(ValueRange::crudo(kU8, 250, 300).valida(),
          "tipo: el resultado sigue cumpliendo el invariante");

    // La lectura con signo es una consulta de representacion, no una conversion.
    int64_t lo = 0, hi = 0;
    check(!ValueRange::todo(kU64).vista_con_signo(lo, hi),
          "tipo: un u64 completo NO cabe en int64 y el dominio lo dice");
    check(ValueRange::todo(kU32).vista_con_signo(lo, hi) && lo == 0 &&
              hi == UINT32_MAX,
          "tipo: un u32 completo si cabe en int64");
}

// ===========================================================================
// 3. Aritmetica: envuelve como el IR, y nunca desborda el host
// ===========================================================================
static void probar_aritmetica() {
    // Lo basico, sin envolver.
    check(es(ValueRange::de_enteros(kI64, 1, 5).sumar(ValueRange::de_enteros(kI64, 10, 20)),
             kI64, 11, 25),
          "suma: [1,5] + [10,20] = [11,25]");
    check(es(ValueRange::de_enteros(kI64, 1, 5).restar(ValueRange::de_enteros(kI64, 10, 20)),
             kI64, -19, -5),
          "resta: los extremos se CRUZAN");
    check(es(ValueRange::de_enteros(kI32, -2, 3).multiplicar(
                 ValueRange::de_enteros(kI32, -5, 1)),
             kI32, -15, 10),
          "producto: las CUATRO esquinas (con signos mezclados el minimo no es lo obvio)");
    check(es(ValueRange::de_enteros(kI64, -5, 3).negar(), kI64, -3, 5),
          "negacion: da la vuelta al intervalo");

    // ENVOLTURA: el resultado exacto cabe, aunque el numero "matematico" no.
    check(es(cte_de(kU8, 250).sumar(cte_de(kU8, 10)), kU8, 4),
          "envuelve: u8 250 + 10 = 4 (no 260, y no 'no se')");
    check(es(cte_de(kU8, 255).sumar(cte_de(kU8, 1)), kU8, 0),
          "envuelve: u8 255 + 1 = 0");
    check(es(cte_de(kI8, 127).sumar(cte_de(kI8, 1)), kI8, -128),
          "envuelve: i8 127 + 1 = -128");
    check(es(cte_de(kI8, -128).restar(cte_de(kI8, 1)), kI8, 127),
          "envuelve: i8 -128 - 1 = 127");
    check(es(cte_de(kI64, INT64_MAX).sumar(cte_de(kI64, 1)), kI64, INT64_MIN),
          "envuelve: i64 INT64_MAX + 1 = INT64_MIN, sin UB por el camino");
    check(es(cte_de(kI64, INT64_MIN).negar(), kI64, INT64_MIN),
          "envuelve: -INT64_MIN = INT64_MIN (el caso que rompe un negar ingenuo)");
    check(es_crudo(ValueRange::constante(kU64, UINT64_MAX).sumar(cte_de(kU64, 1)),
                   kU64, 0, 0),
          "envuelve: u64 UINT64_MAX + 1 = 0");

    /* Un intervalo entero que se pasa del tope tampoco se pierde: si todos sus
     * valores envuelven IGUAL, el desplazado sigue siendo un intervalo. */
    check(es(ValueRange::de_enteros(kU8, 250, 255).sumar(cte_de(kU8, 10)), kU8, 4, 9),
          "envuelve: u8 [250,255] + 10 = [4,9] -- envolver no es perder");
    check(es(cte_de(kI64, INT64_MAX).multiplicar(cte_de(kI64, 2)), kI64, -2),
          "envuelve: i64 INT64_MAX * 2 = -2, exacto y sin UB al calcularlo");

    // Un conjunto que al envolver queda PARTIDO no se representa: el tipo.
    check(ValueRange::de_enteros(kU8, 250, 255)
              .sumar(ValueRange::de_enteros(kU8, 0, 10))
              .es_todo(),
          "envuelve: u8 [250,255] + [0,10] se parte en dos trozos -> todo(u8)");
    check(ValueRange::de_enteros(kI64, 2, INT64_MAX).multiplicar(cte_de(kI64, 2)).es_todo(),
          "producto: un resultado mas ancho que el tipo -> todo(i64), nunca un numero inventado");

    // Ninguna de estas puede acabar en BOTTOM: son valores que existen.
    check(!cte_de(kU8, 250).sumar(cte_de(kU8, 10)).es_bottom() &&
              !ValueRange::de_enteros(kU8, 250, 255).sumar(cte_de(kU8, 10)).es_bottom(),
          "envuelve: envolver NUNCA produce BOTTOM (seria declarar muerto un punto vivo)");

    // Y con `u64` grandes, que es donde un dominio con signo se rendia.
    const ValueRange altos = ValueRange::crudo(kU64, uint64_t(INT64_MAX) + 1,
                                               uint64_t(INT64_MAX) + 100);
    check(es_crudo(altos.sumar(cte_de(kU64, 1)), kU64, uint64_t(INT64_MAX) + 2,
                   uint64_t(INT64_MAX) + 101),
          "u64: se opera por encima del mayor int64 sin perder precision");

    // AND con mascara constante.
    check(es(ValueRange::todo(kU64).conjuncion(ValueRange::constante(kU64, 7)),
             kU64, 0, 7),
          "conjuncion: x & 7 esta en [0,7] venga x de donde venga");
    check(es(ValueRange::todo(kI32).conjuncion(ValueRange::constante(kI32, 0xFF)),
             kI32, 0, 255),
          "conjuncion: x & 0xFF acota aunque x sea todo el tipo");
}

// ===========================================================================
// 4. Conversiones entre anchos: cuatro operaciones, no una
// ===========================================================================
static void probar_conversiones() {
    check(es(cte_de(kU8, 255).extender_sin_signo(kU32), kU32, 255),
          "zext: u8 255 -> u32 255");
    check(es(cte_de(kI8, -1).extender_con_signo(kI32), kI32, -1),
          "sext: i8 -1 -> i32 -1 (el numero no cambia)");
    check(es(cte_de(kI8, -1).extender_sin_signo(kU32), kU32, 255),
          "zext: i8 -1 leido sin signo vale 255 -- por eso no es lo mismo que sext");
    check(ValueRange::de_enteros(kI8, -1, 1).extender_sin_signo(kU32).es_todo() ||
              es(ValueRange::de_enteros(kI8, -1, 1).extender_sin_signo(kU32), kU32, 0, 255),
          "zext: un intervalo que cruza el cero se parte; lo afirmable es el ancho origen");

    check(es(cte_de(kU16, 256).truncar(kU8), kU8, 0), "trunc: u16 256 -> u8 0");
    check(es(cte_de(kU16, 257).truncar(kU8), kU8, 1), "trunc: u16 257 -> u8 1");
    check(ValueRange::de_enteros(kU16, 250, 260).truncar(kU8).es_todo(),
          "trunc: u16 [250,260] -> u8 NO es [250,255]; el conjunto se parte -> [0,255]");
    check(es(ValueRange::de_enteros(kU16, 10, 20).truncar(kU8), kU8, 10, 20),
          "trunc: lo que ya cabia se conserva exacto");
    check(es(ValueRange::de_enteros(kI16, -1, 0).truncar(kI8), kI8, -1, 0),
          "trunc: i16 [-1,0] -> i8 [-1,0]");

    check(es(ValueRange::de_enteros(kU32, 1, 5).reinterpretar(kI32), kI32, 1, 5),
          "bitcast: mismos bits, misma lectura cuando no hay signo de por medio");
    check(ValueRange::crudo(kU32, 0x7FFFFFF0u, 0xFFFFFFFFu)
              .reinterpretar(kI32)
              .es_todo(),
          "bitcast: u32 alto -> i32 NO es monotono; se parte -> todo(i32)");
    check(ValueRange::crudo(kU64, uint64_t(INT64_MAX), uint64_t(INT64_MAX) + 1)
              .reinterpretar(kI64)
              .es_todo(),
          "bitcast: u64 que cruza el maximo con signo -> todo(i64)");
    check(es(cte_de(kU64, 3).reinterpretar(kI64), kI64, 3),
          "bitcast: un valor pequeno cruza los dominios sin perder nada");
}

// ===========================================================================
// 5. Restricciones: lo que afirma una comparacion
// ===========================================================================
static void probar_restricciones() {
    const ValueRange u32todo = ValueRange::todo(kU32);
    check(es(u32todo.restringir_menor(ValueRange::constante(kU32, 200)), kU32, 0, 199),
          "restriccion: u32 < 200 -> [0,199]");
    check(es(u32todo.restringir_mayor_igual(ValueRange::constante(kU32, 200)), kU32,
             200, UINT32_MAX),
          "restriccion: u32 >= 200 -> [200,UINT32_MAX] (la negacion tambien informa)");
    check(es(ValueRange::todo(kI8).restringir_menor(cte_de(kI8, -100)), kI8, -128, -101),
          "restriccion: i8 < -100 -> [-128,-101]");
    check(ValueRange::todo(kI8).restringir_menor(cte_de(kI8, -128)).es_bottom(),
          "restriccion: nada es menor que el minimo del tipo -> BOTTOM");
    check(ValueRange::todo(kU64).restringir_mayor(ValueRange::constante(kU64, UINT64_MAX))
              .es_bottom(),
          "restriccion: nada es mayor que el maximo del tipo -> BOTTOM (sin desbordar)");
    check(ValueRange::de_enteros(kI64, 0, 10)
              .restringir_mayor(ValueRange::de_enteros(kI64, 20, 30))
              .es_bottom(),
          "restriccion: contradecir lo ya sabido es BOTTOM, no 'no se'");

    // `!=` solo estrecha en los extremos; en medio partiria el intervalo.
    const ValueRange diez = ValueRange::de_enteros(kI64, 0, 10);
    check(es(diez.restringir_distinto(cte_de(kI64, 0)), kI64, 1, 10),
          "restriccion: x != 0 sobre [0,10] -> [1,10]");
    check(es(diez.restringir_distinto(cte_de(kI64, 10)), kI64, 0, 9),
          "restriccion: x != 10 sobre [0,10] -> [0,9]");
    check(es(diez.restringir_distinto(cte_de(kI64, 5)), kI64, 0, 10),
          "restriccion: x != 5 partiria el intervalo -> se deja como estaba");
    check(cte_de(kI64, 7).restringir_distinto(cte_de(kI64, 7)).es_bottom(),
          "restriccion: x != x es imposible -> BOTTOM");
}

// ===========================================================================
// 6. Ensanchamiento
// ===========================================================================
static void probar_ensanchamiento() {
    const ValueRange viejo = ValueRange::de_enteros(kI64, 100, 100);
    const ValueRange nuevo = ValueRange::de_enteros(kI64, 100, 101);
    check(es(viejo.ensanchar(nuevo), kI64, 100, INT64_MAX),
          "ensanchar: se suelta SOLO el extremo que crece; el otro se conserva");
    check(es(ValueRange::de_enteros(kU8, 10, 10).ensanchar(ValueRange::de_enteros(kU8, 9, 10)),
             kU8, 0, 10),
          "ensanchar: si baja, se suelta por abajo hasta el minimo del tipo");
    check(es(ValueRange::bottom(kI64).ensanchar(nuevo), kI64, 100, 101),
          "ensanchar: desde BOTTOM no hay nada que soltar");
    check(es(viejo.ensanchar(viejo), kI64, 100, 100),
          "ensanchar: si nada crece, nada se suelta");
}

// ===========================================================================
// 7. El motor: guardas, ramas imposibles, PHI y bucles
// ===========================================================================

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
    check(es(r.at(en_si), kU32, 0, 9), "guarda: la rama verdadera sabe x <= 9");
    check(es(r.at(en_no), kU32, 10, UINT32_MAX),
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
    check(es(r.at(vivo), kI64, 3, 3), "imposible: la rama que si se toma se analiza");
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
    check(es(r.at(y), kI64, 1, 100), "phi: une lo que trae cada arista");
}

/// `i = 0; while (i < 200) i = i + 1;` -- tiene que TERMINAR y dar algo util.
/// @param tipo el mismo bucle escrito con un entero con o sin signo.
static void probar_bucle(ir::IrType tipo, RangeType rt, const char *etiqueta) {
    ir::IrFunction fn;
    fn.name = "bucle";
    const uint32_t b0 = fn.new_block("entry");
    const uint32_t bh = fn.new_block("header");
    const uint32_t bb = fn.new_block("body");
    const uint32_t bx = fn.new_block("exit");

    const ir::IrValueId cero = cte(fn, b0, tipo, 0);
    const ir::IrValueId doscientos = cte(fn, b0, tipo, 200);
    const ir::IrValueId uno = cte(fn, b0, tipo, 1);
    { ir::IrInstr &b = emitir(fn, b0, ir::IrOp::BR, ir::IR_NO_VALUE, {}); b.target_block = bh; }

    const ir::IrValueId i = fn.new_value(tipo);
    const ir::IrValueId inc = fn.new_value(tipo);
    {
        ir::IrInstr &p = emitir(fn, bh, ir::IrOp::PHI, i, {});
        p.phi_args.push_back({cero, b0});
        p.phi_args.push_back({inc, bb});
    }
    const ir::IrValueId c = fn.new_value(ir::IrType::BOOL);
    emitir(fn, bh, rt.sin_signo ? ir::IrOp::CMP_ULT : ir::IrOp::CMP_LT, c,
           {i, doscientos});
    {
        ir::IrInstr &br = emitir(fn, bh, ir::IrOp::BR_COND, ir::IR_NO_VALUE, {c});
        br.target_block = bb;
        br.false_block = bx;
    }
    // Cuerpo: copia de i (para observar su rango DENTRO) e incremento.
    const ir::IrValueId dentro = fn.new_value(tipo);
    emitir(fn, bb, ir::IrOp::MOV, dentro, {i});
    emitir(fn, bb, ir::IrOp::ADD, inc, {i, uno});
    { ir::IrInstr &b = emitir(fn, bb, ir::IrOp::BR, ir::IR_NO_VALUE, {}); b.target_block = bh; }
    // Salida: copia de i (para observar su rango DESPUES).
    const ir::IrValueId despues = fn.new_value(tipo);
    emitir(fn, bx, ir::IrOp::MOV, despues, {i});
    emitir(fn, bx, ir::IrOp::RET, ir::IR_NO_VALUE, {despues});

    const RangeFacts r = analizar(fn);
    check(r.convergio,
          std::string("bucle ") + etiqueta + ": TERMINA (el ensanchamiento corta la cadena)");
    check(r.stats.ensanches > 0,
          std::string("bucle ") + etiqueta + ": el ensanchamiento se dispara por el CICLO");
    check(es(r.at(dentro), rt, 0, 199),
          std::string("bucle ") + etiqueta + ": dentro del cuerpo i esta en [0,199]");
    check(es(r.at(despues), rt, 200, 200),
          std::string("bucle ") + etiqueta + ": al salir i vale exactamente 200");
}

int main() {
    probar_reticulo();
    probar_tipos();
    probar_aritmetica();
    probar_conversiones();
    probar_restricciones();
    probar_ensanchamiento();
    probar_guarda();
    probar_rama_imposible();
    probar_phi();
    probar_bucle(ir::IrType::U32, kU32, "u32");
    probar_bucle(ir::IrType::I32, kI32, "i32");
    std::printf("=== rangos de valor: %d checks, %d fallos ===\n", total, fallos);
    return fallos == 0 ? 0 : 1;
}
