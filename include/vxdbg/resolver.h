/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file resolver.h
 * @brief De una direccion a una explicacion, atravesando todas las capas.
 *
 * Las capas estan separadas para que cada una se pueda cambiar sola, pero quien
 * depura no quiere recorrerlas a mano.  El recorrido siempre es el mismo:
 *
 *     direccion -> colocacion -> codigo -> intermedio -> bajada -> sentencia
 *               -> ambito -> variables -> tipos -> entidad -> fuente
 *
 * Es tan largo y tan constante que merece una sola llamada.  Aqui vive.
 *
 * **Devuelve datos, nunca texto.**  Quien pregunta decide si lo imprime, lo
 * manda al depurador o lo vuelca a un formato de maquina, y en que idioma.  Esa
 * separacion es la que permite contar el mismo fallo de varias maneras sin
 * duplicar la logica de averiguarlo, y la que deja el subsistema utilizable
 * desde un frontend que no sea el nuestro.
 *
 * El resolutor no conoce ningun lenguaje: transporta el @c kind que cada
 * frontend puso en sus entidades sin interpretarlo.
 */

#ifndef VXDBG_RESOLVER_H
#define VXDBG_RESOLVER_H

#include "vxdbg/backend_meta.h"
#include "vxdbg/compilation_unit.h"
#include "vxdbg/ids.h"
#include "vxdbg/lowering_map.h"
#include "vxdbg/source_meta.h"

#include <memory>
#include <string>
#include <vector>

namespace vxdbg {

/**
 * @brief Una entidad tal como se cuenta en un diagnostico.
 *
 * Trae la jerarquia ya resuelta: quien lee un error quiere ver "deriva de
 * Flujo, cumple Cerrable" y no una lista de identificadores que tendria que
 * seguir el mismo.
 */
struct EntityView {
    bool found = false;
    std::string name;
    std::string qualified;
    std::string kind; ///< como lo llama SU lenguaje
    std::vector<std::string> derives;    ///< cadena completa, de cerca a lejos
    std::vector<std::string> implements;
    std::string declared_in;             ///< modulo o espacio de nombres
    SourceSpan declared_at;
    /// La cadena de derivacion se cerraba sobre si misma.  Es un fallo de quien
    /// genero los datos, y callarlo dejaria una jerarquia recortada sin que
    /// nadie supiera por que.
    bool cyclic = false;
};

/**
 * @brief Una variable con su valor localizado en un punto concreto.
 */
struct VariableView {
    std::string name;
    std::string type_name;
    bool is_parameter = false;
    LocationKind location_kind = LocationKind::OptimizedOut;
    int64_t location_value = 0;
};

/**
 * @brief Todo lo que se sabe de una direccion, en los tres niveles.
 *
 * Cada nivel dice por su cuenta si tiene respuesta: hay codigo generado que no
 * vino del intermedio (un prologo), instrucciones que el optimizador dejo sin
 * correspondencia exacta con el fuente, y sentencias que no produjeron codigo
 * ninguno.  Callar esa diferencia haria pasar por "no se sabe" lo que en
 * realidad es "no hay", que son cosas muy distintas para quien busca un fallo.
 */
struct ResolvedSite {
    /// Donde estamos, tal cual.
    uint64_t address = 0;

    /// Contexto: interpretado, compilado al vuelo, ejecutado por el compilador.
    ExecutionContext context;

    /// Nivel de codigo generado.
    bool has_code = false;
    CodeId code;
    uint32_t code_offset = 0;
    BackendKind backend = BackendKind::Velb;

    /// Nivel intermedio.
    bool has_ir = false;
    std::vector<IrInstrId> ir_instrs;

    /// Nivel de fuente.
    bool has_statement = false;
    StatementId statement;
    std::string statement_kind; ///< como lo llama su lenguaje
    OriginKind origin = OriginKind::Written;
    SourceSpan span;
    std::string file_path;

    /// A quien pertenece el codigo, con su jerarquia.
    EntityView entity;

    /// Variables con valor aqui.
    std::vector<VariableView> variables;
};

/**
 * @brief Un tramo de la pila, ya explicado.
 */
struct ResolvedFrame {
    ResolvedSite site;
    /// Descripcion del contexto para quien lo lea ("evaluando constante",
    /// "instanciando plantilla").  Permite contar en UNA traza que la
    /// instanciacion de una plantilla llamo a algo que se evaluo al compilar y
    /// que de ahi salio el fallo, sin cambiar de herramienta.
    std::string context_note;
};

/**
 * @brief De donde saca el resolutor los nodos.
 *
 * Se declara como interfaz para no atarlo a un almacen concreto: hoy los nodos
 * vienen de ficheros cacheados por contenido, manana pueden venir de memoria en
 * una sesion interactiva o de una base de datos compartida por un equipo.  El
 * resolutor no deberia enterarse.
 */
class NodeSource {
  public:
    virtual ~NodeSource() = default;

    virtual const CodeNode *code(CodeId id) const = 0;
    virtual const CodeDebug *code_debug(CodeId id) const = 0;
    virtual const LoweringMap *lowering_of(IrFunctionId fn) const = 0;
    virtual const StatementNode *statement(StatementId id) const = 0;
    virtual const LanguageEntity *entity(LanguageEntityId id) const = 0;
    virtual const ScopeNode *scope(ScopeId id) const = 0;
    virtual const VariableNode *variable(VariableId id) const = 0;
    virtual const VariableMap *variable_map(VariableId id) const = 0;
    virtual const FileNode *file(FileId id) const = 0;
    virtual const CompilationUnit *unit(UnitId id) const = 0;

    /// Instrucciones intermedias de una funcion, para relacionar sentencias.
    virtual IrFunctionId function_of(IrInstrId instr) const = 0;

    /**
     * @brief Numero de orden de una instruccion dentro de su funcion.
     *
     * Hace falta para consultar los tramos de vida de las variables, que se
     * expresan por posicion y no por identificador: una huella no se ordena, y
     * un rango con extremos que no se ordenan no se puede consultar.
     *
     * Lo sabe quien produjo el intermedio; la capa de codigo, que solo conoce
     * instrucciones sueltas, no tiene por que.
     *
     * @param instr Instruccion.
     * @param out_position Recibe su posicion.
     * @return @c true si consta.
     */
    virtual bool position_of(IrInstrId instr, uint32_t &out_position) const = 0;

    /// Variables declaradas en un ambito.  Es una relacion inversa: el nodo de
    /// ambito no las lleva, se indexan aparte.
    virtual std::vector<VariableId> variables_in(ScopeId scope) const = 0;
};

/**
 * @brief Traduce direcciones a explicaciones.
 *
 * Es lo unico que necesita conocer quien depura: le da una direccion y recibe
 * el recorrido entero.  Las capas de debajo pueden reorganizarse sin que se
 * entere.
 */
class DebugResolver {
  public:
    /**
     * @param nodes De donde salen los nodos.
     * @param session Colocacion del codigo en la ejecucion en curso.
     */
    DebugResolver(const NodeSource &nodes, const SessionMap &session)
        : nodes_(nodes), session_(session) {}

    /**
     * @brief Explica una direccion.
     * @param address Direccion de ejecucion.
     * @param want_variables Si buscar tambien las variables vivas.
     * @return Lo que se pudo averiguar; los campos @c has_* dicen que si.
     */
    ResolvedSite resolve(uint64_t address, bool want_variables = true) const;

    /**
     * @brief Explica una pila entera.
     * @param addresses Direcciones, de la mas reciente a la mas antigua.
     * @return Un tramo explicado por cada direccion.
     */
    std::vector<ResolvedFrame> resolve_stack(
        const std::vector<uint64_t> &addresses) const;

    /**
     * @brief El camino inverso: de una posicion del fuente a las direcciones.
     *
     * Es lo que hace falta para poner un punto de parada.  Devuelve TODAS,
     * porque la misma sentencia puede estar compilada de varias maneras a la
     * vez -- interpretada y al vuelo -- y parar solo en una dejaria pasar la
     * ejecucion por la otra.
     *
     * @param file Fichero.
     * @param line Linea.
     * @return Las direcciones donde quedo esa linea.
     */
    std::vector<uint64_t> addresses_for(const std::string &file,
                                        uint32_t line) const;

    /**
     * @brief Describe una entidad con su jerarquia ya resuelta.
     *
     * Publico porque hace falta por si solo: un diagnostico que cita un tipo
     * quiere contarlo entero -- de quien deriva, que cumple -- sin tener que
     * partir de una direccion.
     *
     * @param id Entidad.
     * @return La vista; @c found es @c false si no esta.
     */
    EntityView describe_entity(LanguageEntityId id) const;

  private:
    /* El recorrido va por fases, una por capa que atraviesa, y cada una decide
     * si se puede seguir.  Escrito de corrido cabia, pero el camino tiene cinco
     * saltos y no para de crecer: partido, cada salto se lee y se prueba solo. */

    /// Fase 1: de la direccion al cuerpo de codigo.  @return si se pudo.
    bool resolve_code(uint64_t address, ResolvedSite &s) const;

    /// Fase 2: del codigo a las instrucciones intermedias.  @return si se pudo.
    bool resolve_ir(ResolvedSite &s) const;

    /// Fase 3: del intermedio a la sentencia del fuente.  @return si se pudo.
    bool resolve_statement(ResolvedSite &s) const;

    /// Fase 4: a quien pertenece el codigo, con su jerarquia.
    void resolve_owner(const StatementNode &st, ResolvedSite &s) const;

    /// Fase 5: las variables con valor en ese punto.
    void resolve_variables(const StatementNode &st, ResolvedSite &s) const;

    const NodeSource &nodes_;
    const SessionMap &session_;
};

} // namespace vxdbg

#endif // VXDBG_RESOLVER_H
