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
#include "vx/diag/diag_format.h" // el texto de los motivos vive en el catalogo

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
        const AbstractLoc &l = s.locs[i];
        out += loc_kind_name(l.kind);
        if (l.id != LOC_GENERIC) out += "#" + std::to_string(l.id);
        // Offset/ancho concretos (modelo preciso): "+off/w".  width 0 = objeto
        // entero -> se omite.
        if (l.width > 0)
            out += "+" + std::to_string(l.off) + "/" + std::to_string(l.width);
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
        /* Los que SE CUMPLEN, y detras los que NO con su motivo.  Saber que un
         * contrato falla sin saber por que obliga a ir a leer los efectos y
         * deducirlo; el predicado ya lo sabe, asi que lo dice.  El texto sale
         * del catalogo multi-idioma: aqui no se redacta nada. */
        std::string contracts;
        std::string fallidos;
        for (const EvaluatedContract &c : derive_contracts(s)) {
            if (c.holds) {
                if (!contracts.empty()) contracts += " ";
                contracts += c.name;
                continue;
            }
            std::string motivos;
            for (ContractReason r : c.motivos) {
                if (!motivos.empty()) motivos += ", ";
                motivos += vx::diag::format(contract_reason_code(r), {});
            }
            if (motivos.empty()) continue;
            fallidos += "                ";
            fallidos += c.name;
            fallidos += ": ";
            fallidos += motivos;
            fallidos += "\n";
        }
        os << "  Contratos : " << (contracts.empty() ? "-" : contracts) << "\n";
        if (!fallidos.empty())
            os << "    no cumple:\n" << fallidos;
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
    /* Y CUALES son las instrucciones de asm que no se saben explicar.  Sin el
     * nombre no se puede cerrar la laguna, y cerrarla -- anadir la instruccion a
     * la tabla -- es la forma prevista de que el analizador crezca. */
    if (!g.mnemonicos_desconocidos.empty()) {
        os << "  Instrucciones de asm sin tabular (anadirlas cierra la laguna):\n";
        for (const auto &kv : g.mnemonicos_desconocidos)
            os << "    " << kv.first << " x" << kv.second << "\n";
    }
}

// ---- Proyeccion JSON (misma fuente que el reporte legible) ----

static std::string json_escape(const std::string &s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        if (c == '"' || c == '\\') o.push_back('\\');
        o.push_back(c);
    }
    return o;
}

// Emite un array JSON de banderas may_* activas.
static void may_json(std::ostream &os, const SemanticEffects &e) {
    const char *sep = "";
    os << "[";
    auto add = [&](bool b, const char *n) {
        if (b) { os << sep << "\"" << n << "\""; sep = ","; }
    };
    add(e.may_trap, "trap");
    add(e.may_throw, "throw");
    add(e.may_allocate, "allocate");
    add(e.may_block, "block");
    add(e.may_io, "io");
    os << "]";
}

static void nondeterm_json(std::ostream &os, const SemanticEffects &e) {
    const char *sep = "";
    os << "[";
    auto add = [&](DeterminismTag t, const char *n) {
        if (e.determinism.has(t)) { os << sep << "\"" << n << "\""; sep = ","; }
    };
    add(DeterminismTag::ReadsClock, "clock");
    add(DeterminismTag::ReadsRandom, "random");
    add(DeterminismTag::ReadsPID, "pid");
    add(DeterminismTag::ReadsEnvironment, "env");
    add(DeterminismTag::ExternalObservable, "external");
    os << "]";
}

static void tags_json(std::ostream &os, const SemanticEffects &e) {
    const char *sep = "";
    os << "[";
    auto add = [&](CapabilityTag t, const char *n) {
        if (e.tags.has(t)) { os << sep << "\"" << n << "\""; sep = ","; }
    };
    add(CapabilityTag::MachineState, "machine");
    add(CapabilityTag::InterruptState, "irq");
    add(CapabilityTag::PortIO, "portio");
    add(CapabilityTag::MSR, "msr");
    add(CapabilityTag::CPUID, "cpuid");
    add(CapabilityTag::Privileged, "priv");
    add(CapabilityTag::UserBarrier, "barrier");
    os << "]";
}

// Serializa un SemanticEffects como objeto JSON (mismos campos que print_effects).
static void effects_obj_json(std::ostream &os, const SemanticEffects &e) {
    os << "{\"reads\":\"" << json_escape(loc_set_str(e.mem.reads)) << "\""
       << ",\"writes\":\"" << json_escape(loc_set_str(e.mem.writes)) << "\""
       << ",\"control\":\"" << control_name(e.control.kind) << "\"";
    os << ",\"may\":"; may_json(os, e);
    os << ",\"nondeterm\":"; nondeterm_json(os, e);
    os << ",\"tags\":"; tags_json(os, e);
    os << "}";
}

void effects_json(std::ostream &os, const ir::IrModule &mod) {
    EffectAnalysis ea;
    const ModuleSummary &ms = ea.module_summary(mod);

    os << "{\"functions\":[";
    bool first = true;
    for (const ir::IrFunction &fn : mod.functions) {
        auto it = ms.fns.find(fn.name);
        if (it == ms.fns.end()) continue;
        const FunctionSummary &s = it->second;
        if (!first) os << ",";
        first = false;

        os << "{\"function\":\"" << json_escape(fn.name) << "\""
           << ",\"completeness\":\"" << completeness_name(s.completeness) << "\"";
        // Contratos derivados que se cumplen.
        os << ",\"contracts\":[";
        const char *csep = "";
        for (const EvaluatedContract &c : derive_contracts(s))
            if (c.holds) { os << csep << "\"" << json_escape(c.name) << "\""; csep = ","; }
        os << "]";
        os << ",\"local\":"; effects_obj_json(os, s.semantic.local);
        os << ",\"closure\":"; effects_obj_json(os, s.semantic.closure);
        os << ",\"structure\":{\"blocks\":" << s.structural.block_count
           << ",\"loops\":" << s.structural.loop_count
           << ",\"recursive\":" << (s.structural.recursive ? "true" : "false")
           << "}}";
    }
    os << "]";

    // Lagunas de precision (cobertura + opacidad fundamental) para los diagramas.
    const EffectGaps &g = ea.gaps();
    os << ",\"gaps\":{\"total_top\":" << g.total_top << ",\"by_reason\":[";
    bool rfirst = true;
    for (const auto &kv : g.by_reason) {
        if (!rfirst) os << ",";
        rfirst = false;
        os << "{\"reason\":\"" << reason_name(kv.first) << "\""
           << ",\"kind\":\"" << (reason_is_gap(kv.first) ? "gap" : "fundamental") << "\""
           << ",\"count\":" << kv.second << "}";
    }
    os << "],\"unmodeled_ops\":[";
    bool ofirst = true;
    for (const auto &kv : g.unmodeled_ops) {
        if (!ofirst) os << ",";
        ofirst = false;
        os << "{\"op\":\"" << ir::ir_op_name(static_cast<ir::IrOp>(kv.first))
           << "\",\"count\":" << kv.second << "}";
    }
    os << "]}}";
}

} // namespace effects
} // namespace analysis
