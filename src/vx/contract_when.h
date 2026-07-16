/**
 * @file contract_when.h
 * @brief El `when:` de un contrato: evaluacion, atomos y ORDEN DE PRIORIDAD
 *        entre varios que casan a la vez.
 *
 * Un contrato (`@complexity`, `@alloc`, ...) puede condicionarse con
 * `when: <expr>`, y la expresion habla de dos cosas a la vez:
 *
 *   - del TARGET (`os:` / `arch:` / `cpu:` / `mode:` / `compiler OP M.m` /
 *     `vm OP M.m`), que se resuelve al compilar;
 *   - del PARAMETRO DE TIPO (`is_float<T>()`, `sizeof<T>() OP N`), que solo
 *     tiene respuesta al monomorphizar.
 *
 * Los dos ejes se mezclan en la misma expresion (`arch:x86_64 && is_float<T>()`)
 * porque es una sola gramatica: atomos + `!` + `&&` + `||` + parentesis, con
 * precedencia `!` > `&&` > `||`.  Este modulo la parsea UNA vez y deja que cada
 * llamante diga cuanto vale cada atomo, que es lo que permite usarla tanto para
 * evaluarla de verdad (con el target y T reales) como para razonar sobre ella
 * (con asignaciones hipoteticas, al comparar especificidad).
 *
 * @par Por que hace falta comparar especificidad
 * Varios `when:` pueden casar A LA VEZ:
 *
 *     @complexity(total_post: O(1))
 *     @complexity(total_post: O(n), when: arch:x86_64)
 *
 * En x86 casan los dos y escriben el MISMO campo.  Sin una regla, gana el
 * ultimo textualmente: el orden en el fichero decide el contrato, que es
 * exactamente lo que no queremos.  La regla es:
 *
 *   1. se evaluan todas las condiciones;
 *   2. se aplican todas las que casan;
 *   3. si dos escriben el mismo campo, gana la MAS ESPECIFICA;
 *   4. si no se puede decidir cual, error de ambiguedad.
 *
 * "Mas especifica" tiene definicion exacta y decidible: A es mas especifica que
 * B si A implica B y B no implica A.  La implicacion se comprueba enumerando
 * las asignaciones de la union de atomos (son pocos: tabla de verdad trivial).
 * Sin `when:` equivale a `true`, con lo que TODO lo implica -> es la menos
 * especifica, o sea el default.  Asi el ejemplo de arriba da O(n) en x86 sin
 * depender del orden, y `arch:x86_64` contra `is_float<T>()` -- que no se
 * implican en ningun sentido -- es ambiguo y se rechaza.
 */
#ifndef VX_CONTRACT_WHEN_H
#define VX_CONTRACT_WHEN_H

#include <functional>
#include <string>
#include <vector>

#include "vx/ast.h"

namespace vx {
namespace cwhen {

/// Da valor a un atomo de la expresion.  @p ok se pone a false si el atomo no
/// se reconoce, para que el llamante avise en vez de tragarse el contrato.
using AtomEval = std::function<bool(const std::string &atomo, bool &ok)>;

/// @brief Evalua la expresion completa de un `when:`.
/// @param spec expresion (vacia = `true`).
/// @param ev   valor de cada atomo.
/// @param ok   false si algun atomo no se entendio o la expresion esta mal
///             formada.  El resultado no vale nada si @p ok sale false.
bool eval(const std::string &spec, const AtomEval &ev, bool &ok);

/// @brief Extrae los atomos de la expresion, en orden y sin repetir.
/// @return false si la expresion esta mal formada.
bool atoms(const std::string &spec, std::vector<std::string> &out);

/// Que clase de cosa es un atomo.
enum class AtomKind {
    /// Del TARGET: `clave:valor` con clave de @ref target_keys(), o
    /// `variable OP version` con variable de @ref target_vars().  Se resuelve
    /// al compilar (el `mode:` lo fija la CLI, el `arch:` el driver AOT...).
    TARGET,
    /// Del PARAMETRO DE TIPO: `pred<T>()` con pred de @ref type_preds(),
    /// opcionalmente seguido de `OP N`.  Solo se resuelve al monomorphizar.
    TIPO,
    /// Ni una cosa ni la otra -> ERROR.  No se adivina: un atomo mal escrito
    /// tiene que doler, no colarse como lo que no es.
    DESCONOCIDO,
};

/// Claves de la forma `clave:valor` de un atomo de target.  Registro CERRADO:
/// lo que no este aqui no es un atomo de target.
const std::vector<std::string> &target_keys();
/// Variables de la forma `variable OP version` de un atomo de target.
const std::vector<std::string> &target_vars();
/// Predicados de la forma `pred<T>()` sobre un parametro de tipo.
const std::vector<std::string> &type_preds();

/// Valores conocidos de `arch:`.  Registro CERRADO, y la lista sobre la que
/// `--analyze` recorre para reportar el coste de CADA arquitectura: el coste
/// total cambia con ella cuando hay variantes `@Target` de cuerpos distintos, y
/// analizar solo el host enseña media foto.  Se enumera el registro en vez de
/// rascar los `arch:` del fuente a mano, que es la clase de heuristica que hace
/// que un `archh:` mal escrito pase desapercibido.
const std::vector<std::string> &known_archs();

/// @brief Clasifica un atomo por su ESTRUCTURA contra los registros cerrados.
///
/// Nada de mirar prefijos: `vm` como prefijo casaria con cualquier predicado
/// que empezase por esas letras, y una clave mal escrita (`archh:x86_64`) se
/// colaria como atomo de target y se evaluaria a false en silencio -- el
/// contrato desapareceria sin que nadie dijese nada.  Aqui la forma decide la
/// familia y el registro decide si el nombre existe; lo demas es DESCONOCIDO.
///
/// @param[out] esperado si sale DESCONOCIDO, que se esperaba (para el mensaje).
AtomKind atom_kind(const std::string &atomo, std::string *esperado = nullptr);

/// @c true si TODOS los atomos de @p spec son de target, o sea que la expresion
/// se puede resolver al parsear.  Basta un atomo sobre T para que haya que
/// esperar a la monomorphizacion.  Un atomo DESCONOCIDO cuenta como NO-target
/// para que el error salga donde hay contexto para explicarlo.
bool only_target(const std::string &spec);

/// Relacion entre dos `when:` segun cual es mas especifico.
enum class Spec {
    A_MAS,     ///< A es mas especifica que B (A implica B, B no implica A).
    B_MAS,     ///< B es mas especifica que A.
    IGUALES,   ///< se implican mutuamente (misma condicion) -> gana la ultima.
    AMBIGUAS,  ///< ninguna implica a la otra -> el llamante debe dar error.
};

/// @brief Compara dos `when:` por especificidad.
///
/// Enumera las asignaciones de la union de sus atomos (tratados como booleanos
/// libres) y comprueba la implicacion en los dos sentidos.  Tratarlos como
/// libres es suficiente aunque en la realidad no lo sean (`arch:x86_64` y
/// `arch:arm64` se excluyen): esto solo se llama entre condiciones que YA
/// casaron en esta compilacion, y dos que se excluyen nunca casan a la vez.
///
/// Una expresion vacia es `true`: la implica todo y no implica a ninguna, o sea
/// la menos especifica.
///
/// @param ok false si alguna expresion esta mal formada o tiene demasiados
///        atomos distintos para enumerar (limite defensivo).
Spec compare(const std::string &a, const std::string &b, bool &ok);

/// Las cuatro dimensiones (+ el azucar) ya resueltas para esta compilacion.
struct Resolved {
    std::string expr;
    std::vector<std::string> vars;
    std::string partial_pre;
    std::string partial_post;
    std::string total_pre;
    std::string total_post;
};

/// Reporta un problema al resolver (ambiguedad, atomo desconocido).
using ErrFn = std::function<void(const std::string &msg)>;

/// @brief Resuelve los `@complexity` declarados sobre una funcion o metodo
///        aplicando la regla de prioridad.
///
/// Se evaluan TODAS las condiciones y se aplican TODAS las que casan.  Cuando
/// dos escriben el mismo campo, gana la mas especifica; si ninguna lo es, es
/// ambiguo y se reporta (nunca se elige a dedo ni por orden textual).
///
/// @param decls todos los @complexity de la declaracion, en orden textual.
/// @param ev    valor de cada atomo (target real, y T si se conoce).
/// @param err   se llama por cada problema; el campo en disputa se deja como
///              estaba para no inventar un contrato.
void resolve(const std::vector<ast::PendingComplexity> &decls,
             const AtomEval &ev, const ErrFn &err, Resolved &out);

} // namespace cwhen
} // namespace vx

#endif // VX_CONTRACT_WHEN_H
