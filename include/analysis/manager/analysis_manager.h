/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/manager/analysis_manager.h
 * @brief Infraestructura de analisis del compilador (estilo PassManager nuevo
 * de LLVM / MLIR).  Los HECHOS son unicos (IRFacts), los ANaLISIS son
 *        independientes (cada uno su retículo/punto-fijo), y los PRODUCTOS
 *        (contratos, complejidad, --analyze) son proyecciones puras FUERA de
 *        aqui.  Este header es el nucleo: identidad de analisis, resultados por
 *        TYPE-ERASURE con concepto (sin herencia forzada), PreservedAnalyses y
 *        el gestor con dependencias explicitas + invalidacion.
 *
 * Es TRANSVERSAL (no pertenece al IR): IR es un consumidor, MachineIR sera
 * otro. Por eso vive en @c analysis/, no en @c ir/.
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
 *     microarquitectura (MachineCostFacts: latencia/puertos/uops;
 * SpillCostCard: reload/store/move).  OJO: es conocimiento de la ARQUITECTURA,
 * NO del MachineIR -- por eso vive en @c analysis/hw/, no en @c jit/.  Se puede
 *     preguntar "cuanto cuesta este ADD i64" SIN emitir una sola instruccion.
 *   - MachineIR -> el CUANDO FISICO.  El orden real tras la seleccion, use/def
 * a 2 posiciones por instruccion, folds (LEA), immediates, movimientos extra,
 *     presion de registros.  Solo aqui se conoce el coste OBSERVADO.
 *
 * COSTE ESTIMADO vs OBSERVADO.  De esos ejes salen dos costes:
 *   - ESTIMADO: IR (que op) + MachineKnowledge (cuanto) -> ANTES de bajar a
 *     MachineIR.  Ej: coste de recomputar una @c RematRecipe = latencia de su
 * op.
 *   - OBSERVADO: tras la seleccion de instrucciones (MachineIR) -> incluye
 * folds, immediates, MOVs extra, puertos ocupados. El IR NUNCA conoce su coste
 * maquina (no hay @c IrInstr::machine_cost()); es la MAQUINA quien lo estima
 * (@c MachineCostFacts::estimate(recipe)).  El punto donde los ejes se fusionan
 * es el @c OptimizationContext (Facts-programa x Facts-hardware ->
 * ObjectiveTerms -> decision).
 *
 * ===========================================================================
 *  UN FACT PERTENECE A UN DOMINIO (posiciones tipadas por nivel)
 * ===========================================================================
 * Un mismo concepto puede existir en VARIOS niveles sin ser duplicacion, porque
 * responde a dominios distintos.  Ejemplo canonico: el NEXT-USE ("cuando se
 * vuelve a usar cada valor") existe en dos, y no son intercambiables:
 *   - Belady IR      = @c UseDefFacts         (IrValueId + @c ir::LinearPos) ->
 * remat / sched IR
 *   - Belady Machine = @c MachineNextUseFacts  (vreg + @c codegen::LinearPos)
 * -> allocator Sus POSICIONES viven en dominios distintos (1 vs 2 por
 * instruccion); el tipo fuerte @c ir::LinearPos / @c codegen::LinearPos lo
 * impide cruzar en compilacion (ver @c ir/linear_pos.h, @c
 * codegen/linear_pos.h).  REGLA GENERAL: cada dominio tiene sus Facts y sus
 * posiciones.  Que un Fact nuevo aparezca "por nivel" (no como parche de un
 * consumidor) es indicio de que la regla es correcta.  Cuando lleguen @c CFGPos
 * / @c ProfilePos / ... seran "posiciones" pero ninguna intercambiable -- justo
 * el error que merece atrapar el compilador.
 */
#ifndef VESTA_ANALYSIS_MANAGER_H
#define VESTA_ANALYSIS_MANAGER_H

#include "util/crono_tramo.h"  // el tramo de soltar las tablas
#include "util/env_flags.h"    // medir la espera del cerrojo es OPCIONAL
#include "util/shared_mutex.h" // lector/escritor SIN la emulacion de pthreads
#include "util/thread_owned.h" // un objeto por hilo, sin `thread_local`

#include <cstdint>
#include <memory>
#include <atomic> // los aciertos se cuentan desde el camino compartido
#include <mutex>
// Los RAII del cerrojo son los de `detail` de este mismo fichero: toman igual
// que los de la biblioteca y ademas apuntan cuanto costo ENTRAR.
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace analysis {

// ===========================================================================
// AnalysisID -- identidad ESTABLE de un analisis (ortogonal a la interfaz de
// resultado).  Cada analisis expone un `static char ID;`; su direccion es el
// id.
// ===========================================================================
using AnalysisID = const void *;

/// Deriva el AnalysisID de un tipo de analisis @c A (que define `static char
/// ID`).
template <class A> AnalysisID analysis_id() {
    return &A::ID;
}

// ===========================================================================
// PreservedAnalyses -- que sobrevive a un pase.  Un pase declara lo que
// preserva; el manager conserva esos resultados y recomputa el resto
// perezosamente.
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
    bool all_ = false;
    std::unordered_set<AnalysisID> ids_;
};

// ===========================================================================
// Type-erasure con CONCEPTO.  Los resultados NO heredan nada: pueden ser
// value-types puros (EffectSummary, IRFacts...).  El unico gancho que importa
// es `survives(PreservedAnalyses)`: se detecta por duck-typing (si el resultado
// lo define, se usa; si no, default conservador = no sobrevive -> se
// recomputa).
// ===========================================================================
namespace detail {
template <class T, class = void> struct has_survives : std::false_type {};
template <class T>
struct has_survives<T, decltype((void)std::declval<const T &>().survives(
                           std::declval<const PreservedAnalyses &>()))>
    : std::true_type {};

template <class T> bool call_survives(const T &r, const PreservedAnalyses &p) {
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
    /**
     * @brief Version de la unidad con la que se calculo este resultado.
     *
     * Es lo que hace que la caducidad sea IMPOSIBLE en vez de responsabilidad
     * de quien invalida.  Un resultado no describe "una funcion": describe una
     * funcion EN UN ESTADO.  Si el estado avanzo, lo que se guardo aqui habla
     * de codigo que ya no existe -- y en el caso de @c IrFacts eso no es solo
     * impreciso: guarda PUNTEROS a instrucciones, asi que leerlo tras una
     * mutacion es leer memoria ajena.  Medido: un 50 % de las ejecuciones
     * terminaba en fallo de segmentacion al confiar en la invalidacion manual.
     *
     * Cero = "sin versionar", para las unidades que no son funciones IR.
     */
    uint64_t version = 0;
};

/// Modelo templado: guarda un @c T por VALOR y reenvia el gancho.
template <class T> struct AnalysisResultModel final : AnalysisResultConcept {
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
// "<module>" para el nivel modulo).  El grafo de DEPENDENCIAS se construye
// SOLO: cuando el computo de A pide getResult<B>, se registra "A depende de B";
// asi invalidar B invalida A -- y esto captura los DOS ejes de invalidacion
// (cross- analisis e interprocedural: si el cierre de un caller lee el summary
// del callee, la dependencia lo refleja automaticamente).
// ===========================================================================
class AnalysisManager {
  public:
    /**
     * @brief Suelta las tablas, y lo CRONOMETRA.
     *
     * No es ceremonia: en una compilacion grande el gestor llega al final con
     * cientos de miles de resultados vivos -- cada uno con sus vectores --, y
     * liberarlos todos de golpe al salir del optimizador no lo medía nadie.  Se
     * vacia aqui DENTRO del tramo porque los destructores de los miembros
     * corren DESPUES del cuerpo: sin vaciar a mano, el cronometro cerraria
     * antes de que se libere nada y marcaria cero.
     *
     * Que ese coste exista es en si un sintoma: un analisis solo hace falta
     * mientras se trabaja su funcion, y aqui sobreviven todos hasta el final.
     */
    ~AnalysisManager() {
        util::CronoTramo t_("asa:release_manager",
                            util::flag_on(util::FlagId::Times));
        for (Shard &s : shards_) {
            s.results.clear();
            s.rev_deps.clear();
            s.keys_by_unit.clear();
        }
    }

    /**
     * @brief Que analisis, de que unidad.
     *
     * La unidad es un PUNTERO al nombre internado, no una copia del nombre.
     * Antes era una `std::string`, y construir la clave -- que se hace en cada
     * consulta, aunque solo sea para buscar -- copiaba el nombre: los nombres
     * calificados pasan de los quince caracteres que caben en la propia
     * cadena, asi que cada consulta reservaba.  Se veia entero en el perfil
     * del asignador:
     *
     *     AnalysisManager::Key::Key -> basic_string -> operator new -> malloc
     *
     * Con el puntero, comparar y hashear son aritmetica.  Y NO se pierde
     * seguridad: el internado garantiza que dos nombres iguales dan el MISMO
     * puntero y dos distintos punteros distintos, asi que la comparacion sigue
     * siendo por nombre -- no es un hash con riesgo de colision.
     */
    struct Key {
        AnalysisID id;
        const std::string *unit;
        bool operator==(const Key &o) const {
            return id == o.id && unit == o.unit;
        }
    };
    struct KeyHash {
        size_t operator()(const Key &k) const {
            /* Mezcla de dos punteros.  El desplazamiento evita que dos claves
             * con los papeles cambiados den el mismo valor. */
            return std::hash<const void *>()(k.id) ^
                   (std::hash<const void *>()(k.unit) << 1);
        }
    };

    /// Devuelve el resultado de @c A para @p unit, computandolo perezosamente
    /// con
    /// @p factory si no esta cacheado.  Registra la dependencia con el computo
    /// en curso (si lo hay) para la invalidacion.  @p factory: `() -> T`.
    /**
     * @brief Igual, pero comprobando que lo cacheado siga hablando del MISMO
     *        estado de la unidad.
     *
     * @param version Version actual de la unidad (p.ej. @c IrFunction::version,
     *        que avanza en cuanto un pase la modifica).  Si no coincide con la
     *        del resultado guardado, se recalcula: un resultado viejo no se
     *        entrega jamas, se haya invalidado o no.
     *
     * Esto sustituye a "acordarse de invalidar", que es una obligacion que no
     * se puede comprobar y que ya fallaba: hay caminos donde el IR se modifica
     * sin que nadie avise, y ahi el cache servia punteros a instrucciones
     * borradas.
     */
    template <class A, class T, class Factory>
    const T &get_or_compute_v(const std::string *unit, uint64_t version,
                              Factory &&factory) {
        const Key k{analysis_id<A>(), unit};
        /* El cerrojo protege las TABLAS, y solo eso.  La fabrica corre FUERA
         * -- ver abajo --: calcular un analisis es lo caro, y hacerlo con el
         * cerrojo puesto serializaria exactamente lo que se quiere repartir.
         *
         * Que dos hilos calculen a la vez el mismo analisis es posible y no es
         * un fallo: se guarda el ultimo y el otro conserva el suyo vivo por el
         * respaldo.  Se paga trabajo repetido en un caso raro a cambio de no
         * pagar serializacion en el caso normal. */
        /* ACIERTO DE NIVEL SUPERIOR sin cerrojo exclusivo, igual que en la
         * variante sin version: si la pila esta vacia nadie depende de esto y
         * la consulta solo LEE.  Es el caso normal dentro del bucle repartido,
         * y con cerrojo exclusivo era donde los hilos hacian cola. */
        Shard &sh = shard_of(unit);
        /* Si ya se miro arriba no se vuelve a mirar: para una consulta caduca de
         * nivel superior eran DOS busquedas y dos tomas del cerrojo para saber
         * lo mismo. */
        bool stale_now = false;
        bool already_looked = false;
        if (stack().empty()) {
            bool was_absent = false;
            {
                util::TimedSharedLock rl(sh.m, shared_wait_slot());
                auto hit = sh.results.find(k);
                stale_now = (hit != sh.results.end() &&
                             hit->second->version != version);
                if (hit != sh.results.end() &&
                    hit->second->version == version) {
                    hits_.fetch_add(1, std::memory_order_relaxed);
                    // Respaldar antes de entregar: ver la nota de abajo.
                    retained().push_back(hit->second);
                    return static_cast<AnalysisResultModel<T> *>(
                               hit->second.get())
                        ->result;
                }
                was_absent = (hit == sh.results.end());
            }
            already_looked = true;
            /* NO ESTABA: se calcula y se guarda con UNA sola toma del
             * exclusivo, en vez de dos.
             *
             * Es el unico caso en que la primera toma no hace falta para nada:
             * de nivel superior no hay dependencia que anotar -- la pila esta
             * vacia --, y sin entrada previa no hay nada que invalidar.  Lo
             * unico que hacia era volver a mirar.
             *
             * Medido con VTune sobre 144k lineas: de la espera del gestor, el
             * 70 % es del cerrojo EXCLUSIVO, y el 57 % de las consultas fallan
             * y lo toman DOS veces.  Esto se lleva por delante una de las dos
             * en el 28 % de las consultas que son entradas nuevas.
             *
             * Que dos hilos calculen a la vez lo mismo ya estaba admitido y
             * sigue igual: gana el ultimo en guardar y el otro conserva el suyo
             * vivo por el respaldo. */
            if (was_absent) {
                stack().push_back(k);
                T value = factory();
                stack().pop_back();
                /* Pudo aparecer mientras se calculaba -- otro hilo, o una
                 * consulta anidada de la propia fabrica --.  Si lo que hay es de
                 * OTRA version, hay que sacarlo con lo que dependia de el.
                 *
                 * Se MIRA con el compartido y se saca por `drop_key`, ANTES de
                 * tomar el exclusivo de la franja: sacarlo cascadea, y una
                 * cascada toma el cerrojo de cascada primero.  Hacerlo con la
                 * franja ya puesta invertiria ese orden, que es lo unico que
                 * podria bloquear. */
                bool stale_now = false;
                {
                    util::TimedSharedLock rl(sh.m, shared_wait_slot());
                    auto existing = sh.results.find(k);
                    if (existing == sh.results.end())
                        fresh_.fetch_add(1, std::memory_order_relaxed);
                    else if (existing->second->version != version)
                        stale_now = true;
                }
                if (stale_now) {
                    stale_.fetch_add(1, std::memory_order_relaxed);
                    drop_key(k);
                }
                util::TimedUniqueLock lk(sh.m, exclusive_wait_slot());
                auto model =
                    std::make_shared<AnalysisResultModel<T>>(std::move(value));
                model->version = version;
                T &ref = model->result;
                retained().push_back(model); // ver el caso de acierto
                sh.results[k] = std::move(model);
                index_add(sh, k);
                return ref;
            }
        }
        /* Caduco, si lo esta: se saca ANTES de tomar la franja, por el mismo
         * motivo que arriba -- sacarlo cascadea, y la cascada va primero --. */
        if (!already_looked) {
            util::TimedSharedLock rl(sh.m, shared_wait_slot());
            auto pre = sh.results.find(k);
            stale_now =
                (pre != sh.results.end() && pre->second->version != version);
        }
        if (stale_now) {
            stale_.fetch_add(1, std::memory_order_relaxed);
            drop_key(k);
        }
        util::TimedUniqueLock lk(sh.m, exclusive_wait_slot());
        if (!stack().empty()) sh.rev_deps[k].insert(stack().back());
        auto it = sh.results.find(k);
        if (it != sh.results.end()) {
            if (it->second->version == version) {
                hits_.fetch_add(1, std::memory_order_relaxed);
                // Respaldar antes de entregar: si otro hilo invalida esta
                // unidad -- o una de la que depende --, el mapa suelta su
                // referencia pero el objeto sigue vivo mientras el llamante lo
                // use.  Sin esto, la referencia devuelta puede colgar.
                retained().push_back(it->second);
                return static_cast<AnalysisResultModel<T> *>(it->second.get())
                    ->result;
            }
            /* Reapareci caduco entre el mirar de arriba y esta toma: otro hilo
             * lo guardo.  Se deja estar y se recalcula encima -- sacarlo aqui
             * invertiria el orden de cerrojos --, que es lo que hace el `[k] =`
             * de abajo de todas formas. */
            stale_.fetch_add(1, std::memory_order_relaxed);
        } else if (!stale_now) {
            /* Ausente porque NUNCA estuvo.  Si lo acabamos de sacar por caduco,
             * esta consulta ya se conto como caducada arriba: contarla tambien
             * como nueva la cuenta DOS veces, y eso hundia la tasa de acierto
             * del informe sin que hubiera cambiado nada -- del 47 % al 30 % por
             * un denominador inflado, que es como una medida se vuelve una
             * mentira que encima parece un hallazgo. */
            fresh_.fetch_add(1, std::memory_order_relaxed);
        }
        stack().push_back(k);
        lk.unlock(); // la fabrica, sin el cerrojo puesto
        T value = factory();
        lk.lock();
        stack().pop_back();
        auto model = std::make_shared<AnalysisResultModel<T>>(std::move(value));
        model->version = version;
        T &ref = model->result;
        retained().push_back(model); // ver el caso de acierto
        sh.results[k] = std::move(model);
        index_add(sh, k);
        return ref;
    }

    template <class A, class T, class Factory>
    const T &get_or_compute(const std::string *unit, Factory &&factory) {
        const Key k{analysis_id<A>(), unit};
        /* ACIERTO DE NIVEL SUPERIOR: se lee y ya, sin cerrojo exclusivo.
         *
         * El cerrojo hace falta para anotar la dependencia, y eso SOLO ocurre
         * cuando la consulta esta anidada dentro de otro computo -- la pila es
         * por hilo, asi que vacia significa "nadie depende de esto".  En el
         * bucle repartido del optimizador la inmensa mayoria de las consultas
         * son de nivel superior y ya estan calculadas: con el cerrojo
         * exclusivo, cientos de miles de tareas de cinco microsegundos hacian
         * cola en el MISMO mutex, y por eso repartir salia mas lento que no
         * repartir.
         *
         * Con cerrojo compartido los lectores no se estorban.  El calculo y la
         * invalidacion siguen siendo exclusivos, que es lo unico que muta. */
        Shard &sh = shard_of(unit);
        if (stack().empty()) {
            util::TimedSharedLock rl(sh.m, shared_wait_slot());
            auto hit = sh.results.find(k);
            if (hit != sh.results.end()) {
                // Respaldar antes de entregar, igual que la variante con
                // version: el cerrojo compartido protege la TABLA mientras se
                // busca, no el objeto despues de soltarlo.  Sin esto, otro
                // hilo que invalide esta clave -- o una de la que dependa --
                // deja al llamante con una referencia colgando.
                retained().push_back(hit->second);
                return static_cast<AnalysisResultModel<T> *>(hit->second.get())
                    ->result;
            }
        }
        // Dependencia: el computo en curso (tope de la pila) depende de k.
        util::TimedUniqueLock lk(sh.m, exclusive_wait_slot());
        if (!stack().empty()) sh.rev_deps[k].insert(stack().back());
        auto it = sh.results.find(k);
        if (it != sh.results.end()) {
            retained().push_back(it->second); // ver el caso de arriba
            return static_cast<AnalysisResultModel<T> *>(it->second.get())
                ->result;
        }
        stack().push_back(k);
        /* Soltar el cerrojo ANTES de la fabrica es obligatorio, no una mejora:
         * la fabrica pide otros `get_or_compute` -- eso es lo que crea las
         * dependencias -- y volver a entrar con el cerrojo puesto se
         * autobloquea sobre un mutex no reentrante.  Aqui faltaba, y colgaba.
         */
        lk.unlock();
        T value = factory(); // puede pedir otros get_or_compute -> mas deps
        lk.lock();
        stack().pop_back();
        auto model = std::make_shared<AnalysisResultModel<T>>(std::move(value));
        T &ref = model->result;
        retained().push_back(model); // ver el caso de acierto
        sh.results[k] = std::move(model);
        index_add(sh, k);
        return ref;
    }

    /// ¿Hay resultado cacheado de @c A para @p unit?
    ///
    /// OJO: no mira la VERSION.  Para una unidad versionada esto contesta "hay
    /// algo guardado", que NO es lo mismo que "se va a reutilizar": si la
    /// version no coincide, @ref get_or_compute_v recalcula y esto seguiria
    /// diciendo que si.  Quien quiera saber si de verdad se ahorra el computo
    /// tiene que usar @ref cached_v -- preguntarlo con esta cuenta de menos los
    /// recomputos, y con ellos se pierden los sellos que dependan de saberlo.
    template <class A> bool cached(const std::string *unit) const {
        // Solo LEE, y solo su franja: cerrojo compartido de una sola.
        const Shard &s = shard_of(unit);
        util::TimedSharedLock lk(s.m, shared_wait_slot());
        return s.results.count(Key{analysis_id<A>(), unit}) != 0;
    }

    /**
     * @brief ¿Hay resultado cacheado de @c A para @p unit Y de esta @p version?
     *
     * La pregunta que de verdad interesa antes de pedir algo versionado: si
     * contesta @c false, la siguiente llamada a @ref get_or_compute_v VA a
     * computar.  Existe porque @ref cached se queda corta justo donde importa,
     * y usarla para contar recomputos los cuenta de menos -- lo destapo
     * `test_fact_base_reuse`, que vio que subir la version no aumentaba la
     * cuenta de analisis ejecutados.
     */
    template <class A>
    bool cached_v(const std::string *unit, uint64_t version) const {
        const Shard &s = shard_of(unit);
        util::TimedSharedLock lk(s.m, shared_wait_slot());
        const auto it = s.results.find(Key{analysis_id<A>(), unit});
        return it != s.results.end() && it->second->version == version;
    }

    /// Invalida el resultado @c A de @p unit y, transitivamente, todo lo que
    /// dependia de el (ambos ejes).
    template <class A> void invalidate(const std::string *unit) {
        drop_key(Key{analysis_id<A>(), unit});
    }

    /// Invalida los resultados de @p unit que NO sobreviven a @p preserved
    /// (mecanismo PreservedAnalyses tras un pase).  Cascada por dependencias.
    void invalidate(const std::string *unit,
                    const PreservedAnalyses &preserved) {
        /* Por el indice, no barriendo `results_` entero.
         *
         * Antes esto recorria TODAS las entradas del gestor para quedarse con
         * las de UNA unidad, y lo llama cualquier pase que cambie algo -- o
         * sea, muchas veces por vuelta del punto fijo.  Con el corpus de hoy no
         * se nota (0,06 s en el perfil), pero el coste crece con el producto de
         * unidades por invalidaciones: es de orden equivocado, y eso se
         * descubre tarde y caro cuando alguien compila un modulo grande. */
        /* Cascada: el de cascada PRIMERO y el de la franja despues, que es el
         * orden fijo que descarta el bloqueo mutuo.  @see cascade_m_ */
        /* Primero el intento LOCAL, que es el caso comun: si todo lo que hay que
         * sacar de esta unidad cascadea dentro de su franja, el cerrojo global no
         * hace falta.  @see drop_within_shard */
        {
            Shard &s = shard_of(unit);
            util::TimedUniqueLock lk(s.m, exclusive_wait_slot());
            auto u = s.keys_by_unit.find(unit);
            if (u == s.keys_by_unit.end()) return;
            std::vector<Key> dead;
            for (const Key &k : u->second) {
                auto it = s.results.find(k);
                if (it != s.results.end() && !it->second->survives(preserved))
                    dead.push_back(k);
            }
            bool todo_local = true;
            for (const Key &k : dead)
                if (!drop_within_shard(s, k)) {
                    todo_local = false;
                    break;
                }
            if (todo_local) return;
        }
        util::TimedUniqueLock cl(cascade_m_, exclusive_wait_slot());
        std::vector<Key> con_dependientes;
        {
            /* EXCLUSIVO, y sin soltarlo entre mirar y sacar.
             *
             * Con el compartido habia una VENTANA: otro hilo podia guardar una
             * entrada RECIEN calculada entre las dos cosas, y se invalidaba
             * acto seguido.  Medido, eso recalculaba 220.000 analisis de mas --
             * tres por funcion -- y dejaba la compilacion mas lenta que con un
             * solo cerrojo.  Lo delato la cuenta de `nuevos`, no el reloj. */
            Shard &s = shard_of(unit);
            util::TimedUniqueLock lk(s.m, exclusive_wait_slot());
            auto u = s.keys_by_unit.find(unit);
            if (u == s.keys_by_unit.end()) return;
            std::vector<Key> dead;
            for (const Key &k : u->second) {
                auto it = s.results.find(k);
                if (it != s.results.end() && !it->second->survives(preserved))
                    dead.push_back(k);
            }
            for (const Key &k : dead) {
                /* Sin dependientes no hay cascada: se saca aqui mismo, con el
                 * cerrojo puesto, y no hay ventana ninguna.  Es el caso comun,
                 * porque las dependencias solo se apuntan en consultas
                 * anidadas. */
                if (s.rev_deps.find(k) == s.rev_deps.end()) {
                    s.results.erase(k);
                    index_remove(s, k);
                } else {
                    con_dependientes.push_back(k);
                }
            }
        }
        /* Y los que arrastran cascada, que puede cruzar de franja: ya se tiene
         * el cerrojo de cascada desde arriba. */
        for (const Key &k : con_dependientes)
            invalidate_key_locked(k);
    }

    /**
     * @brief Suelta lo que este hilo tenia cogido.
     *
     * Hay que llamarlo en un punto SEGURO: cuando el llamante ha terminado con
     * la unidad y ya no va a usar ninguna referencia que el gestor le diera.
     * En el bucle de pases, al cerrar cada funcion.
     *
     * Sin esto el respaldo crece sin fin -- cada resultado entregado quedaria
     * vivo hasta el final del proceso --, y con esto la memoria vuelve al
     * comportamiento de antes: solo sobrevive lo que el mapa siga guardando.
     */
    void release_retained() { retained().clear(); }

    /// Borra TODO (reconstruccion completa).
    void clear() {
        /* Toca TODAS las franjas, asi que va por el camino de cascada: con su
         * cerrojo puesto nadie mas esta recorriendo franjas. */
        util::TimedUniqueLock cl(cascade_m_, exclusive_wait_slot());
        for (Shard &s : shards_) {
            util::TimedUniqueLock lk(s.m, exclusive_wait_slot());
            s.results.clear();
            s.rev_deps.clear();
            s.keys_by_unit.clear();
        }
        stack().clear();
    }

    /// Cuantos resultados hay guardados, sumando las franjas.  Cada una con su
    /// cerrojo compartido: no hace falta una foto coherente del conjunto para
    /// contestar "cuantos hay", y pedirla serializaria a todo el mundo.
    size_t size() const {
        size_t n = 0;
        for (const Shard &s : shards_) {
            util::TimedSharedLock lk(s.m, shared_wait_slot());
            n += s.results.size();
        }
        return n;
    }

    /**
     * @brief Cuantas consultas se sirvieron del cache, cuantas encontraron el
     *        resultado caducado, y cuantas no tenian nada guardado.
     *
     * Es lo que dice si cachear aqui puede servir de algo: si casi todo sale
     * CADUCADO, la unidad cambia entre consultas y no hay reuso posible por
     * mucho que se afine el mecanismo.  Sin este dato, "vamos a cachearlo" es
     * una apuesta.
     */
    struct Counts {
        long long hits = 0;  ///< servidas del cache, con la version buena.
        long long stale = 0; ///< habia algo guardado, de otra version.
        long long fresh = 0; ///< no habia nada guardado.
        /**
         * @brief Cuanto se ESPERO por el cerrojo, y en cual.
         *
         * Cero cuando no se pidio medirlo.  Separados porque el compartido y el
         * exclusivo se arreglan de formas OPUESTAS: el compartido cuesta por
         * pelearse N hilos por una linea de cache (se trocea), el exclusivo
         * porque hay fallos que obligan a escribir (se reduce el churn de
         * versiones).  Juntos en un solo numero no distinguen las dos averias.
         */
        long long shared_wait_ns = 0;
        long long exclusive_wait_ns = 0;
    };
    Counts counts() const {
        return Counts{hits_.load(std::memory_order_relaxed),
                      stale_.load(std::memory_order_relaxed),
                      fresh_.load(std::memory_order_relaxed),
                      shared_wait_ns_.load(std::memory_order_relaxed),
                      exclusive_wait_ns_.load(std::memory_order_relaxed)};
    }

  private:
    /* ATOMICOS: los aciertos se cuentan ahora desde el camino rapido, que
     * corre bajo cerrojo COMPARTIDO -- varios hilos a la vez --.  Un contador
     * normal ahi seria una carrera, y ademas una que no falla: da un numero
     * ligeramente bajo y nadie se entera. */
    /* ATOMICOS los TRES.  Antes `stale_` y `fresh_` eran enteros normales y
     * bastaba, porque solo se tocaban con el cerrojo unico puesto.  Al trocear
     * las tablas dos hilos de franjas distintas los suman a la vez, asi que sin
     * esto habria una carrera -- y de las que no fallan: da un numero
     * ligeramente bajo y nadie se entera. */
    mutable std::atomic<long long> hits_{0};
    mutable std::atomic<long long> stale_{0};
    mutable std::atomic<long long> fresh_{0};

    /* Lo que se espero por entrar, en nanosegundos.  ATOMICOS por lo mismo que
     * los aciertos: se suman desde varios hilos a la vez. */
    mutable std::atomic<long long> shared_wait_ns_{0};
    mutable std::atomic<long long> exclusive_wait_ns_{0};

    /**
     * @brief A donde apuntar la espera, o NULO si nadie la pidio.
     *
     * La bandera se pregunta UNA vez por proceso y se guarda: preguntarla en
     * cada toma del cerrojo -- que son mas de un millon en una compilacion
     * grande -- costaria mas que lo que se mide.  Con la bandera apagada esto
     * devuelve nulo y la guarda no llega a leer el reloj.
     */
    static std::atomic<long long> *wait_slot(std::atomic<long long> *dst) {
        static const bool measuring = util::flag_on(util::FlagId::Times);
        return measuring ? dst : nullptr;
    }
    std::atomic<long long> *shared_wait_slot() const {
        return wait_slot(&shared_wait_ns_);
    }
    std::atomic<long long> *exclusive_wait_slot() const {
        return wait_slot(&exclusive_wait_ns_);
    }

    /**
     * @brief Saca @p k y, transitivamente, lo que dependia de el.
     *
     * OJO: NO bloquea NADA por su cuenta.  Se la llama con @ref cascade_m_ ya
     * puesto, porque la cascada puede CRUZAR de franja -- las dependencias
     * cruzan funciones, y dos funciones distintas caen en franjas distintas --
     * y entonces hay que tomar el cerrojo de mas de una.
     *
     * Ese es todo el motivo de que exista un cerrojo de cascada: con el puesto
     * solo hay UNA cascada a la vez, asi que tomar varias franjas seguidas no
     * puede cruzarse con otra que las tome en otro orden.  Y como nadie toma
     * nunca una franja y DESPUES la de cascada, el orden es fijo y el bloqueo
     * mutuo es imposible por construccion, no por cuidado.
     */
    void invalidate_key_locked(const Key &k) {
        std::vector<Key> deps;
        {
            Shard &s = shard_of(k.unit);
            util::TimedUniqueLock lk(s.m, exclusive_wait_slot());
            auto it = s.results.find(k);
            if (it == s.results.end()) return;
            s.results.erase(it);
            index_remove(s, k);
            auto d = s.rev_deps.find(k);
            if (d == s.rev_deps.end()) return;
            deps.assign(d->second.begin(), d->second.end());
            s.rev_deps.erase(d);
        }
        /* La cascada, con la franja de @p k ya SOLTADA: un dependiente puede
         * vivir en la misma, y volver a pedir su cerrojo se autobloquearia
         * (`SharedMutex` no es reentrante).  Soltar antes es seguro porque quien
         * llama sigue teniendo el de cascada: nadie mas esta cascadeando. */
        for (const Key &dep : deps)
            invalidate_key_locked(dep);
    }

    /**
     * @brief Una FRANJA de las tablas, con su propio cerrojo.
     *
     * Habia UNA tabla y UN cerrojo, y era el cuello de botella de compilar:
     * medido sobre 441.000 lineas, 1,25 millones de consultas de veinticuatro
     * hilos dejaban 35,6 segundos de espera acumulada -- el 52 % del CPU en las
     * primitivas de espera del sistema -- para un frontend de 8,5 segundos de
     * pared, con la maquina al 14 %.  No se esperaba por calcular nada: se
     * esperaba por ENTRAR.
     *
     * Con franjas, dos hilos que preguntan por funciones distintas no se ven ni
     * se pelean por la misma linea de cache.
     */
    struct Shard {
        /* COMPARTIDO para leer, exclusivo para escribir.  Los aciertos -- la
         * mayoria dentro del bucle repartido -- solo leen.
         *
         * Y es el NUESTRO, no `std::shared_mutex`: en MinGW ese se apoya en la
         * emulacion de pthreads, que SE ROMPE con hilos que nacen y mueren -- el
         * lote de hilos por nivel de modulos --.  Se vio aqui, con los 23 hilos
         * parados y la seccion critica VACIA, y se reprodujo fuera del
         * compilador en una sonda de sesenta lineas: cambiando solo el tipo del
         * cerrojo, `std::shared_mutex` moria 5 de 5 y `std::mutex` pasaba 5 de
         * 5.  Ver `util/shared_mutex.h`. */
        mutable util::SharedMutex m;

        /* `shared_ptr` y no `unique_ptr`, y no es un detalle: el gestor entrega
         * REFERENCIAS a lo que guarda, y la invalidacion cascadea por
         * dependencias que CRUZAN funciones -- calcular points-to de `f` pide
         * rangos de `g`, asi que invalidar `g` puede borrar entradas de `f`.
         * Con varios hilos, uno puede borrar justo lo que otro esta leyendo.
         *
         * Con `shared_ptr`, borrar del mapa solo suelta LA referencia del mapa:
         * el objeto sigue vivo mientras alguien lo tenga cogido (ver
         * `retained_`). */
        std::unordered_map<Key, std::shared_ptr<AnalysisResultConcept>, KeyHash>
            results;
        std::unordered_map<Key, std::unordered_set<Key, KeyHash>, KeyHash>
            rev_deps;
        /// Que analisis tiene cada unidad, para que invalidarla no obligue a
        /// recorrer el gestor entero.  Indexado por el nombre INTERNADO, como la
        /// clave: asi ni este indice copia cadenas.
        std::unordered_map<const std::string *, std::vector<Key>> keys_by_unit;
    };

    /// Cuantas franjas.  Potencia de dos para repartir con una mascara.
    static constexpr size_t kShards = 64;

    /**
     * @brief En que franja cae @p unit.
     *
     * Por la UNIDAD y no por la clave entera, a proposito: asi TODOS los
     * analisis de una misma funcion caen juntos, y el indice por unidad y su
     * invalidacion nunca cruzan de franja -- que es el caso comun --.  Cruzar
     * solo pasa siguiendo una dependencia entre funciones distintas.
     *
     * Se descartan los bits bajos: las unidades son punteros a nombres
     * internados y los bajos apenas varian entre reservas contiguas.
     */
    static size_t shard_index(const std::string *unit) {
        return (std::hash<const void *>()(unit) >> 4) & (kShards - 1);
    }
    Shard &shard_of(const std::string *unit) {
        return shards_[shard_index(unit)];
    }
    const Shard &shard_of(const std::string *unit) const {
        return shards_[shard_index(unit)];
    }

    mutable Shard shards_[kShards];

    /**
     * @brief El cerrojo de las CASCADAS.  Se toma SIEMPRE antes que el de una
     *        franja, y NUNCA despues.
     *
     * Una cascada puede tener que tocar varias franjas (las dependencias cruzan
     * funciones).  Con este puesto solo hay una cascada a la vez, asi que no
     * pueden cruzarse dos tomando franjas en ordenes distintos; y como nadie
     * toma una franja y luego esta, el orden global es fijo.  El bloqueo mutuo
     * queda descartado por CONSTRUCCION, no por revisar cada sitio.
     *
     * El camino caliente -- preguntar por lo propio -- no lo toca.
     */
    mutable util::SharedMutex cascade_m_;

    /// Apunta @p k en el indice de su franja, si no estaba.
    static void index_add(Shard &s, const Key &k) {
        auto &v = s.keys_by_unit[k.unit];
        for (const Key &x : v)
            if (x.id == k.id) return;
        v.push_back(k);
    }

    /// Quita @p k del indice.  La lista de una unidad son unas pocas entradas
    /// -- un analisis por tipo --, asi que buscar linealmente es mas rapido
    /// que cualquier estructura con indireccion.
    static void index_remove(Shard &s, const Key &k) {
        auto u = s.keys_by_unit.find(k.unit);
        if (u == s.keys_by_unit.end()) return;
        auto &v = u->second;
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i].id == k.id) {
                v[i] = v.back();
                v.pop_back();
                break;
            }
        if (v.empty()) s.keys_by_unit.erase(u);
    }

    /**
     * @brief Saca @p k y lo que dependiera de el.  Puerta para quien NO tiene
     *        ningun cerrojo de franja puesto.
     *
     * Intenta primero el camino CORTO, que es el comun: si la clave no tiene
     * dependientes no hay cascada, y sacarla es cosa de su franja y de nadie
     * mas.  Solo se pasa por el cerrojo global cuando de verdad hay que seguir
     * dependencias, que es lo unico capaz de cruzar de franja.
     *
     * Importa porque el camino de "caduco" es el 23 % de las consultas: tomar el
     * cerrojo global en todas ellas cambia un cuello de botella por otro --
     * medido, eso solo dejaba la compilacion mas lenta que antes de trocear --.
     * Y las dependencias se apuntan SOLO en consultas anidadas, asi que la
     * inmensa mayoria de las claves no tiene ninguna.
     *
     * Sin ventanas: cuando hay dependientes no se toca nada por el camino corto,
     * se abandona y se hace TODO bajo el cerrojo de cascada.  Asi la cascada
     * sigue siendo atomica, que es lo que evita servir un analisis que debia
     * haberse invalidado.
     */
    void drop_key(const Key &k) {
        {
            Shard &s = shard_of(k.unit);
            util::TimedUniqueLock lk(s.m, exclusive_wait_slot());
            if (drop_within_shard(s, k)) return;
        }
        /* Algun dependiente vive en OTRA franja: eso es lo unico que necesita el
         * cerrojo global, y por eso se rehace entero aqui.  No se ha tocado nada
         * arriba, asi que la cascada sigue siendo atomica. */
        util::TimedUniqueLock cl(cascade_m_, exclusive_wait_slot());
        invalidate_key_locked(k);
    }

    /**
     * @brief Intenta sacar @p k con su cascada SIN salir de @p s.
     *
     * Se llama con el cerrojo de @p s puesto en exclusiva, y no toca nada hasta
     * saber que puede terminar: primero recorre la cascada comprobando que todo
     * cae en esta misma franja, y solo entonces borra.  Si encuentra algo de
     * fuera, devuelve @c false sin haber modificado nada.
     *
     * @par Por que existe
     * "Sin dependientes no hay cascada" parecia cubrir el caso comun, y no
     * cubria NINGUNO: `PointsTo(f)` depende de `IRFacts(f)`, asi que invalidar
     * los hechos de una funcion SIEMPRE tiene un dependiente y siempre caia al
     * cerrojo global.  Medido con el analisis de hilos de VTune: un unico
     * cerrojo con **172.881 esperas y 40,4 de los 42,8 segundos de espera
     * total** -- el 94 % --, con la maquina al 7,2 % de aprovechamiento.
     *
     * Y lo que lo arregla es que esa dependencia NO cruza de franja: los dos son
     * la misma unidad, y la franja se elige por la unidad.  Cruzar solo ocurre
     * siguiendo una dependencia entre funciones distintas, que es lo raro.
     *
     * @param s La franja de @p k, con su cerrojo ya puesto.
     * @param k Que sacar.
     * @return true si quedo hecho aqui; false si hay que ir por el global.
     */
    bool drop_within_shard(Shard &s, const Key &k) {
        const size_t mine = shard_index(k.unit);
        /* Fase 1: el cierre transitivo, comprobando que no sale de la franja.
         * Sin tocar nada, para poder abandonar limpiamente. */
        std::vector<Key> victims;
        std::vector<Key> pending;
        pending.push_back(k);
        while (!pending.empty()) {
            const Key cur = pending.back();
            pending.pop_back();
            if (shard_index(cur.unit) != mine) return false; // cruza: al global
            if (s.results.find(cur) == s.results.end()) continue; // ya no esta
            bool ya = false;
            for (const Key &v : victims)
                if (v == cur) {
                    ya = true;
                    break;
                }
            if (ya) continue; // un ciclo de dependencias no cuelga esto
            victims.push_back(cur);
            auto d = s.rev_deps.find(cur);
            if (d == s.rev_deps.end()) continue;
            for (const Key &dep : d->second)
                pending.push_back(dep);
        }
        // Fase 2: todo es de aqui, asi que se saca.
        for (const Key &v : victims) {
            s.results.erase(v);
            index_remove(s, v);
            s.rev_deps.erase(v);
        }
        return true;
    }

    /* Lo que ESTE hilo tiene cogido.  Cada referencia entregada se respalda
     * aqui para que una cascada de otro hilo no pueda destruirla debajo.  Se
     * suelta en un punto seguro -- cuando el llamante termina con la unidad --
     * via `release_retained()`.
     *
     * Por RANURA y no por `thread_local`: en MinGW la TLS es emulada y cada
     * acceso es una llamada -- aqui hay cuatro por funcion y pasada --, y ese
     * `vector` tiene inicializador dinamico, que genera una guarda que se
     * bloquea con hilos que nacen y mueren.  Ademas @c util::ThreadOwned se
     * queda con lo creado y lo libera: si no, lo que dejara en su ranura un
     * hilo muerto no seria un vector perdido, seria mantener VIVOS los
     * resultados que ese vector respalda. */
    std::vector<std::shared_ptr<AnalysisResultConcept>> &retained() {
        return retained_.get();
    }
    util::ThreadOwned<std::vector<std::shared_ptr<AnalysisResultConcept>>>
        retained_;
    /* Por hilo: una pila de computos en curso describe lo que ESTE hilo esta
     * calculando.  Compartida, dos hilos registrarian sus dependencias contra
     * el computo del otro. */
    std::vector<Key> &stack() { return stack_.get(); } // ver retained()
    util::ThreadOwned<std::vector<Key>> stack_;
};

} // namespace analysis

#endif // VESTA_ANALYSIS_MANAGER_H
