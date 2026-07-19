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
