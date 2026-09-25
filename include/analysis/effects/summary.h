/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/effects/summary.h
 * @brief Resumenes por-funcion y por-modulo del sistema de efectos, y la
 *        generacion DECLARATIVA de contratos.  El @c FunctionSummary es la capa
 *        de analisis compartida (efectos + estructura + interproc) computada
 * UNA vez, de la que TODO producto (contratos, complejidad, autodoc, diagramas,
 * API, optimizer) es proyeccion pura.
 */
#ifndef ANALYSIS_EFFECTS_SUMMARY_H
#define ANALYSIS_EFFECTS_SUMMARY_H

#include "analysis/effects/effects.h"
#include "util/named_alloc.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace analysis {
namespace effects {

/// Faceta SEMANTICA de una funcion: efecto local + efecto transitivo (cierre).
struct SemanticSummary {
    SemanticEffects local;   ///< agregado de los bloques de la funcion.
    SemanticEffects closure; ///< cierre transitivo por el callgraph.
};

/// Faceta ESTRUCTURAL (para complejidad).  Big-O NO se deriva de efectos; vive
/// aqui.  Esqueleto en Fase 0; lo puebla el subsistema de coste en Fase 2.
struct StructuralSummary {
    uint32_t block_count = 0;
    uint32_t loop_count = 0;
    uint32_t max_loop_depth = 0;
    bool recursive = false;
    bool has_unbounded_loop = false; ///< algun bucle sin trip-count acotable.
};

/// Faceta INTERPROCEDURAL: agregados numericos del cierre del callgraph.
struct InterprocSummary {
    int64_t alloc_total =
        -1; ///< sitios de alloc alcanzables (-1 = desconocido).
    int64_t stack_peak_total = -1; ///< profundidad de pila peor caso.
    bool has_calls = false; ///< hace alguna llamada (estatica o dinamica).
    bool reaches_dynamic_call = false;
};

/// Resumen COMPLETO por funcion (contenedor de las 3 facetas).
struct FunctionSummary {
    SemanticSummary semantic;
    StructuralSummary structural;
    InterprocSummary interproc;
    AnalysisCompleteness completeness = AnalysisCompleteness::Complete;
    std::string symbol;
    bool exported = false;
};

namespace scratch {
struct SummaryFns; ///< Los resumenes de las funciones, por indice.
} // namespace scratch

/// Posicion de una funcion dentro de un @ref ModuleSummary.
enum SummaryFnIdx : uint32_t {};
/// Ninguna funcion: el nombre no esta en el resumen.
inline constexpr SummaryFnIdx NO_SUMMARY_FN = SummaryFnIdx(UINT32_MAX);

/**
 * @brief Nivel modulo: el resumen de cada funcion, por INDICE (NO un efecto de
 *        modulo).
 *
 * La identidad de una funcion aqui es su indice.  El nombre solo se mira UNA
 * vez, al resolver un sitio de llamada o una consulta de fuera; el punto fijo
 * y todo lo que lo recorre van por indice.  Antes todo iba por nombre: cada
 * paso del punto fijo hasheaba varias cadenas por arista del grafo de llamadas.
 */
struct ModuleSummary {
    /// Los resumenes, en el orden en que se anadieron.
    util::NamedVector<FunctionSummary, scratch::SummaryFns> fns;
    /// Nombre -> indice.  La clave apunta al nombre INTERNADO de la funcion,
    /// que no se mueve aunque @ref fns crezca.
    std::unordered_map<std::string_view, SummaryFnIdx> index;

    /// Anade el resumen de la funcion de nombre internado @p name.
    SummaryFnIdx add(const std::string *name, FunctionSummary s) {
        const SummaryFnIdx i = static_cast<SummaryFnIdx>(fns.size());
        fns.push_back(std::move(s));
        index.emplace(*name, i);
        return i;
    }
    /// Indice de @p name, o @ref NO_SUMMARY_FN si no esta.
    SummaryFnIdx find(std::string_view name) const {
        const auto it = index.find(name);
        return it == index.end() ? NO_SUMMARY_FN : it->second;
    }
    /// Resumen de @p name, o nulo si no esta.
    const FunctionSummary *get(std::string_view name) const {
        const SummaryFnIdx i = find(name);
        return i == NO_SUMMARY_FN ? nullptr : &fns[i];
    }
    /// Resumen de @p name; que no este es un error de quien pregunta.
    const FunctionSummary &at(std::string_view name) const {
        const FunctionSummary *s = get(name);
        if (s == nullptr)
            throw std::out_of_range("ModuleSummary::at: funcion sin resumen");
        return *s;
    }
    void clear() {
        fns.clear();
        index.clear();
    }
};

// ===========================================================================
// Contratos DECLARATIVOS.  Un contrato es una REGLA con
// nombre + predicado sobre el FunctionSummary completo.  Anadir un contrato =
// registrar una regla; NO se toca el motor.
// ===========================================================================
/// PERFIL de contratos: la MISMA base de hechos/efectos, distintas OPINIONES.
/// Separar hechos (objetivos) de opiniones (juicios con politica) permite tener
/// varias definiciones de `pure`/`nothrow`/... sin tocar el IR ni los efectos.
enum class ContractProfile {
    Default, ///< definiciones estandar.
    Strict,  ///< exige mas (p.ej. pure => tambien determinista, sin tags).
    Relaxed, ///< tolera mas (p.ej. pure aunque pueda atrapar -- traps 'no
             ///< pasan').
    Embedded ///< orientado a Bare/freestanding (sin I/O, sin heap, sin
             ///< runtime).
};

/**
 * @enum ContractReason
 * @brief POR QUE un contrato no se cumple.  Es un DATO, no una frase: quien
 *        informa decide como se redacta (y en que idioma).
 */
enum class ContractReason : uint8_t {
    LeeMemoria,     ///< lee memoria en el cierre.
    EscribeMemoria, ///< escribe memoria en el cierre.
    PuedeAtrapar,   ///< puede provocar un fallo (division por cero, ...).
    PuedeLanzar,    ///< puede lanzar una excepcion.
    /// Puede abortar por `panic`.  Aparte de PuedeLanzar porque no son lo
    /// mismo: en la maquina virtual un panic se puede capturar, y en nativo
    /// llama al hook de panico y no vuelve.  Con un solo motivo, un contrato
    /// de nativo decia "puede lanzar" de algo que no lanza nada.
    PuedeAbortar,
    Aloca,              ///< pide memoria.
    Bloquea,            ///< puede quedarse esperando.
    HaceIO,             ///< entrada/salida observable.
    TieneEtiquetas,     ///< capacidades declaradas (barreras del usuario...).
    EsAtomica,          ///< operacion atomica o barrera.
    NoDeterminista,     ///< el resultado depende de algo externo (reloj...).
    AnalisisIncompleto, ///< no se pudo analizar entero: no se afirma nada.
    Llama,              ///< llama a otra funcion.
    UsaMonton,          ///< usa el monton.
    UsaRecolector,      ///< usa el recolector de basura.
    NecesitaRuntime,    ///< necesita el runtime del lenguaje.
};

/**
 * @struct ContractCheck
 * @brief Veredicto de una regla: si se cumple y, si no, por que.
 *
 * Los motivos los produce EL PROPIO predicado.  Tener una funcion aparte que
 * los "explicara" seria una segunda copia del criterio, y dos copias de un
 * criterio son dos criterios en cuanto alguien toca una.
 */
struct ContractCheck {
    std::vector<ContractReason> motivos; ///< vacio = se cumple.
    bool holds() const { return motivos.empty(); }
};

struct ContractRule {
    const char *name;
    ContractCheck (*predicate)(const FunctionSummary &, ContractProfile);
};

/// Registro global de reglas (definidas en contracts.cpp).  Devuelve el vector
/// estable de reglas registradas.
const std::vector<ContractRule> &contract_rules();

/// Contrato evaluado: nombre, si se cumple y -- si no -- por que.
struct EvaluatedContract {
    const char *name;
    bool holds;
    std::vector<ContractReason> motivos;
};

/**
 * @brief Codigo de catalogo del motivo @p r.
 *
 * Devuelve el CODIGO (`VXNNNN`), no la frase: el texto vive en el catalogo
 * multi-idioma y lo resuelve quien informa.  Asi este modulo no redacta nada y
 * anadir un idioma no lo toca.
 */
const char *contract_reason_code(ContractReason r);

/// Proyeccion pura: aplica TODAS las reglas al summary bajo el @p profile.
std::vector<EvaluatedContract>
derive_contracts(const FunctionSummary &s,
                 ContractProfile profile = ContractProfile::Default);

} // namespace effects
} // namespace analysis

#endif // ANALYSIS_EFFECTS_SUMMARY_H
