/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file test_code_origin_fact.cpp
 * @brief El hecho del dominio `asa.code_origin`: de QUIEN es el codigo al que
 *        salta una llamada, y como se dice cuando no se sabe.
 *
 * QUE SE PRUEBA, y por que esto y no otra cosa.  Un hecho del ASA no es solo
 * su conclusion: es la conclusion MAS su sello -- certeza, quien lo dice, con
 * que regla, y DoNDE vale --.  Un hecho correcto con un sello equivocado es
 * peor que no tenerlo, porque parece auditable y no lo es, y ademas se puede
 * reutilizar donde es falso.  Por eso aqui se comprueba el sello entero y no
 * solo si sale "ours" o "foreign".
 *
 * Y el AMBITO tiene caso propio.  Este hecho solo vale en la maquina: en
 * nativo todo es codigo real y las dos formas de llamar caen en la misma
 * instruccion, asi que sin sellar `isa` el hecho afirmaria en un objetivo algo
 * que solo es cierto en otro -- y el almacen lo daria por bueno --.
 *
 * LA TRADUCCIoN DE MOTIVOS TIENE SU PROPIA BATERIA, y no por completismo: al
 * escribirla se mandaron los cinco por un mismo cajon, que es exactamente lo
 * que el vocabulario comun del ASA existe para impedir.  El motivo es lo que
 * decide la ACCIoN, y las de estos son opuestas -- lo que llega por un
 * parametro se declara, lo que sale de memoria se deduce mirando quien
 * escribe, y lo que se corto por presupuesto no es culpa del programa --.
 */
#include "analysis/asa/observed.h"
#include "ir/ir_optimizer.h"
#include "ir/ssa_ir.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace analysis;
using namespace analysis::asa;

static int g_fail = 0;
static int g_checks = 0;

static void check(bool cond, const std::string &what) {
    ++g_checks;
    if (!cond) {
        ++g_fail;
        std::printf("  FAIL: %s\n", what.c_str());
    }
}

/// Un sitio ya clasificado, que es lo que el constructor del hecho consume.
static ir::CallTargetSite site(ir::CallTargetMemory memory,
                               ir::CallTargetUnknown why, uint32_t line) {
    ir::CallTargetSite s;
    s.memory = memory;
    s.why = why;
    s.line = line;
    s.target = static_cast<ir::IrValueId>(7); // un valor cualquiera
    return s;
}

/// Una funcion vacia con nombre: al hecho solo le hace falta el nombre.
static ir::IrFunction a_function() {
    ir::IrFunction fn;
    fn.name = "modulo__doblar";
    return fn;
}

// ---------------------------------------------------------------------------
// El hecho
// ---------------------------------------------------------------------------

/// Codigo nuestro: se afirma, DEMOSTRADO, y con la regla por la que se supo.
static void case_ours() {
    FactStore store;
    const ir::IrFunction fn = a_function();
    Fact f;
    const bool got = code_origin_fact(
        store, fn,
        site(ir::CallTargetMemory::Machine, ir::CallTargetUnknown::None, 42),
        kStagePostOpt, Source::Static, f);
    check(got, "code of ours yields a fact");
    if (!got) return;
    check(std::strcmp(f.what.code, "origin.ours") == 0,
          "ours is asserted as 'origin.ours'");
    check(f.seal.certainty == Certainty::Proven,
          "and PROVEN: the chain of definitions was followed to its origin");
    check(f.what.domain != nullptr &&
              std::strcmp(f.what.domain, kProducerCodeOrigin) == 0,
          "the fact says which domain it belongs to");
    check(f.seal.origin.producer != nullptr &&
              std::strcmp(f.seal.origin.producer, kProducerCodeOrigin) == 0,
          "and who is saying it");
    check(
        f.proof.rule != nullptr && f.proof.rule[0] != '\0',
        "and BY WHAT RULE: a verdict without its derivation cannot be judged");
    check(
        f.about.kind == Subject::Kind::Value && f.about.id == 7u,
        "it speaks of the VALUE holding the address, which is an entity of the "
        "IR and can be cross-checked against what others say about it");
    check(f.seal.origin.site.kind == Anchor::Kind::Line &&
              f.seal.origin.site.id == 42u,
          "and it says where, for whoever has the source in front of them");
}

/// Codigo de fuera: el OTRO hecho, no el mismo con una bandera.
static void case_foreign() {
    FactStore store;
    const ir::IrFunction fn = a_function();
    Fact f;
    const bool got = code_origin_fact(
        store, fn,
        site(ir::CallTargetMemory::Host, ir::CallTargetUnknown::None, 9),
        kStagePostOpt, Source::Static, f);
    check(got, "foreign code yields a fact");
    if (!got) return;
    check(
        std::strcmp(f.what.code, "origin.foreign") == 0,
        "foreign is asserted as 'origin.foreign', which is ANOTHER fact: ours "
        "has a body we can look at and this one does not");
}

/// EL AMBITO.  Solo vale en la maquina; en nativo la distincion no existe.
static void case_only_on_the_machine() {
    FactStore store;
    const ir::IrFunction fn = a_function();
    Fact f;
    if (!code_origin_fact(
            store, fn,
            site(ir::CallTargetMemory::Machine, ir::CallTargetUnknown::None, 1),
            kStagePostOpt, Source::Static, f))
        return;
    check(f.scope.isa != nullptr && std::strcmp(f.scope.isa, kIsaVelb) == 0,
          "it is sealed to the machine: in native code everything is real code "
          "and both ways of calling collapse into the same instruction");
    check(f.scope.stage != nullptr &&
              std::strcmp(f.scope.stage, kStagePostOpt) == 0,
          "and to the moment it was said: what holds after optimising need not "
          "hold before it");
}

/// De lo que no se sabe NO se arma hecho: lo que toca es decir por que, y de
/// eso se encarga el productor.  Dos formas de decir lo mismo serian dos
/// fuentes de la misma verdad.
static void case_no_fact_when_unknown() {
    FactStore store;
    const ir::IrFunction fn = a_function();
    Fact f;
    const bool got =
        code_origin_fact(store, fn,
                         site(ir::CallTargetMemory::Unknown,
                              ir::CallTargetUnknown::FromMemory, 5),
                         kStagePostOpt, Source::Static, f);
    check(!got, "an 'unknown' does not yield an asserted fact");
}

// ---------------------------------------------------------------------------
// El vocabulario del no-saber
// ---------------------------------------------------------------------------

/// Cada motivo a SU clase.  Es lo que decide la accion, y las de estos son
/// opuestas entre si.
static void case_reasons() {
    check(code_origin_unknown_kind(ir::CallTargetUnknown::Parameter) ==
              UnknownReason::OpaqueBoundary,
          "what arrives by a parameter is an opaque boundary: the caller knows "
          "it, and the action is to declare it");
    check(code_origin_unknown_kind(ir::CallTargetUnknown::FromMemory) ==
              UnknownReason::ShapeNotRecognized,
          "what comes out of memory CAN be deduced; what is missing is for the "
          "analysis to cover that shape");
    check(code_origin_unknown_kind(ir::CallTargetUnknown::Computed) ==
              UnknownReason::RuntimeDependent,
          "a computation depends on what happens at run time");
    check(code_origin_unknown_kind(ir::CallTargetUnknown::Disagree) ==
              UnknownReason::RuntimeDependent,
          "two PATHS carrying different things does too, and it is NOT "
          "'SourcesDisagree' -- that is two PRODUCERS disagreeing, which "
          "would be a compiler bug");
    check(code_origin_unknown_kind(ir::CallTargetUnknown::TooDeep) ==
              UnknownReason::BudgetExceeded,
          "giving up on budget is not the programs fault but the analysis, "
          "and saying so is the honest thing");
}

/// Y cada motivo con su codigo estable, para poder agrupar sin leer texto.
static void case_reason_codes() {
    check(
        std::strcmp(code_origin_unknown_code(ir::CallTargetUnknown::Parameter),
                    "origin.from_param") == 0,
        "the reason code is stable");
    check(
        std::strcmp(code_origin_unknown_code(ir::CallTargetUnknown::FromMemory),
                    "origin.from_memory") == 0,
        "y distinto para cada caso");
    check(std::strcmp(code_origin_unknown_code(ir::CallTargetUnknown::TooDeep),
                      "origin.too_deep") == 0,
          "including the one for the budget");
}

int main() {
    std::printf("== the fact of whose code a call jumps into ==\n");
    case_ours();
    case_foreign();
    case_only_on_the_machine();
    case_no_fact_when_unknown();
    case_reasons();
    case_reason_codes();
    std::printf("%d of %d OK\n", g_checks - g_fail, g_checks);
    return g_fail == 0 ? 0 : 1;
}
