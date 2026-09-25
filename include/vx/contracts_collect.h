/**
 * @file contracts_collect.h
 * @brief Leer del AST los contratos que el programa DECLARA.
 *
 * Una responsabilidad sola: recorrer las declaraciones y quedarse con lo que
 * afirman.  No verifica nada -- de eso vive `analyze::` -- ni decide que hacer
 * con un incumplimiento, que lo decide la puerta de `vx/module_checks.h`.
 *
 * Esta aparte porque estaba escrito DOS veces, una en cada camino de
 * compilacion, y de esas copias salen las divergencias: el camino que toma todo
 * programa real se habia quedado sin recoger los contratos de TIPO, asi que
 * declararlos no comprobaba nada -- peor que no tenerlos, porque parecen
 * comprobados --.
 */
#ifndef VESTA_VX_CONTRACTS_COLLECT_H
#define VESTA_VX_CONTRACTS_COLLECT_H

#include "analyze/fingerprint.h"
#include "vx/ast.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace vx {

/**
 * @brief Recoge los contratos de HUELLA (`@pure`/`@nothrow`/`@nopanic`/
 *        `@alloc`/`@stack`) declarados en un AST.
 *
 * Recorre los namespaces y toma tambien los METODOS de struct y clase, con la
 * clave con la que bajan (`Tipo__metodo`), que es la que el analizador vera: un
 * tipo cuya API son metodos podia DECLARAR sus contratos y nadie los
 * verificaba.
 *
 * Los TEMPLATES genericos se saltan: no producen intermedio -- solo lo hacen
 * sus instanciaciones --, y su clave casaria por sufijo con la de cada una,
 * reportando dos veces el mismo incumplimiento.  La monomorfizacion copia los
 * contratos, asi que cada instanciacion se verifica por su cuenta.
 *
 * @param decls Declaraciones del modulo.
 * @param out   Mapa por nombre, al que se anade.
 */
void collect_function_contracts(
    const std::vector<std::unique_ptr<ast::Node>> &decls,
    std::unordered_map<std::string, analyze::FunctionContracts> &out);

/**
 * @brief Recoge los contratos de TIPO (`@pod`/`@no_heap`/`@size`) declarados
 *        sobre un struct, una clase o un enum.
 *
 * Mismo criterio que el de arriba: se lee del AST y no se serializa, porque es
 * conocimiento de compilacion que la generacion de codigo no necesita.
 *
 * @param decls Declaraciones del modulo.
 * @param out   Mapa por nombre de tipo, al que se anade.
 */
void collect_type_contracts(
    const std::vector<std::unique_ptr<ast::Node>> &decls,
    std::unordered_map<std::string, analyze::TypeContracts> &out);

} // namespace vx

#endif // VESTA_VX_CONTRACTS_COLLECT_H
