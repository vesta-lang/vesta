/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/asa/base_hechos.cpp
 * @brief Implementacion de la base de hechos (ver @c analysis/asa/base_hechos.h).
 */

#include "analysis/asa/base_hechos.h"

#include "analysis/asa/fact_store.h"
#include "ir/ssa_ir.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace analysis {
namespace asa {

const char *const kProductorEstructura = "asa.estructura";
const char *const kProductorRangos = "asa.rangos";
const char *const kProductorMemoria = "asa.memoria";
const char *const kProductorFrontera = "asa.frontera";
const char *const kProductorBucles = "asa.bucles";
const char *const kUnidadModulo = "<modulo>";

void register_asa_canonical_names() {
    /* Perezoso y una sola vez, NO un objeto global.  Un inicializador estatico
     * reservaria memoria antes de main aunque nadie fuera a leer hechos de
     * disco, y eso corre las direcciones de todo lo que se reserve despues --
     * en un programa que dependa de la alineacion de lo suyo, algo asi cambia
     * si funciona o no.  Ademas evita el orden de inicializacion entre ficheros
     * objeto, que aqui importa porque la tabla vive en otro. */
    static const bool hecho = [] {
        register_canonical_name(kProductorEstructura);
        register_canonical_name(kProductorRangos);
        register_canonical_name(kProductorMemoria);
        register_canonical_name(kProductorFrontera);
        register_canonical_name(kProductorBucles);
        register_canonical_name(kUnidadModulo);
        return true;
    }();
    (void)hecho;
}

/// Marcadores de identidad para el gestor.  Uno por dominio: la cache va por
/// (analisis, unidad), asi que dos dominios distintos no se pisan.
namespace {
struct MemoriaAnalysis {
    static char ID;
};
struct BuclesAnalysis {
    static char ID;
};
struct FronteraAnalysis {
    static char ID;
};
char MemoriaAnalysis::ID = 0;
char BuclesAnalysis::ID = 0;
char FronteraAnalysis::ID = 0;
} // namespace

BaseDeHechos::BaseDeHechos() { register_asa_canonical_names(); }

BaseDeHechos::~BaseDeHechos() {
    static const bool loguear = std::getenv("VESTA_ASA_HECHOS_DEBUG") != nullptr;
    if (!loguear || consultas_ == 0) return;
    volcar_hechos(volcado(), stderr);
    std::fprintf(stderr,
                 "[hechos] %zu preguntas atendidas, %zu analisis ejecutados\n",
                 consultas_, computos_);
}

std::string BaseDeHechos::clave_de(const ir::IrFunction &fn) {
    if (!fn.name.empty()) return fn.name;
    /* Anonima: la direccion la identifica sin ambiguedad mientras viva, y una
     * base no sobrevive al modulo cuyas funciones consulta. */
    char buf[40];
    std::snprintf(buf, sizeof buf, "<anonima:%p>",
                  static_cast<const void *>(&fn));
    return std::string(buf);
}

void BaseDeHechos::sellar(const char *productor, const std::string &clave,
                          Certeza c, const char *apoyo) {
    Sello s;
    s.certeza = c;
    s.origen.productor = productor;
    if (apoyo != nullptr) s.apoyos.anadir(apoyo);
    auto &tabla = sellos_[productor];
    auto &guardado = tabla[clave];
    guardado = s;
    /* La procedencia apunta a la CLAVE, no al nombre de la funcion: la clave
     * vive en el mapa tanto como el sello, y el nodo no se mueve al crecer.
     * Apuntar al nombre de la funcion dejaria un puntero colgando en cuanto la
     * funcion consultada muriera antes que la base. */
    guardado.origen.funcion = tabla.find(clave)->first.c_str();
}

const IrFacts &BaseDeHechos::estructura(const ir::IrFunction &fn) {
    ++consultas_;
    const std::string clave = clave_de(fn);
    if (!gestor_.cached<IRFactsAnalysis>(clave)) {
        ++computos_;
        /* Un recorrido, sin reticulo ni punto fijo: lo que sale de aqui esta
         * DEMOSTRADO, no inferido.  Los def-use y el CFG son lo que el IR dice,
         * no una aproximacion de lo que podria pasar. */
        sellar(kProductorEstructura, clave, Certeza::Demostrada);
    }
    return gestor_.get_or_compute<IRFactsAnalysis, IrFacts>(
        clave, [&fn]() { return build_ir_facts(fn); });
}

const RangeFacts &BaseDeHechos::rangos(const ir::IrFunction &fn) {
    ++consultas_;
    const std::string clave = clave_de(fn);
    const bool recien = !gestor_.cached<RangeAnalysis>(clave);
    if (recien) ++computos_;
    /* La factoria pide la estructura POR LA BASE, no por su cuenta: asi el
     * gestor anota que los rangos dependen de ella y una invalidacion arrastra
     * a los dos.  Pedirla aparte dejaria rangos vivos sobre hechos muertos. */
    const RangeFacts &rf = gestor_.get_or_compute<RangeAnalysis, RangeFacts>(
        clave, [this, &fn]() { return compute_ranges(fn, estructura(fn)); });
    if (recien) {
        /* La certeza sale del propio analisis, no de quien pregunta: llegar a
         * punto fijo es haber visto todo lo que podia contradecirlo; pararse por
         * presupuesto es "hasta aqui he llegado", que sostiene una decision con
         * red pero no permite quitar una comprobacion. */
        sellar(kProductorRangos, clave,
               rf.convergio ? Certeza::Demostrada : Certeza::Inferida,
               kProductorEstructura);
    }
    return rf;
}

const PointsTo &BaseDeHechos::memoria(const ir::IrFunction &fn) {
    ++consultas_;
    const std::string clave = clave_de(fn);
    if (!gestor_.cached<MemoriaAnalysis>(clave)) {
        ++computos_;
        /* El conjunto de sitios a los que un puntero PUEDE referirse es una
         * sobre-aproximacion COMPLETA: nada que no este dentro puede ocurrir.
         * Que un puntero concreto quede en "cualquier cosa" no rebaja el hecho
         * -- eso lo dice la propia entrada, no su certeza. */
        sellar(kProductorMemoria, clave, Certeza::Demostrada,
               kProductorEstructura);
    }
    return gestor_.get_or_compute<MemoriaAnalysis, PointsTo>(
        clave, [this, &fn]() { return compute_points_to(fn, estructura(fn)); });
}

const LoopFacts &BaseDeHechos::bucles(const ir::IrFunction &fn) {
    ++consultas_;
    const std::string clave = clave_de(fn);
    if (!gestor_.cached<BuclesAnalysis>(clave)) {
        ++computos_;
        sellar(kProductorBucles, clave, Certeza::Demostrada);
    }
    return gestor_.get_or_compute<BuclesAnalysis, LoopFacts>(
        clave, [&fn]() { return compute_loop_facts(fn); });
}

const RangeSummaries &BaseDeHechos::frontera(const ir::IrModule &mod) {
    ++consultas_;
    const std::string clave = kUnidadModulo;
    const bool recien = !gestor_.cached<FronteraAnalysis>(clave);
    if (recien) ++computos_;
    const RangeSummaries &rs =
        gestor_.get_or_compute<FronteraAnalysis, RangeSummaries>(
            clave, [&mod]() { return compute_range_summaries(mod); });
    if (recien) {
        /* Sin punto fijo del grafo de llamadas los resumenes se abren solos, y
         * entonces lo que se sabe es nada -- no algo menos preciso. */
        sellar(kProductorFrontera, clave,
               rs.convergio ? Certeza::Demostrada : Certeza::Desconocida,
               kProductorEstructura);
    }
    return rs;
}

void BaseDeHechos::invalidar(const ir::IrFunction &fn) {
    const std::string clave = clave_de(fn);
    /* La estructura arrastra en cascada a todo lo que se derivo de ella; los
     * demas se descartan tambien de forma explicita por si alguien los pidio
     * antes de que existiera esa dependencia. */
    gestor_.invalidate<IRFactsAnalysis>(clave);
    gestor_.invalidate<RangeAnalysis>(clave);
    gestor_.invalidate<MemoriaAnalysis>(clave);
    gestor_.invalidate<BuclesAnalysis>(clave);
    /* Y su sello con ellos: un hecho muerto que deja su procedencia atras hace
     * que el volcado afirme lo que ya no se sabe. */
    for (auto &dominio : sellos_) dominio.second.erase(clave);
}

Sello BaseDeHechos::sello(const char *productor,
                          const ir::IrFunction &fn) const {
    auto d = sellos_.find(productor);
    if (d == sellos_.end()) return Sello{};
    auto it = d->second.find(clave_de(fn));
    /* Nadie ha preguntado todavia: no se sabe nada, que no es lo mismo que
     * saber que no hay nada. */
    if (it == d->second.end()) return Sello{};
    return it->second;
}

Sello BaseDeHechos::sello_de_modulo(const char *productor) const {
    auto d = sellos_.find(productor);
    if (d == sellos_.end()) return Sello{};
    auto it = d->second.find(kUnidadModulo);
    if (it == d->second.end()) return Sello{};
    return it->second;
}

std::vector<HechoRegistrado> BaseDeHechos::volcado() const {
    std::vector<HechoRegistrado> salida;
    for (const auto &dominio : sellos_)
        for (const auto &par : dominio.second)
            salida.push_back({dominio.first, par.first, par.second});
    /* Orden estable: dos volcados del mismo programa deben poder compararse. */
    std::sort(salida.begin(), salida.end(),
              [](const HechoRegistrado &a, const HechoRegistrado &b) {
                  if (a.funcion != b.funcion) return a.funcion < b.funcion;
                  return std::strcmp(a.dominio, b.dominio) < 0;
              });
    return salida;
}

void volcar_hechos(const std::vector<HechoRegistrado> &entradas, FILE *salida) {
    for (const HechoRegistrado &h : entradas) {
        std::fprintf(salida, "[hechos] %-16s %-32s certeza=%s", h.dominio,
                     h.funcion.c_str(), nombre_certeza(h.sello.certeza));
        for (int i = 0; i < Dependencias::kMax; ++i)
            if (h.sello.apoyos.de[i] != nullptr)
                std::fprintf(salida, " sobre=%s", h.sello.apoyos.de[i]);
        std::fprintf(salida, "\n");
    }
}

} // namespace asa
} // namespace analysis
