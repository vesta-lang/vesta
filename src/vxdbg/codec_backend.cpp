/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codec_backend.cpp
 * @brief Serializacion de la bajada, del codigo generado y de las
 *        transferencias de control.
 */

#include "codec_internal.h"

namespace vxdbg {

using codec_detail::expect;
using codec_detail::make;

// ---------------------------------------------------------------------------
//  Bajada: sentencia -> intermedio
// ---------------------------------------------------------------------------

StoredNode encode(const LoweringMap &n) {
    ByteWriter w;
    w.u32(static_cast<uint32_t>(n.entries.size()));
    for (const auto &e : n.entries) {
        w.id(e.statement);
        w.u8(static_cast<uint8_t>(e.kind));
        w.u8(static_cast<uint8_t>(e.origin));
        w.id(e.inlined_into);
        w.u32(static_cast<uint32_t>(e.ir_instrs.size()));
        for (const auto &i : e.ir_instrs) w.id(i);
    }
    return make(n.header, w);
}

bool decode(const StoredNode &s, LoweringMap &out) {
    if (!expect<LoweringMap>(s, NodeKind::Lowering)) return false;
    ByteReader r(s.payload);
    const uint32_t n_e = r.u32();
    if (!r.ok()) return false;
    out.entries.clear();
    out.entries.reserve(n_e);
    for (uint32_t i = 0; i < n_e && r.ok(); ++i) {
        LoweringEntry e;
        e.statement = r.id<StatementTag>();
        e.kind = static_cast<LoweringKind>(r.u8());
        e.origin = static_cast<OriginKind>(r.u8());
        e.inlined_into = r.id<IrFunctionTag>();
        const uint32_t n_i = r.u32();
        if (!r.ok()) return false;
        for (uint32_t k = 0; k < n_i && r.ok(); ++k)
            e.ir_instrs.push_back(r.id<IrInstrTag>());
        out.entries.push_back(std::move(e));
    }
    if (!r.ok()) return false;
    // El indice inverso NO se guarda: es cache derivada.  Se reconstruye al
    // leer, que cuesta lo mismo que haberlo leido y evita que pueda llegar
    // desincronizado con las entradas.
    out.build_index();
    return true;
}

// ---------------------------------------------------------------------------
//  Codigo generado
// ---------------------------------------------------------------------------

StoredNode encode(const CodeNode &n) {
    ByteWriter w;
    w.id(n.ir_function);
    w.u16(static_cast<uint16_t>(n.backend));
    w.str(n.backend_name);
    w.str(n.optimization);
    w.u32(n.byte_size);
    w.hash(n.semantic_hash);
    w.hash(n.backend_hash);
    w.id(n.unit);
    w.u32(static_cast<uint32_t>(n.dependencies.size()));
    for (const auto &d : n.dependencies) w.hash(d);
    return make(n.header, w);
}

bool decode(const StoredNode &s, CodeNode &out) {
    if (!expect<CodeNode>(s, NodeKind::Code)) return false;
    ByteReader r(s.payload);
    out.header = s.header;
    out.ir_function = r.id<IrFunctionTag>();
    out.backend = static_cast<BackendKind>(r.u16());
    out.backend_name = r.str();
    out.optimization = r.str();
    out.byte_size = r.u32();
    out.semantic_hash = r.hash();
    out.backend_hash = r.hash();
    out.unit = r.id<UnitTag>();
    const uint32_t n_d = r.u32();
    if (!r.ok()) return false;
    out.dependencies.clear();
    for (uint32_t i = 0; i < n_d && r.ok(); ++i)
        out.dependencies.push_back(r.hash());
    return r.ok();
}

StoredNode encode(const CodeDebug &n) {
    ByteWriter w;
    w.id(n.code);
    w.u32(static_cast<uint32_t>(n.ranges.size()));
    for (const auto &rg : n.ranges) {
        w.u32(rg.begin);
        w.u32(rg.end);
        w.u8(static_cast<uint8_t>(rg.kind));
        w.u32(static_cast<uint32_t>(rg.ir_instrs.size()));
        for (const auto &i : rg.ir_instrs) w.id(i);
    }
    return make(n.header, w);
}

bool decode(const StoredNode &s, CodeDebug &out) {
    if (!expect<CodeDebug>(s, NodeKind::CodeDebug)) return false;
    ByteReader r(s.payload);
    out.header = s.header;
    out.code = r.id<CodeTag>();
    const uint32_t n_r = r.u32();
    if (!r.ok()) return false;
    out.ranges.clear();
    out.ranges.reserve(n_r);
    for (uint32_t i = 0; i < n_r && r.ok(); ++i) {
        CodeRange rg;
        rg.begin = r.u32();
        rg.end = r.u32();
        rg.kind = static_cast<RangeKind>(r.u8());
        const uint32_t n_i = r.u32();
        if (!r.ok()) return false;
        for (uint32_t k = 0; k < n_i && r.ok(); ++k)
            rg.ir_instrs.push_back(r.id<IrInstrTag>());
        out.ranges.push_back(std::move(rg));
    }
    return r.ok();
}

StoredNode encode(const VariableMap &n) {
    ByteWriter w;
    w.id(n.variable);
    w.u32(static_cast<uint32_t>(n.locations.size()));
    for (const auto &l : n.locations) {
        w.id(l.from);
        w.id(l.to);
        w.u8(static_cast<uint8_t>(l.kind));
        w.i64(l.value);
    }
    return make(n.header, w);
}

bool decode(const StoredNode &s, VariableMap &out) {
    if (!expect<VariableMap>(s, NodeKind::VariableMap)) return false;
    ByteReader r(s.payload);
    out.header = s.header;
    out.variable = r.id<VariableTag>();
    const uint32_t n_l = r.u32();
    if (!r.ok()) return false;
    out.locations.clear();
    out.locations.reserve(n_l);
    for (uint32_t i = 0; i < n_l && r.ok(); ++i) {
        LocationRange l;
        l.from = r.id<IrInstrTag>();
        l.to = r.id<IrInstrTag>();
        l.kind = static_cast<LocationKind>(r.u8());
        l.value = r.i64();
        out.locations.push_back(l);
    }
    return r.ok();
}

// ---------------------------------------------------------------------------
//  Transferencias de control
// ---------------------------------------------------------------------------

StoredNode encode(const ExecutionEdge &n) {
    ByteWriter w;
    w.id(n.source);
    w.id(n.from);
    w.u8(static_cast<uint8_t>(n.to_kind));
    w.id(n.to);
    w.str(n.to_name);
    w.u8(static_cast<uint8_t>(n.kind));
    w.u8(static_cast<uint8_t>(n.dispatch));
    w.u8(static_cast<uint8_t>(n.form));
    w.u32(static_cast<uint32_t>(n.statements.size()));
    for (const auto &st : n.statements) w.id(st);
    return make(n.header, w);
}

bool decode(const StoredNode &s, ExecutionEdge &out) {
    if (!expect<ExecutionEdge>(s, NodeKind::ExecutionEdge)) return false;
    ByteReader r(s.payload);
    out.header = s.header;
    out.source = r.id<IrInstrTag>();
    out.from = r.id<IrFunctionTag>();
    out.to_kind = static_cast<EndpointKind>(r.u8());
    out.to = r.id<IrFunctionTag>();
    out.to_name = r.str();
    out.kind = static_cast<EdgeKind>(r.u8());
    out.dispatch = static_cast<DispatchKind>(r.u8());
    out.form = static_cast<TransferForm>(r.u8());
    const uint32_t n_s = r.u32();
    if (!r.ok()) return false;
    out.statements.clear();
    for (uint32_t i = 0; i < n_s && r.ok(); ++i)
        out.statements.push_back(r.id<StatementTag>());
    return r.ok();
}

StoredNode encode(const InlineSite &n) {
    ByteWriter w;
    w.id(n.at);
    w.id(n.inlined_function);
    w.id(n.edge);
    w.id(n.parent);
    return make(n.header, w);
}

bool decode(const StoredNode &s, InlineSite &out) {
    if (!expect<InlineSite>(s, NodeKind::InlineSite)) return false;
    ByteReader r(s.payload);
    out.header = s.header;
    out.at = r.id<IrInstrTag>();
    out.inlined_function = r.id<IrFunctionTag>();
    out.edge = r.id<EdgeTag>();
    out.parent = r.id<InlineSiteTag>();
    return r.ok();
}

} // namespace vxdbg
