/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file diagnostic.h
 * @brief La explicacion de un fallo como GRAFO, no como texto.
 *
 * Cuando algo se rompe, la explicacion util casi nunca es una frase: es un
 * conjunto de hechos encadenados, y muchas veces NI SIQUIERA una cadena.
 *
 *     desbordamiento
 *          |
 *        suma
 *        /    \
 *       a      b
 *       |      |
 *   literal  llamada
 *
 * Eso no es una lista: es un grafo dirigido.  Aplanarlo obliga a elegir un
 * orden de lectura y pierde de donde sale cada cosa; convertirlo en una cadena
 * de caracteres lo mata del todo -- ya no se puede traducir, ni plegar, ni
 * navegar, ni volcar a un formato de maquina, ni presentar distinto segun quien
 * mire.  Aqui se guarda el grafo y cada frontend lo cuenta a su manera.
 *
 * **Ni una frase montada.**  Solo codigos del catalogo (`VXNNNN`) y datos.  La
 * frase la compone la presentacion, en el idioma que toque, que es la misma
 * regla que sigue el resto del compilador.
 *
 * **Nada de un lenguaje concreto.**  No se habla de excepciones: se habla de
 * valores del runtime relevantes para explicar el fallo, que valen igual para
 * un `panic`, una senal, un `Result` o una promesa rechazada.
 */

#ifndef VXDBG_DIAGNOSTIC_H
#define VXDBG_DIAGNOSTIC_H

#include "vxdbg/compilation_unit.h"
#include "vxdbg/ids.h"
#include "vxdbg/node.h"
#include "vxdbg/source_meta.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vxdbg {

/// Version del esquema de esta capa.
static constexpr uint32_t DIAG_SCHEMA_VERSION = 1;

/**
 * @brief Como se llega a un valor del runtime.
 *
 * Envuelto y no un entero suelto porque no todo runtime direcciona igual: hay
 * objetos sin direccion, otros detras de un indice o de una tabla.  Con el
 * envoltorio, otro runtime cambia la representacion sin tocar el formato.
 */
struct RuntimeHandle {
    uint64_t value = 0;
    /// Como interpretar @c value ("direccion", "handle", "indice").  Lo pone
    /// el runtime; el formato no lo interpreta.
    std::string representation;
};

/**
 * @brief Un valor del runtime que hace falta para explicar el fallo.
 *
 * No se llama excepcion a proposito.  Un lenguaje tiene excepciones, otro
 * `panic`, otro senales, otro `Result<T,E>`, otro promesas rechazadas: lo comun
 * es que hay un valor, de un tipo, creado en un sitio, que llego hasta aqui.
 * Con eso, un runtime compartido explica el fallo de un programa escrito en
 * cualquier cosa.
 */
struct RuntimeObject {
    DebugNodeHeader header{NodeKind::RuntimeObject, DIAG_SCHEMA_VERSION, {}};

    LanguageEntityId type;  ///< de que tipo es
    RuntimeHandle handle;   ///< como llegar a el, si sigue vivo
    StatementId created_at; ///< donde se creo
    StatementId raised_at;  ///< donde se convirtio en un fallo
    /// Si surgio mientras se atendia otro, cual.  Encadenar causas es lo que
    /// permite explicar el fallo original y no solo el ultimo.
    NodeRef cause;
    /// Mensaje que traiga el propio valor, si lo trae.  No se interpreta ni se
    /// traduce: es del programa, no del sistema.
    std::string message;
};

/// De que es un dato de un diagnostico.
enum class ArgumentKind : uint8_t {
    Text = 0,
    Integer = 1,
    Unsigned = 2,
    Float = 3,
    Bool = 4,
    Null = 5,
    Address = 6, ///< se presenta en hexadecimal
    Node = 7,    ///< apunta a un nodo del grafo
};

/**
 * @brief Un dato con el que se rellena la plantilla del catalogo.
 *
 * No todo es texto: un numero es un numero y una direccion se ensena en
 * hexadecimal.  Convertirlo todo a cadena antes de tiempo obliga a quien
 * presenta a adivinar como formatearlo, y le impide adaptarlo -- separar
 * millares, acortar una direccion, enlazar un nodo.
 */
struct DiagnosticArgument {
    ArgumentKind kind = ArgumentKind::Text;
    std::string text;
    int64_t integer = 0;
    uint64_t unsigned_value = 0;
    double real = 0.0;
    NodeRef node;
};

/// Que papel juega un hecho dentro de la explicacion.
enum class FactRole : uint8_t {
    Subject = 0,     ///< de que va el fallo
    Cause = 1,       ///< por que paso
    Origin = 2,      ///< de donde salio
    Context = 3,     ///< donde estabamos
    Consequence = 4, ///< a que llevo
    Suggestion = 5,  ///< que se puede hacer
    /// Relacionado, sin ser causa ni consecuencia.  Hace falta mas de lo que
    /// parece: mucho de lo que ayuda a entender un fallo no lo causo.
    Related = 6,
};

/**
 * @brief Un hecho de la explicacion.
 *
 * Apunta a un nodo CUALQUIERA en vez de describirlo: asi la explicacion no
 * duplica lo que ya se sabe del programa, y quien la presente puede desplegar
 * cuanto quiera de cada eslabon -- desde el nombre a secas hasta la jerarquia
 * entera del tipo -- sin que la explicacion tenga que preverlo.  Anadir un
 * genero de nodo nuevo no obliga a tocar nada de esto.
 */
struct DiagnosticFact {
    FactRole role = FactRole::Subject;
    NodeRef target;  ///< a que se refiere
    SourceSpan span; ///< donde se ve, si se ve en algun sitio

    /// Codigo del catalogo que explica ESTE eslabon.  El texto vive en el
    /// catalogo multi-idioma, nunca aqui.
    std::string code;
    std::vector<DiagnosticArgument> args;
};

/// Como se relaciona un hecho con otro.
enum class FactRelation : uint8_t {
    Because = 0,     ///< aquel explica a este
    Through = 1,     ///< se llego a este a traves de aquel
    Contains = 2,    ///< aquel es parte de este
    Alternative = 3, ///< aquel es otra posibilidad
    LeadsTo = 4,     ///< este lleva a aquel
};

/**
 * @brief Una arista entre dos hechos.
 */
struct DiagnosticEdge {
    uint32_t from = 0; ///< indice en @ref Diagnostic::facts
    uint32_t to = 0;
    FactRelation relation = FactRelation::Because;
};

/**
 * @brief Un fallo, explicado como grafo.
 *
 * Los hechos son los nodos y las aristas dicen como se encadenan.  Una
 * explicacion lineal es simplemente un grafo en linea, asi que no se pierde
 * nada por poder representar las que no lo son.
 */
struct Diagnostic {
    DebugNodeHeader header{NodeKind::Diagnostic, DIAG_SCHEMA_VERSION, {}};

    /// Codigo del catalogo para el fallo en si.
    std::string code;
    std::vector<DiagnosticArgument> args;

    /// Donde ocurrio y en que contexto (interpretado, al vuelo, al compilar).
    ExecutionContext context;
    SourceSpan where;

    std::vector<DiagnosticFact> facts;
    std::vector<DiagnosticEdge> edges;

    /// Valores del runtime citados, si los hay.
    std::vector<RuntimeObject> objects;

    /// @return Los indices de los hechos por los que conviene empezar a contar
    ///         (los que no son destino de ninguna arista).
    std::vector<uint32_t> roots() const;
};

} // namespace vxdbg

#endif // VXDBG_DIAGNOSTIC_H
