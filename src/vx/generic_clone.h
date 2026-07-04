/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file generic_clone.h
 * @brief Utilidades de clonacion de AST con sustitucion de type-params.
 *
 * Modulo interno del frontend Vex extraido de type_checker.cpp para
 * mantener cada fichero manejable (la monomorphizacion de clases,
 * structs, funciones y metodos genericos comparte estas rutinas).
 *
 * La monomorphizacion clona el AST de una plantilla generica sustituyendo
 * los type params (T, U, ...) por los args concretos en TODOS los
 * @c TypeNode encontrados (firmas, bodies de metodo, etc).  El cloning es
 * AST-puro: no depende de @c TypeChecker, solo de @c ast y del helper
 * libre @c type_to_string (en vex/types.h).  Esto permite reutilizarlo
 * desde varios .cpp del frontend sin acoplar al type checker.
 */

#ifndef VEX_GENERIC_CLONE_H
#define VEX_GENERIC_CLONE_H

#include "vx/ast.h"
#include "vx/types.h"

#include <memory>
#include <string>
#include <vector>

namespace vx {

/// @namespace vx::vexgen
/// @brief Espacio de nombres de las utilidades de monomorphizacion AST.
namespace vexgen {

/**
 * @struct GenSubst
 * @brief Mapping nombre-de-param -> tipo concreto.
 *
 * Usado para substituir @c NamedTypeNode("T") por el tipo correspondiente
 * en la instanciacion.  Se guarda como dos arrays paralelos (@c params y
 * @c args) por simplicidad y localidad de cache.  Ambos punteros pueden
 * ser nulos (clon sin sustitucion, e.g. copia literal de un AST).
 */
struct GenSubst {
    const std::vector<std::string> *params = nullptr;
    const std::vector<Type> *args = nullptr;
};

/// @brief Mangling canonico de un @c Type para nombres de instancia.
std::string mangle_type(const Type &t);

/// @brief Mangling de una lista de type-args (`i64_i32`), separados por '_'.
std::string mangle_args(const std::vector<Type> &args);

/// @brief Reconstruye un @c TypeNode AST a partir de un @c Type resuelto.
///
/// Preserva pointee/element y tamano de punteros y arrays (un type-arg
/// `i64*` o `i64[4]` no debe colapsar a un PrimitiveTypeNode generico).
/// Tambien preserva @c is_virtual (VirtualPtr<T> vs T* host).
std::unique_ptr<ast::TypeNode> type_node_from_type(const Type &a,
                                                   const SourceLoc &loc);

/// @brief Clona un @c TypeNode aplicando la sustitucion @p g.
std::unique_ptr<ast::TypeNode>
clone_type_with_subst(const ast::TypeNode *t, const GenSubst &g);

/// @brief Clona una @c Expr aplicando la sustitucion @p g (default: vacia).
std::unique_ptr<ast::Expr> clone_expr(const ast::Expr *e,
                                      const GenSubst &g = {});

/// @brief Clona un @c Stmt aplicando la sustitucion @p g (default: vacia).
std::unique_ptr<ast::Stmt> clone_stmt(const ast::Stmt *s,
                                      const GenSubst &g = {});

} // namespace vexgen
} // namespace vx

#endif // VEX_GENERIC_CLONE_H
