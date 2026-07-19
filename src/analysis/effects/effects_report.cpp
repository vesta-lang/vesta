/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file effects_report.cpp
 * @brief Reporte legible del modelo de efectos (--analyze): efectos +
 *        contratos derivados + lagunas de precision, por proyeccion del
 *        SemanticSummary (misma fuente que consume el compilador).
 */
#include "analysis/effects/effects_report.h"

#include "ir/ssa_ir.h"
#include "analysis/effects/effect_analysis.h"

#include <string>

namespace analysis {
namespace effects {



static const char *loc_kind_name(AbstractLoc::Kind k) {
    switch (k) {
    case AbstractLoc::Kind::None: return "none";
    case AbstractLoc::Kind::Stack: return "stack";
    case AbstractLoc::Kind::Heap: return "heap";
    case AbstractLoc::Kind::Global: return "global";
    case AbstractLoc::Kind::ArgDerived: return "arg";
    case AbstractLoc::Kind::Unknown: return "unknown";
    }
    return "?";
}

static std::string loc_set_str(const LocSet &s) {
    if (s.is_top) return "unknown(*)";
    if (s.locs.empty()) return "-";
    std::string out;
    for (size_t i = 0; i < s.locs.size(); ++i) {
        if (i) out += ",";
        out += loc_kind_name(s.locs[i].kind);
        if (s.locs[i].id != LOC_GENERIC)
            out += "#" + std::to_string(s.locs[i].id);
    }
    return out;
}

static const char *control_name(ControlKind k) {
    switch (k) {
    case ControlKind::FallThrough: return "fallthrough";
    case ControlKind::Return: return "return";
    case ControlKind::Branch: return "branch";
    case ControlKind::Call: return "call";
    case ControlKind::Throw: return "throw";
    case ControlKind::Suspend: return "suspend";
    case ControlKind::Resume: return "resume";
    case ControlKind::Indirect: return "indirect";
    case ControlKind::NoReturn: return "noreturn";
    }
    return "?";
}

static const char *completeness_name(AnalysisCompleteness c) {
    switch (c) {
    case AnalysisCompleteness::Complete: return "complete";
    case AnalysisCompleteness::Conservative: return "over-approx";
    case AnalysisCompleteness::Unknown: return "unknown";
    }
    return "?";
}

static const char *reason_name(UnknownReason r) {
    switch (r) {
    case UnknownReason::None: return "none";
    case UnknownReason::UnmodeledOp: return "op-sin-modelar";
    case UnknownReason::UnknownMnemonic: return "asm-mnemonico-desconocido";
    case UnknownReason::UnknownIntrinsic: return "intrinsic-desconocido";
    case UnknownReason::UnknownEncoding: return "encoding-desconocido";
    case UnknownReason::UserBarrier: return "barrera-usuario";
    case UnknownReason::DynamicDispatch: return "dispatch-dinamico";
    case UnknownReason::Indirect: return "llamada-indirecta";
    case UnknownReason::UnknownFFI: return "ffi-nativo";
    case UnknownReason::ExternalCallee: return "callee-externo";
    case UnknownReason::UnknownRuntime: return "runtime-opaco";
    }
    return "?";
}

static void print_effects(std::ostream &os, const SemanticEffects &e) {
    os << "    reads      : " << loc_set_str(e.mem.reads) << "\n";
    os << "    writes     : " << loc_set_str(e.mem.writes) << "\n";
    os << "    control    : " << control_name(e.control.kind) << "\n";
    std::string may;
    auto add = [&](bool b, const char *n) {
        if (b) { if (!may.empty()) may += " "; may += n; }
    };
    add(e.may_trap, "trap");
    add(e.may_throw, "throw");
    add(e.may_allocate, "allocate");
    add(e.may_block, "block");
    add(e.may_io, "io");
    os << "    may        : " << (may.empty() ? "-" : may) << "\n";
    std::string det;
    auto addd = [&](DeterminismTag t, const char *n) {
        if (e.determinism.has(t)) { if (!det.empty()) det += " "; det += n; }
    };
    addd(DeterminismTag::ReadsClock, "clock");
    addd(DeterminismTag::ReadsRandom, "random");
    addd(DeterminismTag::ReadsPID, "pid");
    addd(DeterminismTag::ReadsEnvironment, "env");
    addd(DeterminismTag::ExternalObservable, "external");
    os << "    nondeterm  : " << (det.empty() ? "- (determinista)" : det) << "\n";
    std::string tags;
    auto addt = [&](CapabilityTag t, const char *n) {
        if (e.tags.has(t)) { if (!tags.empty()) tags += " "; tags += n; }
    };
    addt(CapabilityTag::MachineState, "machine");
    addt(CapabilityTag::InterruptState, "irq");
    addt(CapabilityTag::PortIO, "portio");
    addt(CapabilityTag::MSR, "msr");
    addt(CapabilityTag::CPUID, "cpuid");
    addt(CapabilityTag::Privileged, "priv");
    addt(CapabilityTag::UserBarrier, "barrier");
    if (!tags.empty()) os << "    tags       : " << tags << "\n";
}

void print_effects_report(std::ostream &os, const ir::IrModule &mod) {
    EffectAnalysis ea;
    const ModuleSummary &ms = ea.module_summary(mod);

    os << "=== Efectos y contratos (modelo unico) ===\n\n";
    // Orden estable: el de mod.functions.
    for (const ir::IrFunction &fn : mod.functions) {
        auto it = ms.fns.find(fn.name);
        if (it == ms.fns.end()) continue;
        const FunctionSummary &s = it->second;

        os << fn.name << "\n";
        // Contratos derivados que SE CUMPLEN.
        std::string contracts;
        for (const EvaluatedContract &c : derive_contracts(s))
            if (c.holds) {
                if (!contracts.empty()) contracts += " ";
                contracts += c.name;
            }
        os << "  Contratos : " << (contracts.empty() ? "-" : contracts) << "\n";
        os << "  Analisis  : " << completeness_name(s.completeness) << "\n";
        os << "  Efecto local:\n";
        print_effects(os, s.semantic.local);
        os << "  Efecto transitivo (cierre):\n";
        print_effects(os, s.semantic.closure);
        os << "  Estructura: bloques=" << s.structural.block_count
           << " bucles=" << s.structural.loop_count
           << (s.structural.recursive ? " recursiva" : "") << "\n\n";
    }

    // Reporte de LAGUNAS: hace visible que falta por modelar (cobertura) y donde
    // la opacidad es fundamental (oportunidades de opt del lado del usuario).
    const EffectGaps &g = ea.gaps();
    os << "=== Lagunas de precision ===\n";
    if (g.empty()) {
        os << "  ninguna: todos los efectos se infirieron con precision.\n";
        return;
    }
    os << "  sitios que subieron al efecto maximo (top): " << g.total_top << "\n";
    os << "  por motivo:\n";
    for (const auto &kv : g.by_reason)
        os << "    " << reason_name(kv.first) << " x" << kv.second
           << (reason_is_gap(kv.first) ? "   (LAGUNA del motor -- modelable)"
                                       : "   (opacidad fundamental)")
           << "\n";
    if (!g.unmodeled_ops.empty()) {
        os << "  IrOps sin modelar (mejorar cobertura del motor):\n";
        for (const auto &kv : g.unmodeled_ops)
            os << "    " << ir::ir_op_name(static_cast<ir::IrOp>(kv.first))
               << " x" << kv.second << "\n";
    }
}

} // namespace effects
} // namespace analysis
