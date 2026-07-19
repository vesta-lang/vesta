/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file test_effects.cpp
 * @brief Tests unitarios del modelo de efectos (Fase 0): reticulo de AbstractLoc
 *        (TOP/BOTTOM, may_alias), LocSet (union, gen/kill, absorbente),
 *        combinadores seq/join (leyes: neutro, absorbente, conmutatividad de
 *        join, gen/kill de memoria y registros), y contratos declarativos.
 */
#include "vx/effects/effects.h"
#include "vx/effects/summary.h"

#include <cstdio>

using namespace vx::fx;

static int g_checks = 0;
static int g_fails = 0;
static void check(bool ok, const char *what) {
    ++g_checks;
    if (!ok) {
        ++g_fails;
        std::printf("  FALLO: %s\n", what);
    }
}

static AbstractLoc L(AbstractLoc::Kind k, uint32_t id = 0) { return {k, id}; }

int main() {
    using K = AbstractLoc::Kind;

    // ---- may_alias: reticulo TOP/BOTTOM ----
    check(!may_alias(L(K::None), L(K::Unknown)), "bottom no aliasa nada");
    check(!may_alias(L(K::None), L(K::Heap, 1)), "bottom no aliasa heap");
    check(may_alias(L(K::Unknown), L(K::Stack)), "top aliasa stack");
    check(may_alias(L(K::Unknown), L(K::Heap, 7)), "top aliasa heap");
    check(!may_alias(L(K::Stack), L(K::Heap, 1)), "clases distintas disjuntas");
    check(may_alias(L(K::Heap, 3), L(K::Heap, 3)), "mismo heap id aliasa");
    check(!may_alias(L(K::Heap, 3), L(K::Heap, 4)), "heap ids distintos no aliasan");
    check(may_alias(L(K::Heap, 0), L(K::Heap, 4)), "heap id 0 (generico) aliasa");
    check(may_alias(L(K::Global, 9), L(K::Global, 9)), "mismo global aliasa");

    // ---- LocSet: union, absorbente, gen/kill ----
    {
        LocSet s;
        check(s.empty(), "locset vacio");
        s.add(L(K::None)); // bottom no aporta
        check(s.empty(), "add bottom no aporta");
        s.add(L(K::Heap, 1));
        s.add(L(K::Heap, 1)); // dup
        check(!s.empty() && !s.is_top && s.locs.size() == 1, "add dedup");
        s.add(L(K::Unknown)); // top absorbente
        check(s.is_top && s.locs.empty(), "unknown colapsa a top");
        check(s.may_alias_any(L(K::Stack)), "top aliasa cualquiera");
    }
    {
        LocSet a, b;
        a.add(L(K::Heap, 1));
        b.add(L(K::Heap, 2));
        a.unite(b);
        check(a.locs.size() == 2, "union de dos heaps distintos");
        // gen/kill: quitar solo el loc concreto exacto.
        LocSet reads, writes;
        reads.add(L(K::Heap, 1));
        reads.add(L(K::Heap, 2));
        writes.add(L(K::Heap, 1));
        reads.subtract_concrete(writes);
        check(reads.locs.size() == 1 && reads.locs[0] == L(K::Heap, 2),
              "gen/kill quita el loc escrito exacto");
        // si writes es top, no mata nada (sound).
        LocSet reads2, wtop;
        reads2.add(L(K::Heap, 1));
        wtop.add(L(K::Unknown));
        reads2.subtract_concrete(wtop);
        check(reads2.locs.size() == 1, "writes top no mata lecturas (sound)");
    }

    // ---- seq: gen/kill de memoria + control terminador ----
    {
        SemanticEffects a; // escribe heap(1)
        a.mem.writes.add(L(K::Heap, 1));
        SemanticEffects b; // lee heap(1) (interno) y heap(2) (externo)
        b.mem.reads.add(L(K::Heap, 1));
        b.mem.reads.add(L(K::Heap, 2));
        SemanticEffects r = seq(a, b);
        check(r.mem.reads.locs.size() == 1 && r.mem.reads.locs[0] == L(K::Heap, 2),
              "seq mem: la lectura interna (heap1) se mata, la externa (heap2) queda");
        check(r.mem.writes.locs.size() == 1, "seq mem: writes se acumulan");
    }
    {
        // control: si 'a' termina (Return), el control del par es el de 'a'.
        SemanticEffects a, b;
        a.control.kind = ControlKind::Return;
        b.control.kind = ControlKind::Call;
        check(seq(a, b).control.kind == ControlKind::Return,
              "seq control: 'a' Return domina");
        SemanticEffects a2, b2;
        a2.control.kind = ControlKind::FallThrough;
        b2.control.kind = ControlKind::Call;
        check(seq(a2, b2).control.kind == ControlKind::Call,
              "seq control: 'a' FallThrough -> control de 'b'");
    }
    {
        // neutro: seq(none, x) == seq(x, none) en las may-props.
        SemanticEffects x;
        x.may_throw = true;
        x.mem.writes.add(L(K::Global, 5));
        SemanticEffects n = SemanticEffects::none();
        check(seq(n, x).may_throw && seq(x, n).may_throw, "neutro preserva may_throw");
    }

    // ---- join: conmutativo + union may-effects ----
    {
        SemanticEffects a, b;
        a.may_allocate = true;
        a.mem.reads.add(L(K::Stack));
        b.may_io = true;
        b.mem.reads.add(L(K::Global, 2));
        SemanticEffects ab = join(a, b);
        SemanticEffects ba = join(b, a);
        check(ab == ba, "join conmutativo");
        check(ab.may_allocate && ab.may_io, "join une may-effects");
        check(ab.mem.reads.locs.size() == 2, "join une read-sets");
    }
    {
        // idempotencia: join(x,x) == x.
        SemanticEffects x;
        x.may_block = true;
        x.tags.add(CapabilityTag::PortIO);
        check(join(x, x) == x, "join idempotente");
    }

    // ---- MachineEffects: gen/kill de registros + stack peak ----
    {
        MachineEffects a; // escribe reg0
        a.regs_written = 0x1;
        a.stack_net = 8;
        a.stack_peak = 8;
        MachineEffects b; // lee reg0 (interno) y reg1 (externo)
        b.regs_read = 0x3; // reg0|reg1
        b.stack_net = 16;
        b.stack_peak = 16;
        MachineEffects r = seq(a, b);
        check(r.regs_read == 0x2, "seq regs: reg0 interno se mata, reg1 queda");
        check(r.regs_written == 0x1, "seq regs: writes se acumulan");
        check(r.stack_net == 24, "seq stack net = 8+16");
        check(r.stack_peak == 24, "seq stack peak = max(8, 8+16)");
    }

    // ---- CapabilityClass ----
    check(class_of(CapabilityTag::PortIO) == CapabilityClass::Observable,
          "PortIO es Observable");
    check(class_of(CapabilityTag::CPUID) == CapabilityClass::Machine,
          "CPUID es Machine");
    check(class_of(CapabilityTag::SecretDependent) == CapabilityClass::Security,
          "SecretDependent es Security");

    // ---- Contratos declarativos ----
    {
        // Funcion pura: sin efectos observables.
        FunctionSummary s;
        s.completeness = AnalysisCompleteness::Complete;
        auto cs = derive_contracts(s);
        bool pure = false, det = false, heapfree = false;
        for (const auto &c : cs) {
            if (std::string(c.name) == "pure") pure = c.holds;
            if (std::string(c.name) == "deterministic") det = c.holds;
            if (std::string(c.name) == "heap_free") heapfree = c.holds;
        }
        check(pure, "funcion vacia es pure");
        check(det, "funcion vacia es deterministic");
        check(heapfree, "funcion vacia es heap_free");
    }
    {
        // Funcion que escribe memoria y aloca: NO pura, NO heap_free.
        FunctionSummary s;
        s.completeness = AnalysisCompleteness::Complete;
        s.semantic.closure.mem.writes.add(L(K::Global, 1));
        s.semantic.closure.may_allocate = true;
        auto cs = derive_contracts(s);
        bool pure = true, heapfree = true, readonly = true;
        for (const auto &c : cs) {
            if (std::string(c.name) == "pure") pure = c.holds;
            if (std::string(c.name) == "heap_free") heapfree = c.holds;
            if (std::string(c.name) == "readonly") readonly = c.holds;
        }
        check(!pure, "funcion que escribe/aloca NO es pure");
        check(!heapfree, "funcion que aloca NO es heap_free");
        check(!readonly, "funcion que escribe NO es readonly");
    }
    {
        // Unknown -> ningun contrato positivo (conservador).
        FunctionSummary s;
        s.completeness = AnalysisCompleteness::Unknown;
        auto cs = derive_contracts(s);
        bool any = false;
        for (const auto &c : cs)
            if (std::string(c.name) == "pure" && c.holds) any = true;
        check(!any, "completeness Unknown -> no pure");
    }

    std::printf("=== effects Fase 0: %d checks, %d fallos ===\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
