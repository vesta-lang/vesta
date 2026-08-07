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

/**
 * @brief Las REGIONES de la funcion con su extension.
 *
 * Hasta ahora el analisis decia DONDE toca una operacion (`heap#3+0/8`) pero no
 * hasta donde llega el objeto, y sin eso "fuera de region" no es decidible: se
 * puede afirmar la direccion, no si se sale.  El dato esta en el sitio de
 * asignacion -- `malloc(64)`, `u8[16] a` -- y aqui se enseña, que es el paso
 * previo a poder señalar un desbordamiento.
 *
 * Se distingue tamano CONSTANTE de SIMBOLICO: simbolico no es desconocido --
 * se sabe que valor lo manda --, y es lo que un rango podra acotar despues.
 */
static void print_regiones(std::ostream &os, EffectAnalysis &ea,
                           const ir::IrFunction &fn) {
    const analysis::PointsTo &pt = ea.points_to_publico(fn);
    std::string out;
    for (uint32_t v = 0; v < pt.extent.size(); ++v) {
        const analysis::RegionExtent &ex = pt.extent[v];
        if (!ex.conocida()) continue;
        const PointsToEntry &e = pt.at(v);
        if (e.kind == AbstractLoc::Kind::Unknown) continue;
        LocSet uno;
        uno.add(AbstractLoc{e.kind, v, 0, 0});
        out += "    " + loc_set_str(uno) + ": ";
        if (ex.constante()) out += std::to_string(ex.bytes) + " bytes";
        else out += "tamano en %" + std::to_string(ex.sym) + " (simbolico)";
        out += "\n";
    }
    if (!out.empty()) os << "  Regiones:\n" << out;
}

/**
 * @brief Accesos que se salen de su region, DEMOSTRADOS.
 *
 * Primer veredicto espacial: junta donde toca una operacion (raiz + offset +
 * ancho) con hasta donde llega el objeto (la extension de su region).
 *
 * El limite que se compara es el HUECO RESERVADO, no el tamano logico, y esa
 * distincion es toda la diferencia entre una herramienta y un estorbo: la
 * primera version comparaba contra el tamano logico y avisaba en 26 de los 453
 * programas del corpus, todos falsos -- un `Rgb g = Color.GREEN` mide 3 bytes y
 * el compilador lo copia con un movimiento de 8, igual que SRET, los objetos,
 * `Optional` y `Result`.  Ensanchar un acceso DENTRO del hueco es legitimo;
 * salirse del hueco no lo es nunca.
 *
 * Solo habla cuando esta DEMOSTRADO: extension constante y offset exacto.  Con
 * un tamano simbolico se calla -- se sabe que valor lo manda, pero no cuanto
 * vale hasta que haya rangos --, porque confundir "no lo se" con "esta mal" es
 * lo que hace que estas comprobaciones se acaben apagando.
 *
 * Y lleva su PRUEBA: la region, el acceso y donde acaba.  Un veredicto sin su
 * derivacion obliga a rehacer el razonamiento a mano.
 */
static void print_fuera_de_region(std::ostream &os, EffectAnalysis &ea,
                                  const ir::IrFunction &fn) {
    /* APAGADO por defecto (`VESTA_ASA_BOUNDS=1` lo enciende) mientras se
     * resuelve lo que encontro.
     *
     * Sobre los 453 programas del corpus avisa en 24.  Los de canales comparten
     * una forma muy concreta, y NO parece un fallo del analisis: en
     * `__vxch_wq_pop` el productor escribe 16 bytes en el retbuf -- tag en +0,
     * handle en +8 -- y el llamador LEE en +16 y +24, que son los campos `fib` y
     * `elem` del `Waiter` como si estuvieran EN LINEA.  Pero `Waiter` es un
     * `@overlay` y lo que se guarda es un handle, asi que productor y consumidor
     * no estan de acuerdo sobre `Optional<@overlay struct>`.
     *
     * Hasta saber si los 24 son esa misma forma -- y arreglar el origen -- esto
     * no se enciende: un aviso que la gente no sabe si creer es peor que no
     * tenerlo. */
    static const bool activo = [] {
        const char *v = std::getenv("VESTA_ASA_BOUNDS");
        return v && v[0] == '1';
    }();
    if (!activo) return;
    const analysis::PointsTo &pt = ea.points_to_publico(fn);
    std::string out;
    unsigned n = 0;
    for (const ir::IrBlock &b : fn.blocks) {
        for (const ir::IrInstr &in : b.instrs) {
            if (n >= 8) break;
            const EffectAnalysisResult r = ea.local(fn, in);
            auto revisa = [&](const LocSet &ls, const char *que) {
                if (ls.is_top) return;
                for (const AbstractLoc &l : ls.locs) {
                    if (l.width <= 0 || !l.concrete()) continue;
                    /* La extension se indexa por VALUE-ID, y solo Stack y Heap
                     * tienen ahi su raiz: en `ArgDerived` el identificador es el
                     * INDICE DEL PARAMETRO, asi que consultarlo aqui devuelve la
                     * extension de un valor sin ninguna relacion.  Ese cruce de
                     * dos espacios de indices era el origen de los avisos falsos
                     * (24 de 453 programas): acusaba a `c.campo` de salirse de
                     * un objeto que no era el suyo.  De un parametro no se sabe
                     * el tamano -- lo sabe quien llama --, asi que se calla. */
                    if (l.kind != AbstractLoc::Kind::Stack &&
                        l.kind != AbstractLoc::Kind::Heap)
                        continue;
                    const analysis::RegionExtent &ex = pt.extent_of(l.id);
                    if (!ex.constante()) continue; // simbolico -> aun no se sabe
                    const int64_t tope = ex.limite();
                    const int64_t fin = l.off + l.width;
                    if (l.off >= 0 && fin <= tope) continue; // dentro del hueco
                    LocSet uno;
                    uno.add(AbstractLoc{l.kind, l.id, 0, 0});
                    out += "    " + std::string(que) + " de " +
                           std::to_string(l.width) + " bytes se sale de " +
                           loc_set_str(uno) + " (linea " +
                           std::to_string(in.source_line) + ")\n";
                    out += "      prueba: objeto = " + std::to_string(ex.bytes) +
                           " bytes, hueco = [0, " + std::to_string(tope) +
                           ") ; acceso = [" + std::to_string(l.off) + ", " +
                           std::to_string(fin) + ")\n";
                    ++n;
                }
            };
            revisa(r.effects.mem.writes, "escritura");
            revisa(r.effects.mem.reads, "lectura");
        }
    }
    if (!out.empty())
        os << "  Accesos fuera de region (demostrados):\n" << out;
}

/**
 * @brief Los PRESTAMOS de la funcion, tal como cruzan al IR.
 *
 * El borrow checker demuestra cosas -- sobre todo que un prestamo mutable es
 * EXCLUSIVO -- y hasta ahora eso moria dentro del type checker, sin llegar al
 * analisis.  Verlos aqui es lo que permite comprobar que el hecho que cruza
 * dice lo mismo que el checker, antes de que nadie razone sobre el.
 *
 * Se dice la NATURALEZA de lo prestado porque no se mezclan: prestar un
 * `unique` no es prestar un local, y un puntero crudo no esta sujeto a estas
 * reglas en absoluto.
 */
static void print_prestamos(std::ostream &os, const ir::IrFunction &fn) {
    if (fn.borrow_facts.empty()) return;
    auto nat = [](ir::IrFunction::BorrowOwnerKind k) {
        switch (k) {
        case ir::IrFunction::BorrowOwnerKind::Unique: return "unique";
        case ir::IrFunction::BorrowOwnerKind::Shared: return "shared";
        case ir::IrFunction::BorrowOwnerKind::Reborrow: return "represtamo";
        default: return "local";
        }
    };
    os << "  Prestamos:\n";
    for (const ir::IrFunction::BorrowFact &b : fn.borrow_facts) {
        os << "    " << (b.mutable_ ? "exclusivo" : "compartido") << " de "
           << (b.owner_name.empty() ? "?" : b.owner_name) << " (" << nat(b.owner_kind)
           << ") en linea " << b.line << "\n";
    }
}

/**
 * @brief Los CONFLICTOS de memoria de una funcion, para que se puedan resolver.
 *
 * Un conflicto es lo que impide tratar dos accesos por separado: tocan memoria
 * que puede ser la misma y al menos uno escribe.  Es lo que bloquea mover una
 * lectura fuera de un bucle, eliminar una escritura o reordenar dos accesos, y
 * hasta ahora se quedaba dentro del compilador: el informe decia el efecto de
 * cada funcion pero no CONTRA QUE choca.  Sabiendolo, se puede cambiar el
 * codigo -- separar los objetos, no aliasar, acotar un puntero -- y recuperar
 * la optimizacion; sin saberlo solo queda adivinar.
 *
 * Se anclan en la LIBERACION porque es donde ademas hay un fallo que ensenar:
 * un acceso a memoria ya liberada.  Solo se afirma dentro del mismo bloque y
 * en orden -- ahi el "despues" es cierto, sin depender de por donde vaya el
 * flujo --, y solo con localizaciones precisas: sobre "cualquier sitio" todo
 * choca con todo y el aviso no diria nada.
 */
static void print_conflictos(std::ostream &os, EffectAnalysis &ea,
                             const ir::IrFunction &fn) {
    struct Acc {
        uint32_t    line = 0;
        bool        escribe = false;
        AbstractLoc loc;
    };
    std::string out;
    unsigned n = 0;
    for (const ir::IrBlock &b : fn.blocks) {
        // Accesos del bloque, en orden.  Uno por localizacion tocada.
        std::vector<Acc> accs;
        std::vector<size_t> libera; // indices de accs que son una liberacion
        for (const ir::IrInstr &in : b.instrs) {
            const EffectAnalysisResult r = ea.local(fn, in);
            const bool es_free = (in.op == ir::IrOp::RAW_FREE ||
                                  in.op == ir::IrOp::SMARTPTR_FREE);
            auto anota = [&](const LocSet &ls, bool escribe) {
                if (ls.is_top) return;
                for (const AbstractLoc &l : ls.locs) {
                    if (l.kind == AbstractLoc::Kind::Unknown ||
                        l.kind == AbstractLoc::Kind::None)
                        continue;
                    if (es_free && escribe) libera.push_back(accs.size());
                    accs.push_back({in.source_line, escribe, l});
                }
            };
            anota(r.effects.mem.reads, false);
            anota(r.effects.mem.writes, true);
        }
        for (size_t li : libera) {
            for (size_t j = li + 1; j < accs.size() && n < 8; ++j) {
                if (!may_alias(accs[li].loc, accs[j].loc)) continue;
                LocSet uno;
                uno.add(accs[li].loc);
                out += "    " + loc_set_str(uno) + ": liberada en linea " +
                       std::to_string(accs[li].line) + ", " +
                       (accs[j].escribe ? "escrita" : "leida") + " despues en " +
                       std::to_string(accs[j].line) + "\n";
                ++n;
            }
        }
    }
    if (!out.empty())
        os << "  Conflictos de memoria (acceso a memoria ya liberada):\n" << out;
}

void print_effects_report(std::ostream &os, const ir::IrModule &mod,
                          Backend backend) {
    EffectAnalysis ea;
    ea.set_backend(backend);
    const ModuleSummary &ms = ea.module_summary(mod);

    /* Se dice para QUIEN.  Un efecto no es una propiedad del IR a secas: las
     * ops que dependen del runtime son una instruccion en la maquina virtual y
     * una llamada a libvesta_rt en nativo, y hay quien directamente no existe
     * fuera de la VM.  Un informe que no diga desde donde mira invita a leerlo
     * como si valiera para los tres. */
    os << "=== Efectos y contratos (modelo unico) -- backend: "
       << backend_name(backend) << " ===\n\n";
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
           << (s.structural.recursive ? " recursiva" : "") << "\n";
        print_regiones(os, ea, fn);
        print_fuera_de_region(os, ea, fn);
        print_prestamos(os, fn);
        print_conflictos(os, ea, fn);
        os << "\n";
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
    if (!g.nativas_desconocidas.empty()) {
        os << "  Funciones nativas sin efectos declarados (declararlas cierra "
              "la laguna):\n";
        for (const auto &kv : g.nativas_desconocidas)
            os << "    " << kv.first << " x" << kv.second << "\n";
    }
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

void effects_json(std::ostream &os, const ir::IrModule &mod, Backend backend) {
    EffectAnalysis ea;
    ea.set_backend(backend);
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
