/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file node.h
 * @brief Encabezado comun a todos los nodos y referencia generica a cualquiera.
 *
 * Todo lo que se guarda en el almacen empieza igual: que genero es, cual es su
 * huella y con que version del esquema se escribio.  Con eso, el almacen, la
 * serializacion, el cache y la navegacion funcionan sin conocer ni uno solo de
 * los tipos concretos.
 *
 * **Sobre el genero.**  Los identificadores siguen siendo tipados
 * (@ref CodeId, @ref VariableId ...) y cruzarlos sigue sin compilar: esa
 * proteccion no se pierde.  El genero reaparece aqui para lo contrario -- para
 * poder referirse a un nodo CUALQUIERA sin saber cual es --, que es justo lo que
 * necesita un diagnostico para citar lo que le convenga, o el almacen para
 * volcar lo que le den.  Son dos necesidades opuestas y cada una tiene su
 * mecanismo: el tipo protege en la interfaz, el genero permite lo generico.
 */

#ifndef VXDBG_NODE_H
#define VXDBG_NODE_H

#include "vxdbg/ids.h"

#include <cstdint>

namespace vxdbg {

/**
 * @brief Genero de un nodo, para poder tratarlos sin distinguirlos.
 *
 * Va DENTRO del nodo y no se deduce de donde estaba guardado: asi un objeto
 * suelto sigue siendo interpretable fuera de su sitio, y reorganizar el almacen
 * no cambia el significado de nada.
 */
enum class NodeKind : uint16_t {
    Unknown = 0,
    Module = 1,
    Entity = 2,     ///< lo que un frontend declare (ver source_meta.h)
    Scope = 3,
    Variable = 4,
    Statement = 5,
    File = 6,
    Lowering = 7,   ///< correspondencia sentencia <-> intermedio
    IrFunction = 8,
    Block = 9,
    IrInstr = 10,
    Code = 11,        ///< un cuerpo de codigo generado
    CodeDebug = 12,   ///< su correspondencia con el intermedio
    VariableMap = 13, ///< donde vive una variable, por backend
    ExecutionEdge = 14, ///< una transferencia de control
    InlineSite = 15,  ///< un marco que el optimizador se llevo
    Diagnostic = 16,  ///< la explicacion de un fallo
    RuntimeObject = 17, ///< un valor del runtime citado por un diagnostico
    /// Por donde se ENTRA al grafo: que simbolos de un artefacto corresponden
    /// a que entidades.  Un almacen direccionado por contenido no se puede
    /// recorrer, y quien depura nunca empieza preguntando por una huella:
    /// empieza por una direccion, que resuelve a un simbolo.  Este nodo lo
    /// convierte en una entidad y abre el grafo entero.
    ArtifactMap = 18,
    /// Los tramos de fuente de un artefacto, por simbolo y linea.  Con la
    /// linea sola no se distingue cual de las tres llamadas que caben en ella
    /// fallo; con la columna y la longitud, si.
    SpanMap = 19,
};

/**
 * @brief Lo que comparten todos los nodos, para el ALMACEN.
 *
 * Quien escribe codigo del compilador nunca lo toca: usa identificadores
 * tipados y el compilador le impide mezclarlos.  Esto es para la otra capa,
 * la que guarda y recorre sin saber que tiene delante.
 *
 * Se pone por composicion y no por herencia: los nodos son datos, y darles una
 * tabla virtual los convertiria en objetos con identidad propia, que es
 * exactamente lo contrario de lo que son.
 */
struct DebugNodeHeader {
    NodeKind kind = NodeKind::Unknown;
    uint32_t schema_version = 0;
    ContentHash hash; ///< del contenido; se rellena al sellarlo
};

/**
 * @brief Genero que corresponde a cada etiqueta de identificador.
 *
 * Se especializa una vez por genero y a partir de ahi la correspondencia es
 * automatica.  Antes habia que pasar el genero a mano al construir una
 * referencia, y nada impedia dar el equivocado: un identificador de variable
 * con el genero de codigo compilaba igual y fallaba mucho despues.
 */
template <typename Tag> struct NodeKindOf;

template <> struct NodeKindOf<ModuleTag> {
    static constexpr NodeKind value = NodeKind::Module;
};
template <> struct NodeKindOf<ScopeTag> {
    static constexpr NodeKind value = NodeKind::Scope;
};
template <> struct NodeKindOf<VariableTag> {
    static constexpr NodeKind value = NodeKind::Variable;
};
template <> struct NodeKindOf<StatementTag> {
    static constexpr NodeKind value = NodeKind::Statement;
};
template <> struct NodeKindOf<IrFunctionTag> {
    static constexpr NodeKind value = NodeKind::IrFunction;
};
template <> struct NodeKindOf<BlockTag> {
    static constexpr NodeKind value = NodeKind::Block;
};
template <> struct NodeKindOf<IrInstrTag> {
    static constexpr NodeKind value = NodeKind::IrInstr;
};
template <> struct NodeKindOf<CodeTag> {
    static constexpr NodeKind value = NodeKind::Code;
};

/**
 * @brief Referencia a un nodo cualquiera.
 *
 * Es lo que permite que un diagnostico cite lo que necesite sin conocer de
 * antemano la lista de cosas citables.  Anadir un genero nuevo no obliga a
 * tocar nada de lo que ya sabe referenciarlos.
 */
struct NodeRef {
    NodeKind kind = NodeKind::Unknown;
    ContentHash hash;

    /// @return @c true si no apunta a nada.
    bool empty() const { return hash.empty(); }

    bool operator==(const NodeRef &o) const {
        return kind == o.kind && hash == o.hash;
    }

    /**
     * @brief Construye una referencia desde un identificador tipado.
     *
     * El genero se deduce del tipo: no hay forma de equivocarse al darlo.
     *
     * @tparam Tag Etiqueta del identificador.
     * @param id Identificador.
     * @return La referencia generica equivalente.
     */
    template <typename Tag> static NodeRef of(const NodeId<Tag> &id) {
        NodeRef r;
        r.kind = NodeKindOf<Tag>::value;
        r.hash = id.hash;
        return r;
    }

    /**
     * @brief Vuelve a un identificador tipado, si el genero coincide.
     *
     * El camino de vuelta al mundo con tipos.  Devuelve vacio si la referencia
     * era de otro genero, en lugar de dar un identificador que apunta a algo
     * que no es lo que se espera.
     *
     * @tparam Tag Etiqueta que se espera.
     * @return El identificador, o uno vacio si el genero no corresponde.
     */
    template <typename Tag> NodeId<Tag> as() const {
        if (kind != NodeKindOf<Tag>::value) return NodeId<Tag>{};
        return NodeId<Tag>{hash};
    }
};
} // namespace vxdbg

namespace std {
/// Permite usar una referencia generica como clave de un mapa.
template <> struct hash<vxdbg::NodeRef> {
    size_t operator()(const vxdbg::NodeRef &r) const noexcept {
        return hash<vxdbg::ContentHash>{}(r.hash) ^
               static_cast<size_t>(r.kind);
    }
};
} // namespace std

#endif // VXDBG_NODE_H
