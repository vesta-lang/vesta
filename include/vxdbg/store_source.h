/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file store_source.h
 * @brief Sirve nodos desde un almacen, guardando los que ya se leyeron.
 *
 * El resolutor pide nodos por referencia y muchas veces los mismos: al explicar
 * una pila, el tipo que sale en un marco suele salir en varios.  Volver al
 * almacen cada vez significaria releer y deserializar lo mismo una y otra vez.
 *
 * Guardarlos es seguro precisamente porque son INMUTABLES: un nodo no cambia
 * nunca -- si cambiara, seria otro nodo con otra huella -- asi que no hay nada
 * que invalidar.  Esa propiedad, que se eligio por el cache incremental, sale
 * gratis aqui.
 *
 * Las relaciones inversas (que variables hay en un ambito, de que funcion es
 * una instruccion) tampoco se guardan en los nodos: se construyen aqui segun se
 * necesitan, como el resto de indices del subsistema.
 */

#ifndef VXDBG_STORE_SOURCE_H
#define VXDBG_STORE_SOURCE_H

#include "vxdbg/resolver.h"
#include "vxdbg/store.h"

#include <memory>
#include <unordered_map>

namespace vxdbg {

/**
 * @brief Fuente de nodos respaldada por un almacen.
 */
class StoreNodeSource : public NodeSource {
  public:
    /**
     * @param store De donde salen los nodos.
     */
    explicit StoreNodeSource(const NodeStore &store) : store_(store) {}

    const CodeNode *code(CodeId id) const override;
    const CodeDebug *code_debug(CodeId id) const override;
    const LoweringMap *lowering_of(IrFunctionId fn) const override;
    const StatementNode *statement(StatementId id) const override;
    const LanguageEntity *entity(LanguageEntityId id) const override;
    const ScopeNode *scope(ScopeId id) const override;
    const VariableNode *variable(VariableId id) const override;
    const VariableMap *variable_map(VariableId id) const override;
    const FileNode *file(FileId id) const override;
    const CompilationUnit *unit(UnitId id) const override;
    IrFunctionId function_of(IrInstrId instr) const override;
    bool position_of(IrInstrId instr, uint32_t &out_position) const override;
    std::vector<VariableId> variables_in(ScopeId scope) const override;

    /**
     * @brief Declara a que funcion pertenece una instruccion intermedia.
     *
     * Lo sabe quien produjo el intermedio, no el almacen: la capa de codigo
     * solo conoce instrucciones, y preguntarle por funciones seria pedirle algo
     * que deliberadamente no sabe.
     *
     * @param instr Instruccion.
     * @param fn Funcion a la que pertenece.
     */
    void note_function_of(IrInstrId instr, IrFunctionId fn);

    /**
     * @brief Declara en que posicion va una instruccion dentro de su funcion.
     *
     * Es lo que permite consultar los tramos de vida de las variables: un
     * identificador es una huella y las huellas no se ordenan, asi que el rango
     * se expresa por posicion.
     *
     * @param instr Instruccion.
     * @param position Su numero de orden.
     */
    void note_position_of(IrInstrId instr, uint32_t position);

    /**
     * @brief Declara la bajada de una funcion intermedia.
     * @param fn Funcion.
     * @param map Huella del mapa de bajada.
     */
    void note_lowering(IrFunctionId fn, ContentHash map);

    /**
     * @brief Declara que un ambito contiene una variable.
     * @param scope Ambito.
     * @param var Variable.
     */
    void note_variable_in(ScopeId scope, VariableId var);

    /**
     * @brief Declara la correspondencia de un cuerpo de codigo.
     * @param code Cuerpo.
     * @param debug Huella de su correspondencia.
     */
    void note_code_debug(CodeId code, ContentHash debug);

    /**
     * @brief Declara donde vive una variable segun un backend concreto.
     *
     * El mapa es otro nodo con su propia huella, y hay uno por cada forma de
     * compilar la misma variable: el vinculo lo declara quien sepa de cual se
     * trata.
     *
     * @param var Variable.
     * @param map Huella de su mapa de ubicaciones.
     */
    void note_variable_map(VariableId var, ContentHash map);

  private:
    /**
     * @brief Lee un nodo del almacen y lo guarda, o devuelve el ya guardado.
     * @tparam T Genero del nodo.
     * @param cache Donde se guardan los de ese genero.
     * @param hash Huella.
     * @return El nodo, o @c nullptr si no esta o no se pudo interpretar.
     */
    template <typename T>
    const T *fetch(std::unordered_map<ContentHash, std::unique_ptr<T>> &cache,
                   ContentHash hash) const;

    const NodeStore &store_;

    // Mutables porque el guardado es transparente: leer dos veces el mismo nodo
    // da lo mismo, y quien pregunta no tiene por que saber si se releyo.
    mutable std::unordered_map<ContentHash, std::unique_ptr<CodeNode>> codes_;
    mutable std::unordered_map<ContentHash, std::unique_ptr<CodeDebug>> debugs_;
    mutable std::unordered_map<ContentHash, std::unique_ptr<LoweringMap>>
        lowerings_;
    mutable std::unordered_map<ContentHash, std::unique_ptr<StatementNode>>
        stmts_;
    mutable std::unordered_map<ContentHash, std::unique_ptr<LanguageEntity>>
        entities_;
    mutable std::unordered_map<ContentHash, std::unique_ptr<ScopeNode>> scopes_;
    mutable std::unordered_map<ContentHash, std::unique_ptr<VariableNode>>
        vars_;
    mutable std::unordered_map<ContentHash, std::unique_ptr<VariableMap>>
        varmaps_;
    mutable std::unordered_map<ContentHash, std::unique_ptr<FileNode>> files_;

    // Relaciones que no viven en los nodos: las declara quien las conoce.
    std::unordered_map<IrInstrId, IrFunctionId> instr_to_fn_;
    std::unordered_map<IrInstrId, uint32_t> instr_to_pos_;
    std::unordered_map<IrFunctionId, ContentHash> fn_to_lowering_;
    std::unordered_map<CodeId, ContentHash> code_to_debug_;
    std::unordered_map<VariableId, ContentHash> var_to_map_;
    std::unordered_map<ScopeId, std::vector<VariableId>> scope_vars_;
};

} // namespace vxdbg

#endif // VXDBG_STORE_SOURCE_H
