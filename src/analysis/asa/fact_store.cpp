/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/asa/fact_store.cpp
 * @brief Implementacion del almacen de hechos (ver @c
 * analysis/asa/fact_store.h).
 */

#include "analysis/asa/fact_store.h"

namespace analysis {
namespace asa {

const FactId kSinHecho = 0xFFFFFFFFu;

const char *nombre_fuente(Fuente f) {
    switch (f) {
    case Fuente::Ejecucion: return "ejecucion";
    case Fuente::Perfil: return "perfil";
    case Fuente::Declarado: return "declarado";
    default: return "estatico";
    }
}

const char *nombre_clase_sujeto(Sujeto::Clase c) {
    switch (c) {
    case Sujeto::Clase::Modulo: return "modulo";
    case Sujeto::Clase::Funcion: return "funcion";
    case Sujeto::Clase::Valor: return "valor";
    case Sujeto::Clase::Bloque: return "bloque";
    case Sujeto::Clase::Instruccion: return "instruccion";
    default: return "simbolo";
    }
}

/* Los nombres canonicos son POCOS -- un punado de productores y dominios -- y
 * se consultan una vez por hecho leido.  Tabla asociativa, no lista: la lectura
 * de un modulo grande hace cientos de miles de consultas. */
static std::unordered_map<std::string, const char *> &tabla_canonicos() {
    static std::unordered_map<std::string, const char *> t;
    return t;
}

void register_canonical_name(const char *nombre) {
    if (nombre == nullptr || nombre[0] == '\0') return;
    /* El PRIMERO que se registra manda: si dos sitios dieran de alta el mismo
     * texto con literales distintos, cambiar de opinion a mitad partiria la
     * identidad justo de los hechos ya leidos. */
    tabla_canonicos().emplace(std::string(nombre), nombre);
}

const char *canonical_name(const std::string &s) {
    auto it = tabla_canonicos().find(s);
    return it == tabla_canonicos().end() ? nullptr : it->second;
}

const char *FactStore::internar(const std::string &s) {
    auto it = internados_.find(s);
    if (it != internados_.end()) return it->second;
    nombres_.push_back(s);
    const char *p = nombres_.back().c_str();
    internados_.emplace(s, p);
    return p;
}

FactId FactStore::anadir(Fact f) {
    const FactId id = static_cast<FactId>(hechos_.size());
    if (f.de_quien.funcion != nullptr && f.de_quien.funcion[0] != '\0')
        por_funcion_[f.de_quien.funcion].push_back(id);
    por_dominio_[f.que.dominio].push_back(id);
    hechos_.push_back(std::move(f));
    return id;
}

const std::vector<FactId> &
FactStore::de_funcion(const std::string &funcion) const {
    static const std::vector<FactId> kVacio;
    auto it = por_funcion_.find(funcion);
    return it == por_funcion_.end() ? kVacio : it->second;
}

const std::vector<FactId> &FactStore::de_dominio(const char *dominio) const {
    static const std::vector<FactId> kVacio;
    auto it = por_dominio_.find(dominio);
    return it == por_dominio_.end() ? kVacio : it->second;
}

std::vector<FactId> FactStore::explicar(FactId id) const {
    std::vector<FactId> orden;
    if (id >= hechos_.size()) return orden;
    /* Anchura y sin repetir: la derivacion es un grafo, no un arbol -- dos
     * hechos pueden apoyarse en el mismo --, y repetirlo haria crecer la
     * explicacion sin anadir nada. */
    std::vector<uint8_t> visto(hechos_.size(), 0);
    orden.push_back(id);
    visto[id] = 1;
    for (size_t i = 0; i < orden.size(); ++i) {
        const Fact &f = hechos_[orden[i]];
        for (FactId d : f.prueba.de) {
            if (d >= hechos_.size() || visto[d]) continue;
            visto[d] = 1;
            orden.push_back(d);
        }
    }
    return orden;
}

FactStore::Recuento FactStore::recuento() const {
    Recuento r;
    for (const Fact &f : hechos_) {
        switch (f.sello.certeza) {
        case Certeza::Demostrada: ++r.demostradas; break;
        case Certeza::Inferida: ++r.inferidas; break;
        default: ++r.desconocidas; break;
        }
    }
    return r;
}

} // namespace asa
} // namespace analysis
