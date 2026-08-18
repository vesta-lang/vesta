/**
 * @file module_interop.h
 * @brief Interop entre TypeChecker y formato @c .vxi ( M.2.d).
 *
 * Funciones libres que conectan el estado del @c TypeChecker con un
 * @c VxiModule.  Viven fuera del TypeChecker para no forzar
 * @c vxi_format.h en todos sus consumidores.
 */

#ifndef VX_MODULE_INTEROP_H
#define VX_MODULE_INTEROP_H

#include <cstdint>
#include <vector>

#include "vx/type_checker.h"

namespace vx {

struct VxiModule; // fwd decl, definido en vxi_format.h

/**
 * @brief Extrae los simbolos publicos del @c TypeChecker a un
 * @c VxiModule listo para serializar.
 *
 * En el MVP M2, "publico" = todos los typedefs, structs, classes,
 * enums y funciones top-level del modulo.  La regla de @c public /
 * @c private explicito llega en M6.
 *
 * @param tc           TypeChecker con el modulo ya verificado.
 * @param source_hash  Hash FNV-1a 64 del source crudo (para
 *                     invalidacion incremental del cache).
 * @param out          @c VxiModule destino.  Se sobrescribe.
 */
void export_typechecker_to_vxi(const TypeChecker &tc, uint64_t source_hash,
                               VxiModule &out,
                               const std::string &strip_prefix = "");

/**
 * @brief Inyecta simbolos importados de un @c VxiModule en las tablas
 * del TypeChecker actual.
 *
 * En el MVP M2 solo se inyectan los simbolos LISTADOS en
 * @c only_symbols.  Los imports plain (`import "x";`) o con alias
 * (`import "x" as a;`) requieren namespace support, pendiente en M2.x.
 *
 * @param tc           TypeChecker destino.
 * @param mod          Modulo @c VxiModule decodificado.
 * @param only_symbols Lista de simbolos a importar (con rename opcional).
 *                     Si vacio: no se inyecta nada.
 */
void import_vxi_into_typechecker(
    TypeChecker &tc, const VxiModule &mod,
    const std::vector<TypeChecker::VxiOnlyEntry> &only_symbols,
    const std::string &module_name);

/**
 * @brief Variante que devuelve la lista de simbolos solicitados pero
 * NO encontrados (o privados) en el `.vxi`.  Util para que el caller
 * (compile_vx_project) emita diagnosticos cross-module precisos
 * ( M6.a L.3).
 */
std::vector<std::string> import_vxi_into_typechecker_with_missing(
    TypeChecker &tc, const VxiModule &mod,
    const std::vector<TypeChecker::VxiOnlyEntry> &only_symbols,
    const std::string &module_name);

/**
 * @brief  M.7: registra un namespace para un @c "import \"lib\";"
 * o @c "import \"lib\" as alias;" plain (sin @c only).
 *
 * @param tc          TypeChecker del consumidor.
 * @param local_name  Nombre visible (alias o module_name si sin alias).
 * @param module_name Nombre original del modulo (para mensajes de error).
 * @param mod         Modulo @c VxiModule decodificado del dep.
 *
 * Tras esta llamada, @c local_name esta registrado como un
 * Symbol::Namespace en el scope global del consumidor.  El usuario puede
 * acceder a sus simbolos con la sintaxis @c local_name.simbolo.
 */
void register_namespace_for_import(TypeChecker &tc,
                                   const std::string &local_name,
                                   const std::string &module_name,
                                   const VxiModule &mod);

/**
 * @brief #cross-module-generics: inyecta las plantillas genericas + conceptos
 * de un `.vxi` importado en el TypeChecker del importador.
 *
 * Re-parsea el TEXTO FUENTE de cada plantilla (`struct Caja<T>`, `concept N`,
 * etc.) y la anyade a @c mod_.decls del importador, para que pueda
 * monomorphizar `Caja<i64>` cross-module.  Debe llamarse ANTES de @c run().
 * Si @p wanted no esta vacio, solo inyecta las plantillas con esos nombres
 * (import `only`); si esta vacio, inyecta todas (import plain/namespace).
 * @p ns_prefix: si no esta vacio, registra ademas el nombre cualificado
 * `<ns>.<Template>` como alias del template para los imports con namespace.
 * @p alias_unqualified: nombres (originales) de comptime/macro fns que deben
 * quedar invocables SIN cualificar (import `only`).  Para cada uno se registra
 * su nombre suelto en comptime_fns_ apuntando al decl mangled -- consistente
 * con como una fn regular via `only` queda en scope sin cualificar.
 */
void inject_generic_templates_from_vxi(
    TypeChecker &tc, const VxiModule &mod,
    const std::unordered_set<std::string> &wanted, const std::string &ns_prefix,
    const std::unordered_set<std::string> &alias_unqualified = {});

} // namespace vx

#endif // VX_MODULE_INTEROP_H
