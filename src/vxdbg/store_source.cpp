/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file store_source.cpp
 * @brief Implementacion de la fuente de nodos respaldada por un almacen.
 */

#include "vxdbg/store_source.h"

#include "vxdbg/codec.h"

#include <algorithm>

namespace vxdbg {

template <typename T>
const T *StoreNodeSource::fetch(
    std::unordered_map<ContentHash, std::unique_ptr<T>> &cache,
    ContentHash hash) const {
    if (hash.empty()) return nullptr;
    auto it = cache.find(hash);
    if (it != cache.end()) return it->second.get();

    auto node = std::make_unique<T>();
    // Leer y descifrar ya lo hace `load_node`; repetirlo aqui obligaria a esta
    // clase a conocer `StoredNode`, que no le hace ninguna falta.  No se guarda
    // el fallo -- si el almacen cambia, la proxima vez podria ir bien -- pero
    // tampoco se devuelve algo a medio construir.
    if (!load_node(store_, hash, *node)) return nullptr;
    const T *raw = node.get();
    cache.emplace(hash, std::move(node));
    return raw;
}

const CodeNode *StoreNodeSource::code(CodeId id) const {
    return fetch(codes_, id.hash);
}

const CodeDebug *StoreNodeSource::code_debug(CodeId id) const {
    // La correspondencia no se localiza por la huella del codigo: es otro nodo,
    // con la suya.  Quien la produjo declara el vinculo.
    auto it = code_to_debug_.find(id);
    if (it == code_to_debug_.end()) return nullptr;
    return fetch(debugs_, it->second);
}

const LoweringMap *StoreNodeSource::lowering_of(IrFunctionId fn) const {
    auto it = fn_to_lowering_.find(fn);
    if (it == fn_to_lowering_.end()) return nullptr;
    return fetch(lowerings_, it->second);
}

const StatementNode *StoreNodeSource::statement(StatementId id) const {
    return fetch(stmts_, id.hash);
}

const LanguageEntity *StoreNodeSource::entity(LanguageEntityId id) const {
    return fetch(entities_, id.hash);
}

const ScopeNode *StoreNodeSource::scope(ScopeId id) const {
    return fetch(scopes_, id.hash);
}

const VariableNode *StoreNodeSource::variable(VariableId id) const {
    return fetch(vars_, id.hash);
}

const VariableMap *StoreNodeSource::variable_map(VariableId id) const {
    // El mapa de una variable es OTRO nodo, con su propia huella: buscarlo por
    // la de la variable no lo encontraria nunca.  Y ademas hay uno por cada
    // forma de compilar, asi que el vinculo lo declara quien sepa de cual se
    // trata.  Es el mismo caso que la correspondencia del codigo.
    auto it = var_to_map_.find(id);
    if (it == var_to_map_.end()) return nullptr;
    return fetch(varmaps_, it->second);
}

const FileNode *StoreNodeSource::file(FileId id) const {
    return fetch(files_, id.hash);
}

const CompilationUnit *StoreNodeSource::unit(UnitId) const {
    // La unidad de compilacion NO es un nodo del grafo -- no se hashea ni se
    // guarda -- asi que el almacen no la tiene.  Quien la necesite la lleva por
    // su cuenta; devolver algo aqui seria inventarselo.
    return nullptr;
}

IrFunctionId StoreNodeSource::function_of(IrInstrId instr) const {
    auto it = instr_to_fn_.find(instr);
    if (it == instr_to_fn_.end()) return IrFunctionId{};
    return it->second;
}

std::vector<VariableId> StoreNodeSource::variables_in(ScopeId scope) const {
    auto it = scope_vars_.find(scope);
    if (it == scope_vars_.end()) return {};
    return it->second;
}

bool StoreNodeSource::position_of(IrInstrId instr,
                                  uint32_t &out_position) const {
    auto it = instr_to_pos_.find(instr);
    if (it == instr_to_pos_.end()) return false;
    out_position = it->second;
    return true;
}

void StoreNodeSource::note_function_of(IrInstrId instr, IrFunctionId fn) {
    instr_to_fn_[instr] = fn;
}

void StoreNodeSource::note_position_of(IrInstrId instr, uint32_t position) {
    instr_to_pos_[instr] = position;
}

void StoreNodeSource::note_lowering(IrFunctionId fn, ContentHash map) {
    fn_to_lowering_[fn] = map;
}

void StoreNodeSource::note_variable_in(ScopeId scope, VariableId var) {
    auto &v = scope_vars_[scope];
    // Sin repetidos: un ambito CONTIENE una variable o no la contiene, no la
    // contiene dos veces.  Dejarlo a criterio de quien llame significaba que
    // declararlo dos veces -- algo facil al recorrer un arbol por dos caminos
    // -- ensenaria la misma variable duplicada al explicar un fallo.
    if (std::find(v.begin(), v.end(), var) != v.end()) return;
    v.push_back(var);
}

void StoreNodeSource::note_code_debug(CodeId code, ContentHash debug) {
    code_to_debug_[code] = debug;
}

void StoreNodeSource::note_variable_map(VariableId var, ContentHash map) {
    var_to_map_[var] = map;
}

} // namespace vxdbg
