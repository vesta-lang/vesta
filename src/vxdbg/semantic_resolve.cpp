/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file semantic_resolve.cpp
 * @brief De un grafo con claves a uno con identidades, y de ahi al almacen.
 *
 * Todo el trabajo esta en el orden.  Una entidad se identifica por su
 * contenido, y su contenido incluye las identidades de aquellas con las que se
 * relaciona: para poder decir que `Lector` deriva de `Flujo` hay que haber
 * resuelto `Flujo` primero.  De ahi el recorrido en profundidad, con memoria de
 * lo ya hecho para que una jerarquia en rombo no resuelva la base cuatro veces.
 *
 * Resolver no guarda nada.  Y como se calcula la identidad tampoco se decide
 * aqui: se pide.
 */

#include "vxdbg/semantic.h"

#include "vxdbg/codec.h"

#include <unordered_map>
#include <unordered_set>

namespace vxdbg {

namespace {

/**
 * @brief Estado de una resolucion.
 */
class Resolver {
  public:
    /**
     * @param nodes Los nodos por resolver.
     * @param identity Como calcular una identidad.
     * @param report Donde dejar constancia.
     */
    Resolver(const std::vector<SemanticNode> &nodes, IdentityFn identity,
             GraphReport &report)
        : identity_(std::move(identity)), report_(report) {
        by_key_.reserve(nodes.size());
        for (const auto &n : nodes)
            if (!by_key_.emplace(n.key, &n).second) ++report_.duplicates;
        out_.reserve(nodes.size());
    }

    /**
     * @brief Resuelve todos, en el orden que haga falta.
     * @param nodes Los mismos que se le dieron al construirlo.
     * @return Los resueltos, en orden de dependencia.
     */
    std::vector<ResolvedNode> run(const std::vector<SemanticNode> &nodes) {
        for (const auto &n : nodes)
            resolve(n.key);
        return std::move(out_);
    }

  private:
    /**
     * @brief Identidad de un nodo por su clave, resolviendolo si hace falta.
     * @param key Clave.
     * @return Su identificador, o uno vacio si no esta o hay un ciclo.
     */
    LanguageEntityId resolve(const std::string &key) {
        if (key.empty()) return {};
        if (auto it = done_.find(key); it != done_.end()) return it->second;
        auto node = by_key_.find(key);
        if (node == by_key_.end()) return {}; // nadie lo declaro
        // Un ciclo no se puede representar: cada identidad dependeria de la
        // otra.  Se corta devolviendo vacio; quien preguntaba omitira la
        // arista.
        if (!in_progress_.insert(key).second) return {};

        const SemanticNode &n = *node->second;
        ResolvedEntity e;
        // Todo lo que no son las aristas se copia tal cual, porque es
        // literalmente la misma descripcion.
        e.name = n.name;
        e.key = n.key;
        e.kind = n.kind;
        e.lang_kind = n.lang_kind;
        e.attributes = n.attributes;
        e.declared_at = n.declared_at;
        e.body = n.body;
        e.byte_size = n.byte_size;
        e.alignment = n.alignment;
        e.relations.reserve(n.relations.size());
        for (const auto &r : n.relations) {
            const auto target = resolve(r.target);
            if (target.hash.empty()) {
                ++report_.unresolved;
                continue;
            }
            Relation out;
            out.kind = r.kind;
            out.target = target;
            out.lang_role = r.lang_role;
            e.relations.push_back(std::move(out));
        }
        in_progress_.erase(key);

        // Se serializa UNA vez: de estos bytes sale la identidad y estos mismos
        // van luego al almacen.  El resolutor no sabe como se calcula la
        // huella, solo que se calcula de aqui.
        StoredNode encoded = encode(e);
        const LanguageEntityId id{identity_(encoded)};
        encoded.header.hash = id.hash;
        done_.emplace(key, id);
        ++report_.resolved;
        // Se apila DESPUES de sus dependencias: quien lo reciba puede guardarlo
        // en este orden sabiendo que nada apunta hacia adelante.
        out_.push_back(ResolvedNode{std::move(e), id, std::move(encoded)});
        return id;
    }

    IdentityFn identity_;
    GraphReport &report_;
    std::unordered_map<std::string, const SemanticNode *> by_key_;
    std::unordered_map<std::string, LanguageEntityId> done_;
    std::unordered_set<std::string> in_progress_;
    std::vector<ResolvedNode> out_;
};

} // namespace

IdentityFn default_identity() {
    // La identidad ES la serializacion: se sella exactamente lo que se
    // guardaria, asi que anadir un campo la cambia sin que haya que acordarse
    // de nada en ningun otro sitio.
    return [](const StoredNode &n) {
        StoredNode copia =
            n; // sellar escribe la cabecera; la de fuera no se toca
        return seal(copia);
    };
}

std::vector<ResolvedNode> resolve_graph(const std::vector<SemanticNode> &nodes,
                                        GraphReport &report,
                                        IdentityFn identity) {
    if (!identity) identity = default_identity();
    Resolver r(nodes, std::move(identity), report);
    return r.run(nodes);
}

void emit_resolved(NodeStore &store, const std::vector<ResolvedNode> &resolved,
                   GraphReport &report) {
    report.ids.reserve(report.ids.size() + resolved.size());
    for (const auto &r : resolved) {
        // Los bytes ya estan: se calcularon al resolver, que es cuando hubo que
        // serializar para saber la identidad.  Aqui solo se guardan.
        if (!store.put(r.encoded)) continue;
        ++report.emitted;
        report.ids.emplace_back(r.entity.key, r.id);
    }
}

GraphReport emit_semantic_graph(NodeStore &store,
                                const std::vector<SemanticNode> &nodes) {
    GraphReport report;
    const auto resolved = resolve_graph(nodes, report);
    emit_resolved(store, resolved, report);
    return report;
}

} // namespace vxdbg
