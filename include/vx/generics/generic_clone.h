/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file generic_clone.h
 * @brief Utilidades de clonacion de AST con sustitucion de type-params.
 *
 * Modulo interno del frontend Vesta extraido de type_checker.cpp para
 * mantener cada fichero manejable (la monomorphizacion de clases,
 * structs, funciones y metodos genericos comparte estas rutinas).
 *
 * La monomorphizacion clona el AST de una plantilla generica sustituyendo
 * los type params (T, U, ...) por los args concretos en TODOS los
 * @c TypeNode encontrados (firmas, bodies de metodo, etc).  El cloning es
 * AST-puro: no depende de @c TypeChecker, solo de @c ast y del helper
 * libre @c type_to_string (en vx/types.h).  Esto permite reutilizarlo
 * desde varios .cpp del frontend sin acoplar al type checker.
 */

#ifndef VX_GENERIC_CLONE_H
#define VX_GENERIC_CLONE_H

#include "vx/ast.h"
#include "vx/types.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace vx {

/// @namespace vx::vxgen
/// @brief Espacio de nombres de las utilidades de monomorphizacion AST.
namespace vxgen {

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
    /**
     * @brief Los argumentos TAL COMO SE ESCRIBIERON, en lugar de @c args.
     *
     * Lo que un concepto inyecta en un tipo se clona ANTES de montar los
     * layouts, y ahi `View<Punto>` todavia no se puede resolver a un @c Type:
     * `Punto` no esta registrado.  No hace falta: el argumento se copia como
     * se escribio y se resuelve despues, con todo lo demas.  Si esta puesto,
     * gana a @c args.
     */
    const std::vector<const ast::TypeNode *> *arg_nodes = nullptr;

    /**
     * @brief Hay algo que sustituir.
     * @return Cierto si hay parametros y argumentos de alguna de las formas.
     */
    [[nodiscard]] bool active() const {
        return params != nullptr && (args != nullptr || arg_nodes != nullptr);
    }
};

/// @brief Mangling canonico de un @c Type para nombres de instancia.
std::string mangle_type(const Type &t);

/// @brief Mangling de una lista de type-args (`i64_i32`), separados por '_'.
std::string mangle_args(const std::vector<Type> &args);

/**
 * @brief El nombre de una INSTANCIA de plantilla: `Box` y `<i64>` ->
 *        `Box_i64`.
 *
 * Es el de todas -- structs, clases, enums, funciones y metodos genericos --,
 * y estaba escrito a mano en cada una.  Un nombre de plantilla importada con
 * namespace llega cualificado (`lib.Box`); el punto no vale en una etiqueta
 * del IR ni del enlazador, asi que sale como `_`.
 *
 * @param tmpl Nombre de la plantilla.
 * @param args Sus argumentos de tipo.
 * @return El nombre de la instancia.
 */
std::string generic_instance_name(const std::string &tmpl,
                                  const std::vector<Type> &args);

/// @brief Reconstruye un @c TypeNode AST a partir de un @c Type resuelto.
///
/// Preserva pointee/element y tamano de punteros y arrays (un type-arg
/// `i64*` o `i64[4]` no debe colapsar a un PrimitiveTypeNode generico).
/// Tambien preserva @c is_virtual (VirtualPtr<T> vs T* host).
std::unique_ptr<ast::TypeNode> type_node_from_type(const Type &a,
                                                   const SourceLoc &loc);

/// @brief Clona un @c TypeNode aplicando la sustitucion @p g (default:
///        vacia, o sea una copia exacta, marcas incluidas).
std::unique_ptr<ast::TypeNode> clone_type_with_subst(const ast::TypeNode *t,
                                                     const GenSubst &g = {});

/// @brief Clona una lista de tipos COMPARTIDOS (argumentos de un concepto, de
///        una base) aplicando @p g: cada instancia recibe los suyos, ya
///        concretos, sin tocar los de la plantilla.
std::vector<std::shared_ptr<ast::TypeNode>>
clone_shared_types_with_subst(
    const std::vector<std::shared_ptr<ast::TypeNode>> &types,
    const GenSubst &g);

/// @brief Clona una lista de conceptos nombrados (`: Da<T>, View<T>`) con sus
///        argumentos sustituidos por @p g.
std::vector<ast::ConceptRef>
clone_concept_refs_with_subst(const std::vector<ast::ConceptRef> &refs,
                              const GenSubst &g);

/**
 * @brief Copia a una INSTANCIA lo que su plantilla declara tras `:` -- base,
 *        sus argumentos y conceptos --, con los argumentos sustituidos:
 *        `Caja<T> : Da<T>` pasa a `Caja<i64> : Da<i64>`.
 *
 * Struct y clase lo hacen igual (tienen los mismos tres campos), asi que es
 * una sola funcion para los dos.
 *
 * @tparam Decl `ast::StructDecl` o `ast::ClassDecl`.
 * @param src La plantilla.
 * @param dst La instancia.
 * @param g   La sustitucion de la instancia.
 */
template <class Decl>
void clone_header_with_subst(const Decl &src, Decl &dst, const GenSubst &g) {
    dst.super_name = src.super_name;
    dst.super_args = clone_shared_types_with_subst(src.super_args, g);
    dst.interface_names = clone_concept_refs_with_subst(src.interface_names, g);
}

/**
 * @brief Copia un parametro con su tipo sustituido y TODO lo que lleva: la
 *        direccion (`in`/`out`/`inout`), si es variadico, el registro ABI.
 *
 * Las copias sueltas de metodos y lambdas solo copiaban nombre y tipo, asi
 * que un `inout T x` de un generico perdia el `inout` al instanciarse.
 *
 * @param p El parametro.
 * @param g La sustitucion.
 * @return La copia.
 */
std::unique_ptr<ast::ParamDecl> clone_param_with_subst(const ast::ParamDecl &p,
                                                       const GenSubst &g);

/// @brief Clona una @c Expr aplicando la sustitucion @p g (default: vacia).
std::unique_ptr<ast::Expr> clone_expr(const ast::Expr *e,
                                      const GenSubst &g = {});

/// @brief Clona un @c Stmt aplicando la sustitucion @p g (default: vacia).
std::unique_ptr<ast::Stmt> clone_stmt(const ast::Stmt *s,
                                      const GenSubst &g = {});

/**
 * @brief Reescribe in-place los identificadores de un AST segun @p renames.
 *
 * Recorre expresiones y sentencias renombrando cada @c IdentExpr cuyo nombre
 * este en el mapa.  No toca los campos (`obj.x`), ni los nombres de metodo, ni
 * los declarados dentro del propio arbol: solo las REFERENCIAS por nombre.
 *
 * Lo usa la inyeccion de plantillas genericas cross-module: el cuerpo de una
 * plantilla viaja como TEXTO en el `.vxi` y se re-parsea en el modulo que la
 * usa, donde los simbolos de su modulo de origen no estan en scope.  Reescribir
 * sus llamadas al label real (`vx_atomic__vx_atomic_load64`) hace que resuelvan
 * y enlacen sin exponer esos nombres al consumidor.
 *
 * @param renames nombre original -> nombre nuevo.  Vacio = no-op.
 */
void rename_idents(ast::Node *n,
                   const std::unordered_map<std::string, std::string> &renames);

} // namespace vxgen
} // namespace vx

#endif // VX_GENERIC_CLONE_H
