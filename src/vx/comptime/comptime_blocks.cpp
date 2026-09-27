/**
 * @file comptime_blocks.cpp
 * @brief Implementacion de la conversion de `comptime { }` en funciones.
 * @see vx/comptime/comptime_blocks.h
 */
#include "vx/comptime/comptime_blocks.h"

#include "ir/synthetic_symbols.h" // el nombre del bloque

#include <memory>
#include <vector>

namespace vx {

namespace {

/**
 * @brief El tipo `i64`.
 * @return Un nodo de tipo nuevo.
 */
std::unique_ptr<ast::TypeNode> i64_type() {
    auto t = std::make_unique<ast::PrimitiveTypeNode>();
    t->prim = PrimitiveKind::I64;
    return t;
}

/**
 * @brief La funcion comptime que ejecuta el cuerpo de un bloque.
 * @param cb   El bloque; sus sentencias se MUEVEN a la funcion.
 * @param name Su simbolo.
 * @return `comptime i64 name() { <sentencias>; return 0; }`.
 */
std::unique_ptr<ast::FunctionDecl> block_function(ast::ComptimeBlockStmt &cb,
                                                  const std::string &name) {
    auto fn = std::make_unique<ast::FunctionDecl>();
    fn->name = name;
    fn->is_comptime = true;
    fn->loc = cb.loc;
    fn->return_type = i64_type();
    auto body = std::make_unique<ast::BlockStmt>();
    for (auto &st : cb.stmts)
        body->body.push_back(std::move(st));
    auto ret = std::make_unique<ast::ReturnStmt>();
    auto zero = std::make_unique<ast::IntLitExpr>();
    zero->value = 0;
    ret->value = std::move(zero);
    body->body.push_back(std::move(ret));
    fn->body = std::move(body);
    return fn;
}

/**
 * @brief La constante que llama al bloque, y con ello lo ejecuta al compilar.
 * @param name El simbolo de la funcion del bloque.
 * @param loc  Donde estaba el bloque.
 * @return `const i64 name_r = name();`.
 */
std::unique_ptr<ast::GlobalVarDecl> block_call(const std::string &name,
                                               const SourceLoc &loc) {
    auto gv = std::make_unique<ast::GlobalVarDecl>();
    gv->name = name + "_r";
    gv->is_const = true;
    gv->loc = loc;
    gv->type = i64_type();
    auto call = std::make_unique<ast::CallExpr>();
    auto callee = std::make_unique<ast::IdentExpr>();
    callee->name = name;
    call->callee = std::move(callee);
    call->loc = loc;
    gv->init = std::move(call);
    return gv;
}

} // namespace

void comptime_blocks_to_functions(ast::ModuleNode &mod,
                                  const std::string &owner) {
    std::vector<std::unique_ptr<ast::Node>> kept;
    std::vector<std::unique_ptr<ast::Node>> synthetic;
    kept.reserve(mod.decls.size());
    size_t index = 0;
    for (auto &d : mod.decls) {
        if (d && d->kind == ast::NodeKind::ComptimeBlockStmt) {
            auto *cb = static_cast<ast::ComptimeBlockStmt *>(d.get());
            const std::string name = ir::ctblock_symbol(owner, index++);
            synthetic.push_back(block_function(*cb, name));
            synthetic.push_back(block_call(name, cb->loc));
            // El bloque original se descarta: su cuerpo ya esta en la funcion.
        } else {
            kept.push_back(std::move(d));
        }
    }
    /* Reasignar SIEMPRE: el bucle ya movio cada decl a `kept`, dejando
     * `mod.decls` con punteros vaciados aunque no hubiera bloques. */
    for (auto &s : synthetic)
        kept.push_back(std::move(s));
    mod.decls = std::move(kept);
}

} // namespace vx
