/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codec_source.cpp
 * @brief Serializacion de los nodos de la capa semantica.
 */

#include "codec_internal.h"

namespace vxdbg {

using codec_detail::expect;
using codec_detail::make;

namespace {

/// Escribe un tramo del fuente.
void put_span(ByteWriter &w, const SourceSpan &s) {
    w.id(s.file);
    w.u32(s.begin_line);
    w.u16(s.begin_column);
    w.u32(s.end_line);
    w.u16(s.end_column);
}

/// Lee un tramo del fuente.
SourceSpan get_span(ByteReader &r) {
    SourceSpan s;
    s.file = r.id<FileTag>();
    s.begin_line = r.u32();
    s.begin_column = r.u16();
    s.end_line = r.u32();
    s.end_column = r.u16();
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
//  Fichero
// ---------------------------------------------------------------------------

StoredNode encode(const FileNode &n) {
    ByteWriter w;
    w.str(n.path);
    w.hash(n.checksum);
    w.str(n.language);
    w.str(n.encoding);
    return make(n.header, w);
}

bool decode(const StoredNode &s, FileNode &out) {
    if (!expect<FileNode>(s, NodeKind::File)) return false;
    ByteReader r(s.payload);
    out.header = s.header;
    out.path = r.str();
    out.checksum = r.hash();
    out.language = r.str();
    out.encoding = r.str();
    return r.ok();
}

// ---------------------------------------------------------------------------
//  Entidad del lenguaje
// ---------------------------------------------------------------------------

StoredNode encode(const LanguageEntity &n) {
    ByteWriter w;
    w.str(n.name);
    w.str(n.qualified);
    w.str(n.kind);

    w.u32(static_cast<uint32_t>(n.relations.size()));
    for (const auto &rel : n.relations) {
        w.u8(static_cast<uint8_t>(rel.kind));
        w.id(rel.target);
        w.str(rel.lang_role);
    }

    w.u32(static_cast<uint32_t>(n.attributes.size()));
    for (const auto &a : n.attributes) {
        w.str(a.name);
        w.u8(static_cast<uint8_t>(a.kind));
        w.str(a.text);
        w.i64(a.number);
        w.id(a.reference);
    }

    put_span(w, n.declared_at);
    w.id(n.body);
    w.u32(n.byte_size);
    w.u32(n.alignment);
    return make(n.header, w);
}

bool decode(const StoredNode &s, LanguageEntity &out) {
    if (!expect<LanguageEntity>(s, NodeKind::Entity)) return false;
    ByteReader r(s.payload);
    out.header = s.header;
    out.name = r.str();
    out.qualified = r.str();
    out.kind = r.str();

    const uint32_t n_rel = r.u32();
    if (!r.ok()) return false;
    out.relations.clear();
    // Se reserva por el numero leido, que ya paso por el lector: si los bytes
    // estuvieran rotos, `ok()` seria falso y no se llegaria aqui.  Sin esto, una
    // entidad con muchas relaciones va creciendo el vector a cachos.
    out.relations.reserve(n_rel);
    for (uint32_t i = 0; i < n_rel && r.ok(); ++i) {
        Relation rel;
        rel.kind = static_cast<RelationKind>(r.u8());
        rel.target = r.id<LanguageEntityTag>();
        rel.lang_role = r.str();
        out.relations.push_back(std::move(rel));
    }

    const uint32_t n_att = r.u32();
    if (!r.ok()) return false;
    out.attributes.clear();
    out.attributes.reserve(n_att);
    for (uint32_t i = 0; i < n_att && r.ok(); ++i) {
        Attribute a;
        a.name = r.str();
        a.kind = static_cast<AttributeKind>(r.u8());
        a.text = r.str();
        a.number = r.i64();
        a.reference = r.id<LanguageEntityTag>();
        out.attributes.push_back(std::move(a));
    }

    out.declared_at = get_span(r);
    out.body = r.id<IrFunctionTag>();
    out.byte_size = r.u32();
    out.alignment = r.u32();
    return r.ok();
}

// ---------------------------------------------------------------------------
//  Ambito
// ---------------------------------------------------------------------------

StoredNode encode(const ScopeNode &n) {
    ByteWriter w;
    w.id(n.parent);
    w.id(n.owner);
    put_span(w, n.span);
    return make(n.header, w);
}

bool decode(const StoredNode &s, ScopeNode &out) {
    if (!expect<ScopeNode>(s, NodeKind::Scope)) return false;
    ByteReader r(s.payload);
    out.header = s.header;
    out.parent = r.id<ScopeTag>();
    out.owner = r.id<LanguageEntityTag>();
    out.span = get_span(r);
    return r.ok();
}

// ---------------------------------------------------------------------------
//  Variable
// ---------------------------------------------------------------------------

StoredNode encode(const VariableNode &n) {
    ByteWriter w;
    w.str(n.name);
    w.id(n.type);
    w.id(n.scope);
    w.boolean(n.is_parameter);
    put_span(w, n.declared_at);
    return make(n.header, w);
}

bool decode(const StoredNode &s, VariableNode &out) {
    if (!expect<VariableNode>(s, NodeKind::Variable)) return false;
    ByteReader r(s.payload);
    out.header = s.header;
    out.name = r.str();
    out.type = r.id<LanguageEntityTag>();
    out.scope = r.id<ScopeTag>();
    out.is_parameter = r.boolean();
    out.declared_at = get_span(r);
    return r.ok();
}

// ---------------------------------------------------------------------------
//  Sentencia
// ---------------------------------------------------------------------------

StoredNode encode(const StatementNode &n) {
    ByteWriter w;
    put_span(w, n.span);
    w.id(n.scope);
    w.str(n.lang_kind);
    return make(n.header, w);
}

bool decode(const StoredNode &s, StatementNode &out) {
    if (!expect<StatementNode>(s, NodeKind::Statement)) return false;
    ByteReader r(s.payload);
    out.header = s.header;
    out.span = get_span(r);
    out.scope = r.id<ScopeTag>();
    out.lang_kind = r.str();
    return r.ok();
}

} // namespace vxdbg
