/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/manager/analysis_manager.h
 * @brief Infraestructura de analisis del compilador (estilo PassManager nuevo de
 *        LLVM / MLIR).  Los HECHOS son unicos (IRFacts), los ANaLISIS son
 *        independientes (cada uno su retículo/punto-fijo), y los PRODUCTOS
 *        (contratos, complejidad, --analyze) son proyecciones puras FUERA de
 *        aqui.  Este header es el nucleo: identidad de analisis, resultados por
 *        TYPE-ERASURE con concepto (sin herencia forzada), PreservedAnalyses y
 *        el gestor con dependencias explicitas + invalidacion.
 *
 * Es TRANSVERSAL (no pertenece al IR): IR es un consumidor, MachineIR sera otro.
 * Por eso vive en @c analysis/, no en @c ir/.
 *
 * ===========================================================================
 *  VISTA GLOBAL -- EL MOTOR DE CONOCIMIENTO (un motor, capas modulares)
 * ===========================================================================
 *
 * Vesta no es una cadena de pases: es un MOTOR DE CONOCIMIENTO (Programa ->
 * Hechos -> Decisiones).  El motor es UNO SOLO, hecho de capas modulares que
 * se montaron a la vez y cada una se encarga de una cosa distinta.  ESTE
 * header (@c AnalysisManager) NO es un motor separado del @c FunctionSnapshot
 * (codegen/rbank): son DOS CAPAS del MISMO motor con responsabilidades
 * distintas -- GESTION del ciclo de vida vs VISTA de consulta.  Colaboran; no
 * compiten ni se duplican.  (Leer una como "otro motor" es el error a evitar.)
 *
 * El motor no produce hechos sueltos: produce CONOCIMIENTO derivado del
 * programa, y los Facts son su REPRESENTACION ESTABLE.  La cadena es siempre la
 * misma -- los productores (los Analysis) generan hechos; los consumidores los
 * consultan; nadie recomputa el IR:
 *
 *   IR --> Analysis (productor) --> Facts --> query<T>() --> consumidores
 *          recorrido / reticulo    dato       demand-        allocator /
 *                                  estable    driven         scheduler / ...
 *
 *   IR  (una representacion del programa)
 *    |     cada analisis DERIVA su hecho de un recorrido/reticulo; ningun
 *    |     consumidor re-recorre el IR -- el hecho se computa UNA vez.
 *    v
 *  +---------------------------------------------------------------------+
 *  | CAPA DE HECHOS (Facts) -- cada hecho un modulo independiente         |
 *  |                                                                      |
 *  |  STRUCTURAL  un recorrido, sin reticulo (analysis/facts, ir/):       |
 *  |     IrFacts (def-use, call-sites, back-edges)     Liveness           |
 *  |  SEMANTIC    reticulo / punto-fijo / interproc (analysis..):         |
 *  |     PointsTo + alias    EscapeInfo    SemanticEffects + summaries    |
 *  |  HARDWARE    Tipo C, por microarquitectura (analysis/hw):            |
 *  |     MachineCostFacts (latencias reload / store / move)               |
 *  |  DERIVED     compuestos de los anteriores (analysis/derived, facts): |
 *  |     LoopFacts    ProfileFacts    RematFacts                          |
 *  +---------------------------------------------------------------------+
 *      |                                          |
 *      | GESTION (ciclo de vida)                  | CONSULTA (demand-driven)
 *      v                                          v
 *  +------------------------------+   +---------------------------------+
 *  | AnalysisManager (ESTE file)  |   | FunctionSnapshot (codegen/rbank)|
 *  | transversal: IR + MachineIR  |   | vista para allocator/scheduler  |
 *  |  - cache por (AnalysisID,unit)|  |  - query<T>() lazy + LazyFact   |
 *  |  - deps explicitas (rev_deps) |  |  - QueryProducer<T> = algoritmo |
 *  |  - invalidacion en cascada    |  |  - deps se resuelven SOLAS      |
 *  |    (PreservedAnalyses)        |  |    (produce llama a query<U>)   |
 *  |  - dos ejes: cross-analisis   |  |  - DATO puro -> serializable    |
 *  |    e interprocedural          |  |    (core dump del conocimiento) |
 *  +------------------------------+   +---------------------------------+
 *          \                                    /
 *           \  El DISENO es UN unico ciclo de vida de los analisis (el del
 *            \ AnalysisManager); el snapshot mantiene HOY una cache propia
 *             \ (LazyFact) que se migrara a ese ciclo para heredar su
 *              \ invalidacion (los HUECOS estan en optimization_context.h).
 *               v El modelo mental ya es uno solo.
 *  +---------------------------------------------------------------------+
 *  | CONSUMIDORES: interp / JIT / AOT / allocator (rbank) / scheduler /   |
 *  |   vectorizer / --analyze / contratos de coste / LSP                  |
 *  +---------------------------------------------------------------------+
 *
 * POR QUE DOS CAPAS Y NO UNA: el @c AnalysisManager resuelve el CICLO DE VIDA
 * (computar perezoso, cachear por unidad, e INVALIDAR en cascada cuando un
 * pase muta el IR -- mecanismo @c PreservedAnalyses).  El @c FunctionSnapshot
 * resuelve la CONSULTA ergonomica de codegen (@c query<T>() que arrastra sus
 * dependencias solo).  Son ortogonales: uno gestiona QUE sobrevive a un
 * cambio; el otro ofrece COMO se pide un hecho.  El plan es que el segundo se
 * apoye en el primero (heredar invalidacion), no que uno sustituya al otro.
 *
 * POR QUE MODULAR: anadir conocimiento = anadir un MODULO (un Fact nuevo con
 * su productor), NO tocar el motor.  El @c AnalysisManager no cambia cuando
 * aparece un Fact; el @c query<T>() del snapshot tampoco.  Un hecho STRUCTURAL
 * y uno SEMANTIC nunca se mezclan (criterio en @c ir_facts.h: si necesita
 * reticulo o punto-fijo es ANALISIS, no hecho).  Y cada capa tiene informacion
 * que las otras NO tienen -- el IR sabe def-use y forma del CFG; el ASM sabe
 * latencias y puertos; el perfil sabe frecuencia real de ejecucion -- el motor
 * las mantiene juntas y consultables sin colapsarlas en una sola.
 *
 * ===========================================================================
 *  LOS TRES EJES DEL CONOCIMIENTO (que / cuanto / cuando-fisico)
 * ===========================================================================
 * El conocimiento que consume una decision (allocator, scheduler, ...) se
 * reparte en tres ejes ortogonales, cada uno en su nivel:
 *
 *   - IR  -> el QUE y el CUANDO SEMANTICO.  Que operacion, que tipo; cuando se
 *     vuelve a usar un valor EN EL PROGRAMA (UseDefFacts), profundidad de loop
 *     (LoopFacts), frecuencia (ProfileFacts), si es recomputable (RematFacts).
 *   - MachineKnowledge -> el CUANTO.  Coste real de una operacion en la
 *     microarquitectura (MachineCostFacts: latencia/puertos/uops; SpillCostCard:
 *     reload/store/move).  OJO: es conocimiento de la ARQUITECTURA, NO del
 *     MachineIR -- por eso vive en @c analysis/hw/, no en @c jit/.  Se puede
 *     preguntar "cuanto cuesta este ADD i64" SIN emitir una sola instruccion.
 *   - MachineIR -> el CUANDO FISICO.  El orden real tras la seleccion, use/def a
 *     2 posiciones por instruccion, folds (LEA), immediates, movimientos extra,
 *     presion de registros.  Solo aqui se conoce el coste OBSERVADO.
 *
 * COSTE ESTIMADO vs OBSERVADO.  De esos ejes salen dos costes:
 *   - ESTIMADO: IR (que op) + MachineKnowledge (cuanto) -> ANTES de bajar a
 *     MachineIR.  Ej: coste de recomputar una @c RematRecipe = latencia de su op.
 *   - OBSERVADO: tras la seleccion de instrucciones (MachineIR) -> incluye folds,
 *     immediates, MOVs extra, puertos ocupados.
 * El IR NUNCA conoce su coste maquina (no hay @c IrInstr::machine_cost()); es la
 * MAQUINA quien lo estima (@c MachineCostFacts::estimate(recipe)).  El punto donde
 * los ejes se fusionan es el @c OptimizationContext (Facts-programa x
 * Facts-hardware -> ObjectiveTerms -> decision).
 *
 * ===========================================================================
 *  UN FACT PERTENECE A UN DOMINIO (posiciones tipadas por nivel)
 * ===========================================================================
 * Un mismo concepto puede existir en VARIOS niveles sin ser duplicacion, porque
 * responde a dominios distintos.  Ejemplo canonico: el NEXT-USE ("cuando se
 * vuelve a usar cada valor") existe en dos, y no son intercambiables:
 *   - Belady IR      = @c UseDefFacts         (IrValueId + @c ir::LinearPos)  -> remat / sched IR
 *   - Belady Machine = @c MachineNextUseFacts  (vreg + @c codegen::LinearPos)      -> allocator
 * Sus POSICIONES viven en dominios distintos (1 vs 2 por instruccion); el tipo
 * fuerte @c ir::LinearPos / @c codegen::LinearPos lo impide cruzar en compilacion
 * (ver @c ir/linear_pos.h, @c codegen/linear_pos.h).  REGLA GENERAL: cada dominio
 * tiene sus Facts y sus posiciones.  Que un Fact nuevo aparezca "por nivel" (no
 * como parche de un consumidor) es indicio de que la regla es correcta.  Cuando
 * lleguen @c CFGPos / @c ProfilePos / ... seran "posiciones" pero ninguna
 * intercambiable -- justo el error que merece atrapar el compilador.
 */
#ifndef VESTA_ANALYSIS_MANAGER_H
#define VESTA_ANALYSIS_MANAGER_H

#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace analysis {

// ===========================================================================
// AnalysisID -- identidad ESTABLE de un analisis (ortogonal a la interfaz de
// resultado).  Cada analisis expone un `static char ID;`; su direccion es el id.
// ===========================================================================
using AnalysisID = const void *;

/// Deriva el AnalysisID de un tipo de analisis @c A (que define `static char ID`).
template <class A> AnalysisID analysis_id() { return &A::ID; }

// ===========================================================================
// PreservedAnalyses -- que sobrevive a un pase.  Un pase declara lo que preserva;
// el manager conserva esos resultados y recomputa el resto perezosamente.
// ===========================================================================
class PreservedAnalyses {
public:
    static PreservedAnalyses all() {
        PreservedAnalyses p;
        p.all_ = true;
        return p;
    }
    static PreservedAnalyses none() { return PreservedAnalyses{}; }

    /// Marca un analisis @c A como preservado.
    template <class A> void preserve() { ids_.insert(analysis_id<A>()); }
    void preserve_id(AnalysisID id) { ids_.insert(id); }

    bool preserves_all() const { return all_; }
    bool preserves_id(AnalysisID id) const {
        return all_ || ids_.count(id) != 0;
    }
    template <class A> bool preserves() const {
        return preserves_id(analysis_id<A>());
    }

private:
    bool                            all_ = false;
    std::unordered_set<AnalysisID>  ids_;
};

// ===========================================================================
// Type-erasure con CONCEPTO.  Los resultados NO heredan nada: pueden ser
// value-types puros (EffectSummary, IRFacts...).  El unico gancho que importa
// es `survives(PreservedAnalyses)`: se detecta por duck-typing (si el resultado
// lo define, se usa; si no, default conservador = no sobrevive -> se recomputa).
// ===========================================================================
namespace detail {
template <class T, class = void>
struct has_survives : std::false_type {};
template <class T>
struct has_survives<
    T, decltype((void)std::declval<const T &>().survives(
           std::declval<const PreservedAnalyses &>()))> : std::true_type {};

template <class T>
bool call_survives(const T &r, const PreservedAnalyses &p) {
    // preserve-all no invalida NADA (el pase no cambio nada relevante).
    if (p.preserves_all()) return true;
    if constexpr (has_survives<T>::value)
        return r.survives(p);
    else
        return false; // sin gancho ante un pase que cambio algo: recomputar
}
} // namespace detail

/// Interfaz interna de un resultado type-erased.
struct AnalysisResultConcept {
    virtual ~AnalysisResultConcept() = default;
    virtual bool survives(const PreservedAnalyses &p) const = 0;
};

/// Modelo templado: guarda un @c T por VALOR y reenvia el gancho.
template <class T>
struct AnalysisResultModel final : AnalysisResultConcept {
    T result;
    explicit AnalysisResultModel(T r) : result(std::move(r)) {}
    bool survives(const PreservedAnalyses &p) const override {
        return detail::call_survives(result, p);
    }
};

// ===========================================================================
// AnalysisManager -- lazy + caché + dependencias explicitas + invalidacion.
//
// Clave = (AnalysisID, unit).  `unit` es un string (nombre de funcion, o
// "<module>" para el nivel modulo).  El grafo de DEPENDENCIAS se construye SOLO:
// cuando el computo de A pide getResult<B>, se registra "A depende de B"; asi
// invalidar B invalida A -- y esto captura los DOS ejes de invalidacion (cross-
// analisis e interprocedural: si el cierre de un caller lee el summary del
// callee, la dependencia lo refleja automaticamente).
// ===========================================================================
class AnalysisManager {
public:
    struct Key {
        AnalysisID  id;
        std::string unit;
        bool operator==(const Key &o) const {
            return id == o.id && unit == o.unit;
        }
    };
    struct KeyHash {
        size_t operator()(const Key &k) const {
            return std::hash<const void *>()(k.id) ^
                   (std::hash<std::string>()(k.unit) << 1);
        }
    };

    /// Devuelve el resultado de @c A para @p unit, computandolo perezosamente con
    /// @p factory si no esta cacheado.  Registra la dependencia con el computo en
    /// curso (si lo hay) para la invalidacion.  @p factory: `() -> T`.
    template <class A, class T, class Factory>
    const T &get_or_compute(const std::string &unit, Factory &&factory) {
        const Key k{analysis_id<A>(), unit};
        // Dependencia: el computo en curso (tope de la pila) depende de k.
        if (!stack_.empty()) rev_deps_[k].insert(stack_.back());
        auto it = results_.find(k);
        if (it != results_.end())
            return static_cast<AnalysisResultModel<T> *>(it->second.get())->result;
        stack_.push_back(k);
        T value = factory(); // puede pedir otros get_or_compute -> mas deps
        stack_.pop_back();
        auto model = std::make_unique<AnalysisResultModel<T>>(std::move(value));
        T &ref = model->result;
        results_[k] = std::move(model);
        return ref;
    }

    /// ¿Hay resultado cacheado de @c A para @p unit?
    template <class A> bool cached(const std::string &unit) const {
        return results_.count(Key{analysis_id<A>(), unit}) != 0;
    }

    /// Invalida el resultado @c A de @p unit y, transitivamente, todo lo que
    /// dependia de el (ambos ejes).
    template <class A> void invalidate(const std::string &unit) {
        invalidate_key(Key{analysis_id<A>(), unit});
    }

    /// Invalida los resultados de @p unit que NO sobreviven a @p preserved
    /// (mecanismo PreservedAnalyses tras un pase).  Cascada por dependencias.
    void invalidate(const std::string &unit, const PreservedAnalyses &preserved) {
        std::vector<Key> dead;
        for (const auto &kv : results_)
            if (kv.first.unit == unit && !kv.second->survives(preserved))
                dead.push_back(kv.first);
        for (const Key &k : dead) invalidate_key(k);
    }

    /// Borra TODO (reconstruccion completa).
    void clear() {
        results_.clear();
        rev_deps_.clear();
        stack_.clear();
    }

    size_t size() const { return results_.size(); }

private:
    void invalidate_key(const Key &k) {
        auto it = results_.find(k);
        if (it == results_.end()) return;
        results_.erase(it);
        auto d = rev_deps_.find(k);
        if (d == rev_deps_.end()) return;
        std::vector<Key> deps(d->second.begin(), d->second.end());
        rev_deps_.erase(d);
        for (const Key &dep : deps) invalidate_key(dep); // cascada
    }

    std::unordered_map<Key, std::unique_ptr<AnalysisResultConcept>, KeyHash>
        results_;
    std::unordered_map<Key, std::unordered_set<Key, KeyHash>, KeyHash> rev_deps_;
    std::vector<Key> stack_; // computos en curso (para registrar dependencias)
};

} // namespace analysis

#endif // VESTA_ANALYSIS_MANAGER_H
