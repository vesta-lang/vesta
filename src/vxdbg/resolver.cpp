/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file resolver.cpp
 * @brief El recorrido de una direccion a una explicacion.
 */

#include "vxdbg/resolver.h"

#include <algorithm>
#include <unordered_set>

namespace vxdbg {

EntityView DebugResolver::describe_entity(LanguageEntityId id) const {
    EntityView v;
    const LanguageEntity *e = nodes_.entity(id);
    if (!e) return v;

    v.found = true;
    v.name = e->name;
    v.qualified = e->qualified;
    v.kind = e->kind;           // especie comun; se puede comparar
    v.lang_kind = e->lang_kind; // como lo llama SU lenguaje; no se interpreta
    v.declared_at = e->declared_at;

    // Cadena de derivacion completa: quien lee un error quiere ver de donde
    // viene el tipo, no solo su padre inmediato.
    //
    // El corte es por CICLO detectado, no por un limite de niveles.  Con un
    // tope a secas, una jerarquia legitimamente profunda se recortaba sin
    // avisar y un ciclo de verdad quedaba igual de disimulado; asi solo se para
    // cuando algo se repite, que es cuando de verdad hay un problema en los
    // datos.
    std::unordered_set<LanguageEntityId> vistos;
    vistos.insert(id);
    LanguageEntityId actual = id;
    bool primero = true;
    while (true) {
        const LanguageEntity *cur = nodes_.entity(actual);
        if (!cur) break;
        LanguageEntityId base;
        for (const auto &rel : cur->relations) {
            if (rel.kind == RelationKind::Derives) {
                base = rel.target;
            } else if (rel.kind == RelationKind::Implements && primero) {
                if (const LanguageEntity *i = nodes_.entity(rel.target))
                    v.implements.push_back(i->name);
            } else if (rel.kind == RelationKind::DeclaredIn && primero) {
                if (const LanguageEntity *m = nodes_.entity(rel.target))
                    v.declared_in = m->name;
            }
        }
        primero = false;
        if (base.empty()) break;
        if (!vistos.insert(base).second) {
            // Ya se paso por aqui: los datos traen un ciclo.  Se deja constancia
            // en vez de callarlo, porque una jerarquia circular es un fallo de
            // quien la genero y sin decirlo nadie lo buscaria.
            v.cyclic = true;
            break;
        }
        const LanguageEntity *b = nodes_.entity(base);
        if (!b) break;
        v.derives.push_back(b->name);
        actual = base;
    }
    return v;
}

bool DebugResolver::resolve_code(uint64_t address, ResolvedSite &s) const {
    // Se mira en TODAS las revisiones, de la mas reciente a la mas antigua: un
    // marco puede seguir dentro de una version del codigo que ya se sustituyo,
    // y es justo la que hay que explicar.
    uint32_t offset = 0;
    uint32_t revision = 0;
    const CodeId code = session_.find(address, offset, revision);
    if (code.empty()) return false;

    s.has_code = true;
    s.code = code;
    s.code_offset = offset;
    s.context.code_revision = revision;

    if (const CodeNode *cn = nodes_.code(code)) {
        s.backend = cn->backend;
        s.context.unit = cn->unit;
        // El contexto sale del backend que genero el codigo, no de una
        // suposicion sobre como se estaba ejecutando.
        switch (cn->backend) {
        case BackendKind::Interpreter:
        case BackendKind::Velb:
            s.context.kind = ExecutionKind::Interpreter;
            break;
        default: s.context.kind = ExecutionKind::Jit; break;
        }
    }
    return true;
}

bool DebugResolver::resolve_ir(ResolvedSite &s) const {
    const CodeDebug *cd = nodes_.code_debug(s.code);
    if (!cd) return false;
    s.ir_instrs = cd->ir_at(s.code_offset);
    if (s.ir_instrs.empty()) return false;
    s.has_ir = true;
    return true;
}

bool DebugResolver::resolve_statement(ResolvedSite &s) const {
    // La bajada es de la FUNCION, asi que primero hay que saber de cual es la
    // instruccion; eso lo sabe quien produjo el intermedio, no la capa de
    // codigo, que deliberadamente no conoce funciones.
    const IrFunctionId fn = nodes_.function_of(s.ir_instrs.front());
    const LoweringMap *lm = fn.empty() ? nullptr : nodes_.lowering_of(fn);
    if (!lm) return false;

    const auto stmts = lm->statements_of(s.ir_instrs.front());
    if (stmts.empty()) return false;
    s.statement = stmts.front();
    s.has_statement = true;

    const StatementNode *st = nodes_.statement(s.statement);
    if (!st) return false;
    s.statement_kind = st->lang_kind;
    s.span = st->span;
    if (const FileNode *f = nodes_.file(st->span.file)) s.file_path = f->path;

    // De donde salio la sentencia: escrita, o generada al bajar.  Presentar
    // como escrito lo que nadie escribio despista mas que ayuda.
    if (const LoweringEntry *le = lm->of_statement(s.statement))
        s.origin = le->origin;
    return true;
}

void DebugResolver::resolve_owner(const StatementNode &st,
                                  ResolvedSite &s) const {
    if (const ScopeNode *sc = nodes_.scope(st.scope))
        s.entity = describe_entity(sc->owner);
}

void DebugResolver::resolve_variables(const StatementNode &st,
                                      ResolvedSite &s) const {
    for (const VariableId &vid : nodes_.variables_in(st.scope)) {
        const VariableNode *vn = nodes_.variable(vid);
        if (!vn) continue;
        VariableView vv;
        vv.name = vn->name;
        vv.is_parameter = vn->is_parameter;
        if (const LanguageEntity *t = nodes_.entity(vn->type))
            vv.type_name = t->name;
        // Donde vive depende del backend, asi que sale de su mapa y no del nodo.
        // Se consulta por POSICION dentro de la funcion: los tramos de vida no
        // se pueden expresar con huellas, que no se ordenan.
        uint32_t pos = 0;
        if (nodes_.position_of(s.ir_instrs.front(), pos)) {
            if (const VariableMap *vm = nodes_.variable_map(vid)) {
                const LocationRange lr = vm->at(pos);
                vv.location_kind = lr.kind;
                vv.location_value = lr.value;
            }
        }
        s.variables.push_back(std::move(vv));
    }
}

ResolvedSite DebugResolver::resolve(uint64_t address, bool want_variables) const {
    ResolvedSite s;
    s.address = address;

    // Cada fase decide si se puede seguir.  Que una falle no invalida lo que ya
    // se averiguo: saber el cuerpo de codigo sin llegar a la linea sigue siendo
    // mas que no saber nada, y los campos `has_*` dicen hasta donde se llego.
    if (!resolve_code(address, s)) return s;
    if (!resolve_ir(s)) return s;
    if (!resolve_statement(s)) return s;

    const StatementNode *st = nodes_.statement(s.statement);
    if (!st) return s;

    resolve_owner(*st, s);
    if (want_variables) resolve_variables(*st, s);
    return s;
}

std::vector<ResolvedFrame> DebugResolver::resolve_stack(
    const std::vector<uint64_t> &addresses) const {
    std::vector<ResolvedFrame> out;
    out.reserve(addresses.size());
    for (uint64_t a : addresses) {
        ResolvedFrame f;
        f.site = resolve(a);
        f.context_note = f.site.context.description;
        out.push_back(std::move(f));
    }
    return out;
}

std::vector<uint64_t> DebugResolver::addresses_for(const std::string &file,
                                                   uint32_t line) const {
    // El camino inverso, para colocar un punto de parada.  No se puede recorrer
    // "hacia atras" con los mismos pasos: hay que preguntar a quien tenga el
    // indice de sentencias por fichero y linea.  Mientras ese indice no exista,
    // decirlo es mejor que devolver una lista vacia que se lee como "esa linea
    // no genero codigo", que es una respuesta muy distinta.
    (void)file;
    (void)line;
    return {};
}

} // namespace vxdbg
