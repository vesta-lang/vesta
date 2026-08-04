/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file source_meta.h
 * @brief Modelo SEMANTICO del programa, sin presuponer lenguaje.
 *
 * Capa mas alta del subsistema.
 *
 * **No hay `class` ni `struct` en este fichero.**  Hay @ref LanguageEntity, con
 * un @c kind que decide cada frontend: uno de Vesta declarara entidades
 * `"class"`, `"struct"` o `"enum"`; uno de Rust, `"trait"` o `"impl"`; uno de
 * Lisp, lo que le convenga.  El formato no los conoce y no le hace falta: para
 * explicar un error basta con saber que hay una entidad llamada `Lector`, de
 * genero `"class"`, que deriva de otra llamada `Flujo`.  Fijar aqui el
 * vocabulario de un lenguaje habria obligado a los demas a inventarse otro
 * formato, que es justo lo que se quiere evitar.
 *
 * Se llaman entidades del LENGUAJE y no del fuente porque muchas no estan
 * escritas en ningun sitio: una especializacion implicita, un cierre, un tipo
 * sintetizado o un modulo generado son igual de reales para quien depura, y
 * llamarlas "del fuente" habria sido mentira desde el primer dia.
 *
 * Lo que SI es comun, y por eso vive aqui, son las RELACIONES: derivar, cumplir
 * un contrato, contener miembros, usar a otro.  Cualquier lenguaje con tipos y
 * funciones las tiene, aunque las escriba distinto.
 *
 * Mismo patron que las demas capas: el NODO describe lo que no cambia, y lo que
 * depende de como se haya compilado vive en un MAPA aparte -- las ubicaciones
 * de una variable estan en @ref VariableMap, no en @ref VariableNode, por la
 * misma razon por la que la colocacion del codigo no esta en el nodo de codigo.
 */

#ifndef VXDBG_SOURCE_META_H
#define VXDBG_SOURCE_META_H

#include "vxdbg/ids.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vxdbg {

/// Version del esquema de esta capa.
static constexpr uint32_t SOURCE_SCHEMA_VERSION = 1;


/**
 * @brief Un fichero de codigo, una sola vez.
 *
 * Las posiciones lo referencian en vez de repetir la ruta: en un proyecto
 * grande hay millones de posiciones y un punado de ficheros, y guardar la
 * cadena en cada una multiplicaria el tamano sin anadir nada.  De paso, tener
 * el fichero como nodo permite comprobar que el fuente no ha cambiado desde que
 * se compilo, que es lo que evita ensenar la linea equivocada.
 */
struct FileNode {
    /// Version de ESTE nodo.  Cada uno lleva la suya para poder cambiar de
    /// forma sin arrastrar a los demas, y el codec la exige al leer.
    static constexpr uint32_t kSchemaVersion = 1;
    DebugNodeHeader header{NodeKind::File, kSchemaVersion, {}};
    std::string path;
    ContentHash checksum; ///< del contenido, para detectar que ya no coincide
    std::string language; ///< lenguaje del fichero
    std::string encoding; ///< codificacion del texto
};

/**
 * @brief Un punto del fuente.
 */
struct SourcePos {
    FileId file;
    uint32_t line = 0;
    uint16_t column = 0; ///< base 1; 0 = no se sabe
};

/**
 * @brief Un TRAMO del fuente, de un punto a otro.
 *
 * Una sentencia rara vez cabe en una linea -- una llamada con argumentos, un
 * `match`, un bloque -- y guardar solo donde empieza impide resaltar lo que de
 * verdad fallo.  Con el tramo, una herramienta puede subrayar exactamente la
 * expresion culpable.
 */
struct SourceSpan {
    FileId file;
    uint32_t begin_line = 0;
    uint16_t begin_column = 0;
    uint32_t end_line = 0;
    uint16_t end_column = 0;
};

/**
 * @brief Como se relaciona una entidad con otra.
 *
 * El conjunto es corto a proposito: son las relaciones que comparten los
 * lenguajes, no las que los distinguen.  Un `extends` de Java, un `:` de Vesta
 * y una base de C++ son todos @ref Derives; que cada uno lo escriba distinto es
 * cosa de como se presente, no de como se guarde.
 */
enum class RelationKind : uint8_t {
    Derives = 0,      ///< hereda o extiende
    Implements = 1,   ///< cumple un contrato (interface, trait, protocol)
    Contains = 2,     ///< miembro: campo, metodo, variante, constante
    DeclaredIn = 3,   ///< pertenece a un modulo o espacio de nombres
    Instantiates = 4, ///< es una instancia concreta de algo generico
    Aliases = 5,      ///< es otro nombre para otra entidad
    Overrides = 6,    ///< redefine un miembro de una entidad de la que deriva
    /// Usa a otra: llamarla, leerla, tenerla como tipo.  Responde a "quien usa
    /// esto" sin tener que reconstruir el arbol del programa.
    Uses = 7,
};

/**
 * @brief Una relacion con otra entidad.
 */
struct Relation {
    RelationKind kind = RelationKind::Contains;
    LanguageEntityId target;
    /// Como lo llama el frontend, si le da un nombre propio (`"mixin"`,
    /// `"companion"`...).  Vacio = el generico basta.
    std::string lang_role;
};

/// De que es un valor de atributo.
enum class AttributeKind : uint8_t {
    String = 0,
    Integer = 1,
    Bool = 2,
    Reference = 3, ///< apunta a otra entidad
};

/**
 * @brief Un dato suelto que el frontend quiere conservar.
 *
 * Es la valvula de escape que evita cambiar el formato cada vez que un lenguaje
 * necesita guardar algo suyo.  Lleva su genero porque no todo es texto: un
 * indice de vtable es un numero y convertirlo en cadena obligaria a quien lo
 * lea a adivinar como interpretarlo.
 */
struct Attribute {
    std::string name;
    AttributeKind kind = AttributeKind::String;
    std::string text;         ///< si @ref AttributeKind::String
    int64_t number = 0;       ///< si Integer o Bool
    LanguageEntityId reference; ///< si Reference
};

/**
 * @brief De que ESPECIE es una entidad, en terminos que cualquier lenguaje
 *        comparte.
 *
 * El conjunto es cerrado y corto por el mismo motivo que @ref RelationKind: es
 * lo que permite a quien lee decidir sin conocer el lenguaje -- si algo es una
 * funcion, se puede llamar; si es un contrato, se cumple, no se instancia.
 *
 * Como nombra cada lenguaje a los suyos va aparte, en @c lang_kind.  Tenerlo
 * todo en una cadena parecia mas flexible, pero el precio es que "field",
 * "Field" y "static_field" acaban conviviendo y nadie puede comparar nada:
 * quien consume tendria que conocer las variantes de cada frontend, que es
 * justo lo contrario de lo que se busca.
 */
enum class EntityKind : uint8_t {
    Unknown = 0,
    /// Tipo con instancias: clase, struct, registro, union.
    Type = 1,
    /// Algo que se cumple y no se instancia: interfaz, trait, protocolo,
    /// concepto.
    Contract = 2,
    /// Tipo por enumeracion de casos, con o sin datos asociados.
    Enumeration = 3,
    /// Algo que se ejecuta: funcion, metodo, constructor, destructor.
    Function = 4,
    /// Miembro con almacenamiento.
    Field = 5,
    /// Valor fijo: constante, variante sin datos.
    Constant = 6,
    /// Agrupacion: modulo, espacio de nombres, paquete.
    Module = 7,
    /// Otro nombre para algo que ya existe.
    Alias = 8,
};

/**
 * @brief Cualquier cosa que el frontend declare.
 *
 * El @c kind dice de que especie es, en terminos comunes; el @c lang_kind, como
 * lo llama su lenguaje.  Lo que el formato entiende ademas son las relaciones,
 * que es lo que permite a un diagnostico decir "en el metodo `parse` de
 * `Lector`, que deriva de `Flujo` y cumple `Cerrable`" sin saber que significa
 * "clase" en ese lenguaje.
 */
struct LanguageEntity {
    static constexpr uint32_t kSchemaVersion = 2;
    DebugNodeHeader header{NodeKind::Entity, kSchemaVersion, {}};

    std::string name;      ///< como se llama
    /// Identidad SEMANTICA, la que decide si dos entidades son la misma.  El
    /// nombre visible no sirve: `Vector` de dos espacios de nombres distintos, o
    /// dos instanciaciones de la misma plantilla, se llaman igual y no lo son.
    /// Quien emite pone aqui la clave con la que el propio compilador las
    /// distingue.
    std::string qualified;
    EntityKind kind = EntityKind::Unknown;
    /// Genero SEGUN SU LENGUAJE: "class", "struct", "trait", "macro",
    /// "template", "property"...  El formato lo transporta, no lo juzga.
    std::string lang_kind;

    std::vector<Relation> relations;
    std::vector<Attribute> attributes;
    SourceSpan declared_at;

    /// Si la entidad tiene cuerpo, la funcion intermedia que lo implementa.
    /// Vacio para las que no ejecutan nada.
    IrFunctionId body;

    /// Tamano y alineamiento de una instancia, si el concepto aplica.
    uint32_t byte_size = 0;
    uint32_t alignment = 0;
};

/**
 * @brief Un ambito lexico.
 *
 * No lleva la lista de sus variables: cada variable ya sabe a que ambito
 * pertenece, y repetirlo en los dos sitios es una redundancia que puede acabar
 * discrepando.  Quien necesite recorrer las variables de un ambito construye un
 * indice, como con cualquier otra relacion inversa.
 */
struct ScopeNode {
    static constexpr uint32_t kSchemaVersion = 1;
    DebugNodeHeader header{NodeKind::Scope, kSchemaVersion, {}};
    ScopeId parent;
    LanguageEntityId owner; ///< entidad a la que pertenece
    SourceSpan span;
};

/**
 * @brief Una variable, tal como la escribio quien programa.
 *
 * Solo lo que NO depende de como se haya compilado.  Donde vive -- un registro,
 * un hueco de la pila, en ningun sitio porque el optimizador la borro -- va en
 * @ref VariableMap, porque eso cambia con cada backend y con cada nivel de
 * optimizacion mientras que la variable sigue siendo la misma.
 */
struct VariableNode {
    static constexpr uint32_t kSchemaVersion = 1;
    DebugNodeHeader header{NodeKind::Variable, kSchemaVersion, {}};
    std::string name;
    LanguageEntityId type;
    ScopeId scope;
    bool is_parameter = false;
    SourceSpan declared_at;
};

/**
 * @brief Una sentencia o expresion.
 *
 * Es la unidad que interesa al poner un punto de parada: "esta linea" es en
 * realidad "esta sentencia".  A que codigo bajo NO se guarda aqui, sino en el
 * mapa de bajada (@c lowering_map.h), por el mismo motivo que las demas capas
 * separan el nodo de su correspondencia.
 */
struct StatementNode {
    static constexpr uint32_t kSchemaVersion = 1;
    DebugNodeHeader header{NodeKind::Statement, kSchemaVersion, {}};
    SourceSpan span;
    ScopeId scope;
    /// Como lo llama el frontend (`"if"`, `"call"`, `"assign"`...).  Permite
    /// explicar el fallo con la palabra correcta sin que el formato tenga que
    /// conocer la gramatica de nadie.
    std::string lang_kind;
};

} // namespace vxdbg

#endif // VXDBG_SOURCE_META_H
