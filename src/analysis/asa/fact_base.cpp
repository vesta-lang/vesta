/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/asa/fact_base.cpp
 * @brief Implementacion de la base de hechos (ver @c
 * analysis/asa/fact_base.h).
 */

#include "util/env_flags.h"
#include "analysis/asa/fact_base.h"

#include "util/fnv.h" // la mezcla del proyecto, no otra escrita aqui
#include "vx/diag/diag_catalog.h" // el texto de las trazas, en todos los idiomas

#include "analysis/asa/fact_store.h"
#include "analysis/asa/producers.h" // ModuleWalk: el recorrido, una sola vez
#include "ir/ssa_ir.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace analysis {
namespace asa {

/* El VOCABULARIO va en ingles, como los identificadores: estos nombres viajan
 * al fichero de hechos, al volcado y al MCP, y ahi los lee gente y herramientas
 * que no tienen por que saber espanol.  La PROSA -- comentarios, y el texto que
 * ve el usuario, que sale del catalogo multi-idioma -- es lo unico que no. */
const char *const kProducerStructure = "asa.structure";
const char *const kProducerRanges = "asa.ranges";
const char *const kProducerMemory = "asa.memory";
const char *const kProducerAddressSpace = "asa.address_space";
const char *const kProducerEffects = "asa.effects";
const char *const kProducerEscape = "asa.escape";
const char *const kProducerLayout = "asa.layout";
const char *const kProducerAsmFlow = "asa.asm_flow";
const char *const kProducerBoundary = "asa.boundary";
const char *const kProducerLoops = "asa.loops";
const char *const kProducerBulkMemory = "asa.bulk_memory";
const char *const kProducerCodeOrigin = "asa.code_origin";
const char *const kProducerBackend = "asa.backend";
const char *const kProducerOverlays = "asa.overlays";
const char *const kProducerDemandedBits = "asa.demanded_bits";
const char *const kProducerParamContracts = "asa.param_contracts";
const char *const kModuleUnit = "<module>";

AnchorLines::AnchorLines(const ir::IrFunction &fn) {
    /* UNA pasada, y las tres tablas a la vez: son la misma informacion mirada
     * por tres claves distintas, asi que recorrer tres veces solo cambiaria el
     * numero de veces que el modulo entra y sale de la cache. */
    by_value_.assign(fn.values.size(), 0);
    by_block_.assign(fn.blocks.size(), 0);
    size_t total = 0;
    for (const ir::IrBlock &b : fn.blocks)
        total += b.instrs.size();
    by_instr_.assign(total, 0);

    uint32_t idx = 0;
    for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        for (const ir::IrInstr &in : fn.blocks[bi].instrs) {
            by_instr_[idx++] = in.source_line;
            /* La linea del bloque es la de su primera instruccion QUE TENGA
             * una.  No la primera a secas: el intermedio mete operaciones
             * sinteticas -- copias de phi, saltos de relleno -- que no salen de
             * ninguna linea del usuario, y empezar por ellas daria cero. */
            if (by_block_[bi] == 0 && in.source_line > 0)
                by_block_[bi] = in.source_line;
            /* Y la de un valor es la de la instruccion que lo DEFINE. */
            if (in.dst != ir::IR_NO_VALUE && in.dst < by_value_.size())
                by_value_[in.dst] = in.source_line;
        }
    }
}

uint32_t AnchorLines::of(const Anchor &a) const {
    switch (a.kind) {
    case Anchor::Kind::Line:
        /* Ya es una linea: no cuelga de ninguna entidad del intermedio -- una
         * vista `@overlay` se declara en el fuente y no la produce ninguna
         * instruccion --, asi que no hay nada contra lo que resolverla.  Es la
         * unica clase que puede quedarse rancia, y por eso es el ultimo
         * recurso. */
        return a.id;
    case Anchor::Kind::Value:
        return a.id < by_value_.size() ? by_value_[a.id] : 0;
    case Anchor::Kind::Block:
        return a.id < by_block_.size() ? by_block_[a.id] : 0;
    case Anchor::Kind::Instruction:
        return a.id < by_instr_.size() ? by_instr_[a.id] : 0;
    default:
        /* `None`: el productor no dijo donde miro.  Cero, y que decida quien
         * pregunta -- inventar la linea 1 seria senalar un sitio que nadie ha
         * mirado, que es peor que no senalar ninguno. */
        return 0;
    }
}

void register_asa_canonical_names() {
    /* Perezoso y una sola vez, NO un objeto global.  Un inicializador estatico
     * reservaria memoria antes de main aunque nadie fuera a leer hechos de
     * disco, y eso corre las direcciones de todo lo que se reserve despues --
     * en un programa que dependa de la alineacion de lo suyo, algo asi cambia
     * si funciona o no.  Ademas evita el orden de inicializacion entre ficheros
     * objeto, que aqui importa porque la tabla vive en otro. */
    static const bool done = [] {
        register_canonical_name(kProducerStructure);
        register_canonical_name(kProducerRanges);
        register_canonical_name(kProducerMemory);
        register_canonical_name(kProducerAddressSpace);
        register_canonical_name(kProducerLayout);
        register_canonical_name(kProducerAsmFlow);
        register_canonical_name(kProducerBoundary);
        register_canonical_name(kProducerLoops);
        register_canonical_name(kProducerBulkMemory);
        register_canonical_name(kProducerBackend);
        register_canonical_name(kProducerOverlays);
        register_canonical_name(kProducerDemandedBits);
        register_canonical_name(kProducerParamContracts);
        register_canonical_name(kProducerEffects);
        register_canonical_name(kProducerEscape);
        register_canonical_name(kProducerCodeOrigin);
        register_canonical_name(kModuleUnit);
        return true;
    }();
    (void)done;
}

/// Marcadores de identidad para el gestor.  Uno por dominio: la cache va por
/// (analisis, unidad), asi que dos dominios distintos no se pisan.
///
/// Cada uno declara ademas @c kName , y no es decorativo: @ref
/// FactBase::memoized lo exige, asi que un analisis nuevo que se olvide de
/// ponerlo NO COMPILA en vez de aparecer sin medir.  El nombre va en INGLES,
/// como el resto del vocabulario que sale impreso.
namespace {
struct MemoryAnalysis {
    static char ID;
    static constexpr const char *kName = "memory";
};
/* `LoopsAnalysis` ya NO esta aqui: vive en `analysis/facts/loop_facts.h`, junto
 * a su dominio y como los demas.  Escondido en esta unidad dejaba al gestor
 * fuera del alcance de los seis consumidores de bucles, que por eso llamaban al
 * productor a pelo. */
struct IvBoundsAnalysis {
    static char ID;
    static constexpr const char *kName = "iv_bounds";
};
struct BoundaryAnalysis {
    static char ID;
    static constexpr const char *kName = "boundary";
};
struct EffectsSummaryAnalysis {
    static char ID;
    static constexpr const char *kName = "effects";
};
struct ParamAliasingAnalysis {
    static char ID;
    static constexpr const char *kName = "param_aliasing";
};
struct ModuleWalkAnalysis {
    static char ID;
    static constexpr const char *kName = "module_walk";
};
struct EscapeAnalysisId {
    static char ID;
    static constexpr const char *kName = "escape";
};
char MemoryAnalysis::ID = 0;
/* `LoopsAnalysis::ID` se define en `loop_facts.cpp`, con su dominio. */
char IvBoundsAnalysis::ID = 0;
char BoundaryAnalysis::ID = 0;
char EffectsSummaryAnalysis::ID = 0;
char ParamAliasingAnalysis::ID = 0;
char ModuleWalkAnalysis::ID = 0;
char EscapeAnalysisId::ID = 0;
} // namespace

/**
 * @brief Ordena dos analisis por lo que costaron, el mas caro primero.
 *
 * Con NOMBRE y no una lambda: una lambda no sale con el suyo en un perfil, que
 * es donde se mira cuando algo cuesta.
 *
 * @param a Un reparto.
 * @param b El otro.
 * @return true si @p a costo mas.
 */
static bool analysis_costlier_first(const FactBase::AnalysisStats *a,
                                    const FactBase::AnalysisStats *b) {
    return a->micros > b->micros;
}

bool FactBase::telemetry_on() noexcept {
    /* UNA vez por proceso.  Es lo que hace que medir sea gratis cuando no se
     * mide: apagado queda una rama sobre un booleano ya resuelto, que el
     * predictor acierta siempre, y ni se lee el reloj ni se toca contador
     * alguno. */
    static const bool on = util::flag_on(util::FlagId::Times);
    return on;
}

FactBase::AnalysisStats &FactBase::stats_slot(AnalysisID id, const char *name) {
    /* Busqueda lineal sobre un vector plano: son un punado de analisis -- once
     * hoy -- y a esa escala recorrerlos gana a cualquier tabla hash, que ademas
     * reservaria.  Y solo se llega aqui con la telemetria encendida. */
    for (size_t i = 0; i < analysis_ids_.size(); ++i)
        if (analysis_ids_[i] == id) return analysis_stats_[i];
    analysis_ids_.push_back(id);
    analysis_stats_.push_back(AnalysisStats{});
    analysis_stats_.back().name = name;
    return analysis_stats_.back();
}

FactBase::FactBase() {
    register_asa_canonical_names();
}

FactBase::FactBase(const char *stage) {
    register_asa_canonical_names();
    /* Nulo o vacio se queda con el de por defecto: una base sin momento
     * declarado habla de lo que el compilador tiene delante antes de
     * optimizar, que es de donde parte todo. */
    if (stage != nullptr && stage[0] != '\0') default_stage_ = stage;
}

FactBase::~FactBase() {
    /* El reparto POR ANALISIS, si se pidieron tiempos.  Va aqui y no en quien
     * usa la base porque las bases nacen y mueren en varios sitios -- una por
     * momento, otra corta solo para los prestamos -- y ninguno de ellos las ve
     * todas: dejarselo a cada uno significaba que la mayoria no se contaba. */
    if (telemetry_on() && !analysis_stats_.empty()) {
        std::vector<const AnalysisStats *> by_cost;
        by_cost.reserve(analysis_stats_.size());
        for (const AnalysisStats &s : analysis_stats_)
            by_cost.push_back(&s);
        std::sort(by_cost.begin(), by_cost.end(), analysis_costlier_first);
        for (const AnalysisStats *s : by_cost) {
            const std::string msg = vx::diag::format(
                "VXA079", vx::diag::current_language(),
                {s->name != nullptr ? s->name : "?", std::to_string(s->micros),
                 std::to_string(s->computes), std::to_string(s->queries),
                 default_stage_});
            std::fprintf(stderr, "[asa] %s\n", msg.c_str());
        }
    }
    static const bool log_it = util::flag_on(util::FlagId::AsaFactsDebug);
    if (!log_it || queries_ == 0) return;
    dump_facts(dump(), stderr);
    /* Por el CATALOGO, como el resto: esto lo lee una persona, y una traza
     * escrita a mano solo esta en un idioma. */
    const std::string msg = vx::diag::format(
        "VXA076", vx::diag::current_language(),
        {std::to_string(queries_), std::to_string(computations_)});
    std::fprintf(stderr, "%s\n", msg.c_str());
}

const std::string *FactBase::key_of(const ir::IrFunction &fn,
                                    const char *stage) {
    /* El MOMENTO va en la clave, siempre.  Sin el, lo que se analizo antes de
     * optimizar se sirve despues, y entonces se contesta sobre un codigo que ya
     * no existe: valores que el optimizador borro, bloques que fusiono.  Eso no
     * da un error, da respuestas equivocadas -- que es la forma cara de
     * fallar --, y es lo que ya llevaba `effects` desde el principio mientras
     * el resto se quedo fuera. */
    const char *st = (stage != nullptr) ? stage : "";
    if (!fn.name.empty()) return util::intern_name(fn.name + "@" + st);
    /* Anonima: la direccion la identifica sin ambiguedad mientras viva, y una
     * base no sobrevive al modulo cuyas funciones consulta. */
    char buf[40];
    std::snprintf(buf, sizeof buf, "<anonima:%p>",
                  static_cast<const void *>(&fn));
    /* Internado tambien: asi la clave es un puntero venga de donde venga, y
     * dos consultas sobre la misma funcion anonima dan el mismo. */
    return util::intern_name(std::string(buf) + "@" + st);
}

uint64_t FactBase::module_version(const ir::IrModule &mod) noexcept {
    /* Plegado, no suma: dos funciones que se intercambian versiones -- una sube
     * y otra baja -- darian la misma suma y el resultado se serviria rancio.
     * Con la posicion dentro de la mezcla, no. */
    uint64_t h = util::kFnvOffset;
    for (const ir::IrFunction &fn : mod.functions)
        h = util::fnv_mix(h, fn.version);
    return h;
}

const std::string *FactBase::module_key(const char *stage) {
    return util::intern_name(std::string(kModuleUnit) + "@" +
                             (stage != nullptr ? stage : ""));
}

void FactBase::mark(const char *producer, const std::string &key, Certainty c,
                    const char *support) {
    Seal s;
    s.certainty = c;
    s.origin.producer = producer;
    if (support != nullptr) s.support.add(support);
    auto &table = seals_[producer];
    auto &stored = table[key];
    stored = s;
    /* La procedencia apunta a la CLAVE, no al nombre de la funcion: la clave
     * vive en el mapa tanto como el sello, y el nodo no se mueve al crecer.
     * Apuntar al nombre de la funcion dejaria un puntero colgando en cuanto la
     * funcion consultada muriera antes que la base. */
    stored.origin.function = table.find(key)->first.c_str();
}

const IrFacts &FactBase::structure(const ir::IrFunction &fn,
                                   const char *stage) {
    const std::string *key = key_of(fn, stage_or_default(stage));
    /* Con la VERSION: `cached` a secas dice "hay algo guardado", que no es lo
     * mismo que "se va a reutilizar".  Preguntarlo sin ella contaba de menos
     * los recomputos y, peor, se saltaba el sello -- lo destapo el test de
     * reuso. */
    const bool fresh = !manager_.cached_v<IRFactsAnalysis>(key, fn.version);
    if (fresh) {
        /* Un recorrido, sin reticulo ni punto fijo: lo que sale de aqui esta
         * DEMOSTRADO, no inferido.  Los def-use y el CFG son lo que el IR dice,
         * no una aproximacion de lo que podria pasar. */
        mark(kProducerStructure, *key, Certainty::Proven);
    }
    /* Por la puerta VERSIONADA.  Estos hechos guardan punteros a instrucciones,
     * asi que servir uno de antes de una mutacion no es imprecision: es leer
     * memoria liberada.  La version la sube el optimizador en un solo sitio, y
     * el gestor recalcula si no coincide -- lo que sustituye a "acordarse de
     * invalidar", que es una obligacion que no se puede comprobar. */
    return memoized<IRFactsAnalysis, IrFacts>(
        fresh, key, fn.version,
        [this, &fn, stage]() { return structure_from_store_(fn, stage); });
}

std::shared_ptr<const RangeFacts>
FactBase::ranges_from_store_(const ir::IrFunction &fn, const char *stage) {
    /* Sin almacen, lo de siempre.  Y el marcador de quien pregunta se pone
     * igual en los dos caminos: es instrumentacion del motor de rangos y no
     * tiene nada que ver con si hubo cache. */
    const RangeRequester asker(RangeAsker::FactBase);
    const uint64_t ir_key = function_code_key(fn);

    if (analysis_store_ != nullptr) {
        const uint64_t k =
            analysis_store_->key_of(kRangeFactsAnalysisName, kRangeFactsFormat,
                                    ir_key, stage_or_default(stage));
        std::vector<uint8_t> bytes;
        if (analysis_store_->load(k, bytes)) {
            /* Se arma un `RangeFacts` y se entrega por puntero, que es como lo
             * guarda el gestor: copiarlo seria duplicar el estado por bloque
             * que lleva dentro. */
            auto restored = std::make_shared<RangeFacts>();
            if (deserialize_range_facts(bytes.data(), bytes.size(), ir_key,
                                        fn.values.size(), *restored))
                return restored;
            /* Estaba y no se pudo interpretar.  Se DICE -- "no habia" y "habia
             * y estaba roto" se arreglan distinto -- y se computa. */
            analysis_store_->note_rejected();
        }
    }

    /* Con las cotas de induccion, que las saca el PROPIO motor de rangos.  Es
     * conocimiento que los rangos no pueden deducir solos -- la guarda de un
     * bucle desenrollado compara `i + 7`, y despejar la `i` con aritmetica que
     * envuelve es incorrecto --, y sin ellas la variable del bucle vale TODO SU
     * TIPO.
     *
     * Antes se pasaban desde aqui, y eso las dejaba en su version pobre:
     * `compute_loop_iv_bounds` solo despeja limites CONSTANTES ESCRITOS, y en
     * un programa real eso dejaba sin cota al 89 % de los bucles contados.  El
     * motor las saca ESCALONADAS -- rangos sin cotas, cotas con esos rangos,
     * rangos con las cotas --, que recupera los limites que no son un literal
     * sin cerrar el circulo.  Pasarlas desde aqui SALTABA ese escalon. */
    std::shared_ptr<const RangeFacts> computed = compute_ranges_ptr(
        fn, structure(fn, stage), RangeOptions{}, nullptr, nullptr);
    if (analysis_store_ != nullptr && computed != nullptr) {
        const uint64_t k =
            analysis_store_->key_of(kRangeFactsAnalysisName, kRangeFactsFormat,
                                    ir_key, stage_or_default(stage));
        analysis_store_->store(k, serialize_range_facts(*computed));
    }
    return computed;
}

IrFacts FactBase::structure_from_store_(const ir::IrFunction &fn,
                                        const char *stage) {
    /* Sin almacen, lo de siempre: se computa.  No tener cache nunca puede ser
     * un error, asi que este camino no avisa de nada. */
    if (analysis_store_ == nullptr) return build_ir_facts(fn);

    /* La clave sale del CONTENIDO de la funcion, no de su nombre ni de su
     * posicion: dos compilaciones de una funcion que no cambio dan la misma,
     * aunque el resto del modulo si haya cambiado.  Y como es del contenido,
     * dos funciones con el mismo intermedio comparten el analisis -- que es
     * correcto, porque el def-use de un codigo identico es identico. */
    const uint64_t k =
        analysis_store_->key_of(kIrFactsAnalysisName, kIrFactsFormat,
                                function_code_key(fn), stage_or_default(stage));

    std::vector<uint8_t> bytes;
    if (analysis_store_->load(k, bytes)) {
        IrFacts f;
        if (deserialize_ir_facts(bytes.data(), bytes.size(), fn, f)) return f;
        /* Estaba pero no se pudo interpretar.  Se DICE -- es lo que separa "no
         * habia" de "habia y estaba roto", que se arreglan de formas distintas
         * -- y se computa, que sigue siendo correcto. */
        analysis_store_->note_rejected();
    }
    IrFacts f = build_ir_facts(fn);
    analysis_store_->store(k, serialize_ir_facts(f));
    return f;
}

const DemandedBits &FactBase::demanded(const ir::IrFunction &fn,
                                       const char *stage) {
    ++queries_;
    const std::string *key = key_of(fn, stage_or_default(stage));
    const bool fresh =
        !manager_.cached_v<DemandedBitsAnalysis>(key, fn.version);
    /* Por el gestor, como los rangos: los tres que preguntan -- el pase que
     * quita normalizaciones, el productor del dominio y quien venga -- acaban
     * en la MISMA instancia en vez de recalcularla cada uno.  Esa es la unica
     * forma de que anadir un consumidor no cueste otro analisis. */
    const DemandedBits &db =
        *memoized<DemandedBitsAnalysis, std::shared_ptr<const DemandedBits>>(
            fresh, key, fn.version, [&fn]() {
                return std::make_shared<const DemandedBits>(
                    compute_demanded_bits(fn));
            });
    if (fresh) {
        /* La certeza sale del analisis: llegar a punto fijo es haber visto
         * todo lo que podia contradecirlo; cortar por la cota deja una cota
         * valida pero no demostrada al maximo. */
        mark(kProducerDemandedBits, *key,
             db.converged ? Certainty::Proven : Certainty::Inferred, nullptr);
    }
    return db;
}

const RangeFacts &FactBase::ranges(const ir::IrFunction &fn,
                                   const char *stage) {
    ++queries_;
    const std::string *key = key_of(fn, stage_or_default(stage));
    const bool fresh = !manager_.cached_v<RangeAnalysis>(key, fn.version);
    /* La factoria pide la estructura POR LA BASE, no por su cuenta: asi el
     * gestor anota que los rangos dependen de ella y una invalidacion arrastra
     * a los dos.  Pedirla aparte dejaria rangos vivos sobre hechos muertos. */
    /* El gestor guarda el PUNTERO, no una copia.  `RangeFacts` lleva dentro el
     * estado de entrada de cada bloque, asi que copiarlo es duplicar el
     * analisis entero, y debajo ya existe UNA instancia en la cache por
     * dependencias.  Los otros dos gestores que piden rangos -- el del
     * optimizador y el de efectos -- apuntan a la MISMA. */
    const RangeFacts &rf =
        *memoized<RangeAnalysis, std::shared_ptr<const RangeFacts>>(
            fresh, key, fn.version, [this, &fn, stage]() {
                /* Con las cotas de induccion, que las saca el PROPIO
                 * motor de rangos.  Es conocimiento que los rangos no
                 * pueden deducir solos -- la guarda de un bucle
                 * desenrollado compara `i + 7`, y despejar la `i` con
                 * aritmetica que envuelve es incorrecto --, y sin ellas
                 * la variable del bucle vale TODO SU TIPO.
                 *
                 * Antes se pasaban desde aqui, y eso las dejaba en su
                 * version pobre: `compute_loop_iv_bounds` solo despeja
                 * limites CONSTANTES ESCRITOS, y en un programa real eso
                 * dejaba sin cota al 89 % de los bucles contados.  El
                 * motor las saca ESCALONADAS -- rangos sin cotas, cotas
                 * con esos rangos, rangos con las cotas --, que recupera
                 * los limites que no son un literal sin cerrar el
                 * circulo.  Pasarlas desde aqui SALTABA ese escalon. */
                return ranges_from_store_(fn, stage);
            });
    if (fresh) {
        /* La certeza sale del propio analisis, no de quien pregunta: llegar a
         * punto fijo es haber visto todo lo que podia contradecirlo; pararse
         * por presupuesto es "hasta aqui he llegado", que sostiene una decision
         * con red pero no permite quitar una comprobacion. */
        mark(kProducerRanges, *key,
             rf.convergio ? Certainty::Proven : Certainty::Inferred,
             kProducerStructure);
    }
    return rf;
}

const PointsTo &FactBase::memory(const ir::IrFunction &fn, const char *stage) {
    ++queries_;
    const std::string *key = key_of(fn, stage_or_default(stage));
    const bool fresh = !manager_.cached_v<MemoryAnalysis>(key, fn.version);
    if (fresh) {
        /* El conjunto de sitios a los que un puntero PUEDE referirse es una
         * sobre-aproximacion COMPLETA: nada que no este dentro puede ocurrir.
         * Que un puntero concreto quede en "cualquier cosa" no rebaja el hecho
         * -- eso lo dice la propia entrada, no su certeza. */
        mark(kProducerMemory, *key, Certainty::Proven, kProducerStructure);
    }
    /* Versionado, y aqui es donde mas importa: `PointsTo` se apoya en los
     * hechos de estructura, que guardan punteros a instrucciones. */
    return memoized<MemoryAnalysis, PointsTo>(
        fresh, key, fn.version,
        [this, &fn, stage]() { return memory_from_store_(fn, stage); });
}

PointsTo FactBase::memory_from_store_(const ir::IrFunction &fn,
                                      const char *stage) {
    if (analysis_store_ == nullptr)
        return compute_points_to(fn, structure(fn, stage));

    const uint64_t k =
        analysis_store_->key_of(kPointsToAnalysisName, kPointsToFormat,
                                function_code_key(fn), stage_or_default(stage));
    std::vector<uint8_t> bytes;
    if (analysis_store_->load(k, bytes)) {
        PointsTo restored;
        if (deserialize_points_to(bytes.data(), bytes.size(), fn.values.size(),
                                  restored))
            return restored;
        /* Estaba y no se pudo interpretar.  Se DICE -- "no habia" y "habia y
         * estaba roto" se arreglan distinto -- y se computa. */
        analysis_store_->note_rejected();
    }
    PointsTo computed = compute_points_to(fn, structure(fn, stage));
    analysis_store_->store(k, serialize_points_to(computed));
    return computed;
}

/* A quien le pregunta el analisis de espacios por los punteros: A LA BASE, que
 * es donde ya estan resueltos y cacheados.  Funcion con NOMBRE y contexto,
 * como el resto de oraculos. */
namespace {
struct AddressSpacePtCtx {
    FactBase *base;
    const char *stage;
};

const PointsTo &address_space_points_to(void *ctx, const ir::IrFunction &fn) {
    AddressSpacePtCtx *c = static_cast<AddressSpacePtCtx *>(ctx);
    return c->base->memory(fn, c->stage);
}
} // namespace

const AddressSpaces &FactBase::address_spaces(const ir::IrFunction &fn,
                                              const char *stage) {
    ++queries_;
    const std::string *key = key_of(fn, stage_or_default(stage));
    const bool fresh =
        !manager_.cached_v<AddressSpaceAnalysis>(key, fn.version);
    if (fresh) {
        /* Se APOYA en la memoria -- emparejar la escritura con la lectura es
         * cosa de points-to, y este no lo rehace --, y aquella a su vez en la
         * estructura: la cadena es transitiva.  Decirlo es lo que permite que
         * invalidar un eslabon tire de este, en vez de servir una respuesta
         * calculada sobre otro codigo.
         *
         * Y lo que se afirma es PROBADO por construccion: donde no se pudo
         * probar, la entrada no dice @c Host ni @c Machine -- dice que no se
         * sabe, con su motivo --, asi que no hay respuestas de media certeza
         * que sellar aparte. */
        mark(kProducerAddressSpace, *key, Certainty::Proven, kProducerMemory);
    }
    return memoized<AddressSpaceAnalysis, AddressSpaces>(
        fresh, key, fn.version,
        [this, &fn, stage]() { return address_spaces_from_store_(fn, stage); });
}

AddressSpaces FactBase::address_spaces_from_store_(const ir::IrFunction &fn,
                                                   const char *stage) {
    AddressSpacePtCtx pt_ctx{this, stage};
    PointsToOracle oracle;
    oracle.ask = &address_space_points_to;
    oracle.ctx = &pt_ctx;

    if (analysis_store_ == nullptr)
        return compute_address_spaces(fn, structure(fn, stage), oracle);

    const uint64_t k =
        analysis_store_->key_of(kAddressSpaceAnalysisName, kAddressSpaceFormat,
                                function_code_key(fn), stage_or_default(stage));
    std::vector<uint8_t> bytes;
    if (analysis_store_->load(k, bytes)) {
        AddressSpaces restored;
        if (deserialize_address_spaces(bytes.data(), bytes.size(),
                                       fn.values.size(), restored))
            return restored;
        /* Estaba y no se pudo interpretar.  Se DICE -- "no habia" y "habia y
         * estaba roto" se arreglan distinto -- y se computa. */
        analysis_store_->note_rejected();
    }
    AddressSpaces computed =
        compute_address_spaces(fn, structure(fn, stage), oracle);
    analysis_store_->store(k, serialize_address_spaces(computed));
    return computed;
}

const LoopFacts &FactBase::loops(const ir::IrFunction &fn, const char *stage) {
    ++queries_;
    const std::string *key = key_of(fn, stage_or_default(stage));
    const bool fresh = !manager_.cached_v<LoopsAnalysis>(key, fn.version);
    if (fresh) {
        mark(kProducerLoops, *key, Certainty::Proven);
    }
    return memoized<LoopsAnalysis, LoopFacts>(
        fresh, key, fn.version, [&fn]() { return compute_loop_facts(fn); });
}

const LoopIvBounds &FactBase::iv_bounds(const ir::IrFunction &fn,
                                        const char *stage) {
    ++queries_;
    const std::string *key = key_of(fn, stage_or_default(stage));
    const bool fresh = !manager_.cached_v<IvBoundsAnalysis>(key, fn.version);
    if (fresh) {
        /* Demostrado: sale de la FORMA del bucle y de constantes escritas, sin
         * punto fijo que pueda pararse por presupuesto ni aproximacion que
         * pueda quedarse corta.  Por eso puede alimentar a los rangos y no al
         * reves -- si preguntara, se morderian la cola. */
        mark(kProducerLoops, *key, Certainty::Proven, kProducerStructure);
    }
    return memoized<IvBoundsAnalysis, LoopIvBounds>(
        fresh, key, fn.version, [this, &fn, stage]() {
            return compute_loop_iv_bounds(fn, structure(fn, stage),
                                          loops(fn, stage));
        });
}

const RangeSummaries &FactBase::boundary(const ir::IrModule &mod,
                                         const char *stage) {
    ++queries_;
    // Internada tambien: la clave es un puntero, hable de una funcion o del
    // modulo entero.  Y con el momento, como todas.
    const std::string *key = module_key(stage_or_default(stage));
    const bool fresh =
        !manager_.cached_v<BoundaryAnalysis>(key, module_version(mod));
    const RangeSummaries &rs = memoized<BoundaryAnalysis, RangeSummaries>(
        fresh, key, module_version(mod),
        [&mod]() { return compute_range_summaries(mod); });
    if (fresh) {
        /* Sin punto fijo del grafo de llamadas los resumenes se abren solos, y
         * entonces lo que se sabe es nada -- no algo menos preciso. */
        mark(kProducerBoundary, *key,
             rs.convergio ? Certainty::Proven : Certainty::Unknown,
             kProducerStructure);
    }
    return rs;
}

const ModuleWalk &FactBase::walk(const ir::IrModule &mod, const char *stage) {
    ++queries_;
    const std::string *key = module_key(stage_or_default(stage));
    const bool fresh =
        !manager_.cached_v<ModuleWalkAnalysis>(key, module_version(mod));
    /* Y con la version del MoDULO plegada: el recorrido dice quien llama a
     * quien, asi que cambiar cualquier funcion puede cambiarlo. */
    return memoized<ModuleWalkAnalysis, ModuleWalk>(
        fresh, key, module_version(mod),
        [&mod]() { return ModuleWalk::of(mod); });
}

const effects::ParamAliasing &FactBase::param_aliasing(const ir::IrModule &mod,
                                                       const char *stage) {
    ++queries_;
    /* La clave lleva el MOMENTO, por lo mismo que la de los efectos: esto se
     * apoya en ellos, asi que compartirlo entre momentos le daria a uno el
     * resumen de un codigo que ya no existe. */
    const std::string *key = module_key(stage_or_default(stage));
    const bool fresh =
        !manager_.cached_v<ParamAliasingAnalysis>(key, module_version(mod));
    /* Fuera de la lambda: pedir el recorrido tambien es una consulta a la base,
     * y meterla dentro la ataria a que la lambda se ejecute -- que es justo lo
     * que no pasa cuando ya esta cacheado. */
    const ModuleWalk &w = walk(mod);
    const effects::ParamAliasing &pa =
        memoized<ParamAliasingAnalysis, effects::ParamAliasing>(
            fresh, key, module_version(mod), [&mod, &w, stage, this]() {
                return effects::ParamAliasing(mod, w, *this, stage);
            });
    if (fresh) {
        /* Lo que se sella aqui es el ANALISIS, no cada respuesta: esta montado
         * sobre el resolutor de punteros y no anade suposiciones propias.  Que
         * un par concreto salga "no se" viaja en la respuesta de esa consulta,
         * que es donde tiene que ir. */
        mark(kProducerParamContracts, *key, Certainty::Proven, kProducerMemory);
    }
    return pa;
}

effects::EffectAnalysis &FactBase::effects(const ir::IrModule &mod,
                                           const char *stage) {
    ++queries_;
    /* La clave lleva el MOMENTO.  Sin el, el resumen que tomo el optimizador al
     * empezar se le entregaria al comprobador de regiones despues de que el
     * modulo haya cambiado: un resumen de codigo que ya no existe.  No falla --
     * avisa de accesos que ya no estan, o deja de avisar de uno real. */
    const std::string *key = module_key(stage_or_default(stage));
    const bool fresh =
        !manager_.cached_v<EffectsSummaryAnalysis>(key, module_version(mod));
    /* Se guarda por PUNTERO: el motor lleva dentro sus tablas y el resumen
     * guarda referencias a ellas, asi que copiarlo al meterlo en la cache
     * dejaria el resumen apuntando a las tablas de la copia vieja. */
    const std::shared_ptr<effects::EffectAnalysis> &engine =
        memoized<EffectsSummaryAnalysis,
                 std::shared_ptr<effects::EffectAnalysis>>(
            fresh, key, module_version(mod), [&mod]() {
                auto e = std::make_shared<effects::EffectAnalysis>();
                e->module_summary(mod); // deja el motor con sus tablas listas
                return e;
            });
    if (fresh) {
        /* Lo que sale de recorrer el grafo de llamadas entero es demostrado; lo
         * que se queda a medias -- una nativa sin declarar, un puntero a
         * funcion sin resolver -- lo dice el propio resumen en sus lagunas, y
         * el sello no puede afirmar mas que el. */
        mark(kProducerEffects, *key, Certainty::Proven, kProducerStructure);
    }
    return *engine;
}

const std::unordered_map<std::string, EscapeInfo> &
FactBase::escape(const ir::IrModule &mod) {
    ++queries_;
    // Internada tambien: la clave es un puntero, hable de una funcion o del
    // modulo entero.
    const std::string *key = util::intern_name(kModuleUnit);
    const bool fresh =
        !manager_.cached_v<EscapeAnalysisId>(key, module_version(mod));
    /* La estructura y la memoria de cada funcion se piden POR LA BASE, no
     * aparte: asi el punto fijo del escape reusa lo que ya haya y una
     * invalidacion arrastra a los dos. */
    const auto &res =
        memoized<EscapeAnalysisId, std::unordered_map<std::string, EscapeInfo>>(
            fresh, key, module_version(mod), [this, &mod]() {
                auto facts_of =
                    [this](const ir::IrFunction &f) -> const IrFacts & {
                    return structure(f);
                };
                auto pt_of =
                    [this](const ir::IrFunction &f) -> const PointsTo & {
                    return memory(f);
                };
                return compute_escape_module(mod, facts_of, pt_of);
            });
    if (fresh) {
        /* El punto fijo se cierra sobre el grafo de llamadas: un callee que no
         * se ve captura TODO, que es la respuesta correcta sin su cuerpo.  Lo
         * que sale de ahi esta demostrado. */
        mark(kProducerEscape, *key, Certainty::Proven, kProducerMemory);
    }
    return res;
}

void FactBase::invalidate(const ir::IrFunction &fn) {
    /* Con el momento POR DEFECTO de la base.  Invalidar es "esta funcion ha
     * cambiado", y quien la cambia esta trabajando en un momento concreto: lo
     * de los OTROS momentos habla de otro codigo y no le afecta -- lo pre-opt
     * sigue siendo cierto de lo pre-opt aunque el optimizador ya haya pasado.
     */
    const std::string *key = key_of(fn, default_stage_);
    /* La estructura arrastra en cascada a todo lo que se derivo de ella; los
     * demas se descartan tambien de forma explicita por si alguien los pidio
     * antes de que existiera esa dependencia. */
    manager_.invalidate<IRFactsAnalysis>(key);
    manager_.invalidate<RangeAnalysis>(key);
    manager_.invalidate<MemoryAnalysis>(key);
    manager_.invalidate<LoopsAnalysis>(key);
    /* Y su sello con ellos: un hecho muerto que deja su procedencia atras hace
     * que el volcado afirme lo que ya no se sabe. */
    for (auto &domain : seals_)
        domain.second.erase(*key);
}

Seal FactBase::seal(const char *producer, const ir::IrFunction &fn) const {
    auto d = seals_.find(producer);
    if (d == seals_.end()) return Seal{};
    auto it = d->second.find(*key_of(fn, default_stage_));
    /* Nadie ha preguntado todavia: no se sabe nada, que no es lo mismo que
     * saber que no hay nada. */
    if (it == d->second.end()) return Seal{};
    return it->second;
}

Seal FactBase::module_seal(const char *producer) const {
    auto d = seals_.find(producer);
    if (d == seals_.end()) return Seal{};
    auto it = d->second.find(kModuleUnit);
    if (it == d->second.end()) return Seal{};
    return it->second;
}

std::vector<RecordedFact> FactBase::dump() const {
    std::vector<RecordedFact> out;
    for (const auto &domain : seals_)
        for (const auto &pair : domain.second)
            out.push_back({domain.first, pair.first, pair.second});
    /* Orden estable: dos volcados del mismo programa deben poder compararse. */
    std::sort(out.begin(), out.end(),
              [](const RecordedFact &a, const RecordedFact &b) {
                  if (a.function != b.function) return a.function < b.function;
                  return std::strcmp(a.domain, b.domain) < 0;
              });
    return out;
}

void dump_facts(const std::vector<RecordedFact> &entries, FILE *out) {
    for (const RecordedFact &h : entries) {
        std::fprintf(out, "[hechos] %-16s %-32s certeza=%s", h.domain,
                     h.function.c_str(), certainty_name(h.seal.certainty));
        for (int i = 0; i < Support::kMax; ++i)
            if (h.seal.support.on[i] != nullptr)
                std::fprintf(out, " sobre=%s", h.seal.support.on[i]);
        std::fprintf(out, "\n");
    }
}

} // namespace asa
} // namespace analysis
