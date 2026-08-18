/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/codegen/test_vm_problem.cpp
 * @brief El camino del INTERPRETE entra al modelo del allocator:
 *        @c ir::LivenessResult -> @c AbstractProblem.
 *
 * Lo que se comprueba es el CONTRATO del adaptador, no que compile:
 *   - los intervalos entran en SU dominio (IR: 1 posicion por instruccion) y
 *     con el mismo criterio inclusivo, sin reescalar,
 *   - @c crosses_call sale de las posiciones de llamada linealizadas,
 *   - el adaptador es FINO: no inventa valores, no los reordena y no toca nada
 *     que el IR no dijera.
 *
 * Ese ultimo punto es el que hay que vigilar: si el adaptador empezara a
 * "arreglar" el problema, la logica del allocator volveria a repartirse entre
 * dos sitios -- exactamente lo que este refactor viene a eliminar.
 */

#include "codegen/vm_problem.h"
#include "ir/liveness.h"
#include "ir/ssa_ir.h"

#include <cstdio>

using namespace ir;

static int g_checks = 0, g_fails = 0;
#define CHECK(c)                                                               \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(c)) {                                                            \
            ++g_fails;                                                         \
            std::printf("  FALLO L%d: %s\n", __LINE__, #c);                    \
        }                                                                      \
    } while (0)

namespace {

/// Busca el valor @p id dentro del problema (o nullptr).
const codegen::rbank::AbstractValue *
find(const codegen::rbank::AbstractProblem &p, IrValueId id) {
    for (const auto &v : p.values)
        if (v.value_id == id) return &v;
    return nullptr;
}

/// Funcion minima de un solo bloque con @p n instrucciones, la @p call_at de
/// ellas una llamada (o SIZE_MAX para ninguna).
IrFunction make_fn(size_t n, size_t call_at) {
    IrFunction fn;
    fn.name = "t";
    fn.blocks.emplace_back();
    for (size_t i = 0; i < n; ++i) {
        IrInstr in;
        in.op = (i == call_at) ? IrOp::CALL : IrOp::NOP;
        in.dst = IR_NO_VALUE;
        fn.blocks[0].instrs.push_back(in);
    }
    return fn;
}

/// Vivacidad sintetica: un bloque, @p n instrucciones, intervalos dados.
LivenessResult make_live(size_t n,
                         const std::vector<std::pair<uint32_t, uint32_t>> &iv) {
    LivenessResult l;
    l.num_instrs = static_cast<uint32_t>(n);
    l.block_start = {0};
    l.block_end = {static_cast<uint32_t>(n ? n - 1 : 0)};
    IrValueId id = 0;
    for (const auto &pr : iv)
        l.intervals.push_back({id++, pr.first, pr.second});
    return l;
}

} // namespace

int main() {
    std::printf("=== test_vm_problem (el interprete entra al modelo) ===\n");

    /* --- 1. Los intervalos entran SIN reescalar, en dominio IR ------------
     * Es la comprobacion que separa este adaptador del del MachineIR: alli una
     * instruccion ocupa DOS posiciones (use=2*gi, def=2*gi+1) y aqui UNA.  Si
     * alguien "reutilizara" el otro adaptador, los intervalos saldrian con el
     * doble de longitud y el modelo veria interferencias que no existen. */
    {
        const IrFunction fn = make_fn(10, SIZE_MAX);
        const LivenessResult live = make_live(10, {{2, 7}, {0, 3}, {5, 5}});
        const codegen::rbank::AbstractProblem p =
            codegen::liveness_to_problem(fn, live);

        CHECK(p.values.size() == 3);
        const auto *a = find(p, 0), *b = find(p, 1), *c = find(p, 2);
        CHECK(a && a->start == 2 && a->end == 7); // tal cual, sin *2 ni +1
        CHECK(b && b->start == 0 && b->end == 3);
        CHECK(c && c->start == 5 &&
              c->end == 5); // vive UNA posicion (inclusivo)
        // Longitud preservada: end-start+1 es lo que dijo el IR.
        if (a) CHECK(a->end - a->start + 1 == 6);
    }

    /* --- 2. crosses_call sale de las llamadas, con covers INCLUSIVO ------- */
    {
        const IrFunction fn = make_fn(10, /*call_at=*/5);
        const LivenessResult live = make_live(10, {{2, 7},   // cubre la llamada
                                                   {0, 4},   // acaba antes
                                                   {6, 9},   // empieza despues
                                                   {5, 5}}); // JUSTO en ella
        const codegen::rbank::AbstractProblem p =
            codegen::liveness_to_problem(fn, live);

        const auto *cross = find(p, 0), *before = find(p, 1),
                   *after = find(p, 2), *exact = find(p, 3);
        CHECK(cross && cross->req.crosses_call);
        CHECK(before && !before->req.crosses_call);
        CHECK(after && !after->req.crosses_call);
        // covers(p) es def <= p <= end: el borde CUENTA.  Si se tratara como
        // semiabierto, un valor definido justo en la llamada se creeria a
        // salvo.
        CHECK(exact && exact->req.crosses_call);
    }

    /* --- 3. Sin llamadas, nadie cruza ------------------------------------- */
    {
        const IrFunction fn = make_fn(6, SIZE_MAX);
        const LivenessResult live = make_live(6, {{0, 5}, {1, 2}});
        const codegen::rbank::AbstractProblem p =
            codegen::liveness_to_problem(fn, live);
        for (const auto &v : p.values)
            CHECK(!v.req.crosses_call);
    }

    /* --- 4. Adaptador FINO: ni inventa ni reordena ------------------------
     * Los value_id son los IrValueId directamente (el modelo los trata como
     * opacos), asi que el resultado vuelve al emisor sin tabla de traduccion.
     */
    {
        const IrFunction fn = make_fn(8, SIZE_MAX);
        const LivenessResult live = make_live(8, {{0, 1}, {2, 3}, {4, 5}});
        const codegen::rbank::AbstractProblem p =
            codegen::liveness_to_problem(fn, live);
        CHECK(p.values.size() == live.intervals.size()); // ni uno mas
        for (size_t i = 0; i < p.values.size(); ++i) {
            CHECK(p.values[i].value_id == live.intervals[i].id); // mismo orden
            CHECK(p.values[i].req.value_id == p.values[i].value_id);
            CHECK(p.values[i].req.cls == codegen::rbank::ResourceClass::GP);
            // Sin pines mientras se corra en SOMBRA: meterlos cambiaria lo que
            // el problema es el que resuelve el asignador del interprete.
            CHECK(p.values[i].req.fixed_reg == -1);
        }
        CHECK(p.affinity.edges.empty()); // no se inventan afinidades
    }

    /* --- 5. Un intervalo VACIO no entra ----------------------------------- */
    {
        const IrFunction fn = make_fn(4, SIZE_MAX);
        LivenessResult live = make_live(4, {{1, 2}});
        live.intervals.push_back(
            {99, 3, 2}); // end < def -> no es un valor vivo
        const codegen::rbank::AbstractProblem p =
            codegen::liveness_to_problem(fn, live);
        CHECK(p.values.size() == 1);
        CHECK(find(p, 99) == nullptr);
    }

    /* --- 6. Pines de la convencion de llamada -----------------------------
     * params[i] vive en r(i+1) hasta 12; del 13 en adelante NO cabe en registro
     * y es memoria.  Es una restriccion del ABI de la VM, no una decision del
     * asignador, asi que tiene que estar EN el problema: sin ella el modelo no
     * sabe que un parametro esta clavado y mueve otra cosa (medido: +1 derrame
     * en 12 funciones del corpus antes de anyadirlos). */
    {
        IrFunction fn = make_fn(4, SIZE_MAX);
        fn.params = {10, 11, 12};
        LivenessResult live = make_live(4, {});
        live.intervals = {{10, 0, 3}, {11, 0, 3}, {12, 0, 3}, {50, 1, 2}};
        const codegen::rbank::AbstractProblem p =
            codegen::liveness_to_problem(fn, live);

        const auto *p0 = find(p, 10), *p1 = find(p, 11), *p2 = find(p, 12);
        CHECK(p0 && p0->req.fixed_reg == 1); // params[0] -> r1
        CHECK(p1 && p1->req.fixed_reg == 2);
        CHECK(p2 && p2->req.fixed_reg == 3);
        CHECK(p0 && p0->req.has_fixed_reg());
        // Un valor que NO es parametro no lleva pin.
        const auto *v = find(p, 50);
        CHECK(v && v->req.fixed_reg == -1);
        CHECK(v && !v->req.must_be_memory());
    }

    /* --- 7. Mas de 12 parametros: los extra son MEMORIA --------------------
     */
    {
        IrFunction fn = make_fn(4, SIZE_MAX);
        LivenessResult live = make_live(4, {});
        for (IrValueId i = 0; i < 14; ++i) {
            fn.params.push_back(i);
            live.intervals.push_back({i, 0, 3});
        }
        const codegen::rbank::AbstractProblem p =
            codegen::liveness_to_problem(fn, live);

        for (IrValueId i = 0; i < 12; ++i) {
            const auto *v = find(p, i);
            CHECK(v && v->req.fixed_reg == static_cast<int16_t>(i + 1));
            CHECK(v && !v->req.must_be_memory());
        }
        // El 13o y el 14o no caben en r1-r12: memoria, sin pin.
        for (IrValueId i = 12; i < 14; ++i) {
            const auto *v = find(p, i);
            CHECK(v && v->req.fixed_reg == -1);
            CHECK(v && v->req.must_be_memory());
        }
    }

    /* --- 8. Los clobbers IMPLICITOS de R0 llegan al problema --------------
     * Muchas instrucciones de la VM dejan su resultado o su estado en R0 sin
     * que R0 sea operando.  Un valor vivo a traves de una de ellas NO puede
     * vivir ahi.  El modelo lo entiende con su regla de siempre -- cruzar un
     * punto que destruye una lane volatil -- pero solo si el adaptador le
     * cuenta esas posiciones, que es lo que se comprueba aqui.
     *
     * Sin esto el asignador reparte R0 como cualquier otro y el valor se
     * pierde: paso de verdad, con corrupcion de heap y cuelgues en el corpus.
     */
    {
        // Instruccion 3 = DEFFIELD (deja 1/0 en R0), sin ninguna CALL.
        IrFunction fn = make_fn(8, SIZE_MAX);
        fn.blocks[0].instrs[3].op = IrOp::DEFFIELD;
        const LivenessResult live = make_live(8, {{1, 6},   // cruza el clobber
                                                  {0, 2},   // acaba antes
                                                  {4, 7},   // empieza despues
                                                  {3, 3}}); // JUSTO en el
        const codegen::rbank::AbstractProblem p =
            codegen::liveness_to_problem(fn, live);

        const auto *cross = find(p, 0), *before = find(p, 1),
                   *after = find(p, 2), *exact = find(p, 3);
        CHECK(cross && cross->req.crosses_call);
        CHECK(before && !before->req.crosses_call);
        CHECK(after && !after->req.crosses_call);
        CHECK(exact &&
              exact->req.crosses_call); // el borde CUENTA, como en un call

        // Y una op que NO toca R0 no inventa un clobber donde no lo hay.
        IrFunction limpia = make_fn(8, SIZE_MAX);
        limpia.blocks[0].instrs[3].op = IrOp::ADD;
        const codegen::rbank::AbstractProblem q =
            codegen::liveness_to_problem(limpia, live);
        for (const auto &v : q.values)
            CHECK(!v.req.crosses_call);
    }

    std::printf("--- %d checks, %d fallos ---\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
