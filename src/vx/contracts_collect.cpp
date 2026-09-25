/**
 * @file contracts_collect.cpp
 * @brief Implementacion de la lectura de contratos declarados en el AST.
 */
#include "vx/contracts_collect.h"

namespace vx {

void collect_function_contracts(
    const std::vector<std::unique_ptr<ast::Node>> &decls,
    std::unordered_map<std::string, analyze::FunctionContracts> &out) {
    for (const auto &d : decls) {
        if (!d) continue;
        if (d->kind == ast::NodeKind::NamespaceDecl) {
            collect_function_contracts(
                static_cast<const ast::NamespaceDecl *>(d.get())->decls, out);
            continue;
        }
        if (d->kind == ast::NodeKind::FunctionDecl) {
            const auto *fd = static_cast<const ast::FunctionDecl *>(d.get());
            analyze::FunctionContracts c;
            c.pure = fd->contract_pure;
            c.nothrow = fd->contract_nothrow;
            c.nopanic = fd->contract_nopanic;
            c.alloc_total = fd->contract_alloc;
            c.alloc_partial = fd->contract_alloc_partial;
            c.stack_total = fd->contract_stack;
            c.stack_partial = fd->contract_stack_partial;
            if (c.any()) out[fd->name] = c;
        }
        /* Los metodos de struct y de clase, por UN camino: cada rama solo dice
         * de donde salen y con que nombre de tipo; recogerlos se escribe una
         * vez.  Los templates se saltan -- el porque, en la cabecera --. */
        const std::vector<std::unique_ptr<ast::ClassMethodDecl>> *ms = nullptr;
        const std::string *type_name = nullptr;
        if (d->kind == ast::NodeKind::StructDecl) {
            const auto *sd = static_cast<const ast::StructDecl *>(d.get());
            if (sd->type_params.empty() && !sd->is_specialization) {
                ms = &sd->methods;
                type_name = &sd->name;
            }
        } else if (d->kind == ast::NodeKind::ClassDecl) {
            const auto *cd = static_cast<const ast::ClassDecl *>(d.get());
            if (cd->type_params.empty()) {
                ms = &cd->methods;
                type_name = &cd->name;
            }
        }
        if (ms == nullptr) continue;
        /* Un metodo baja a una `IrFunction` llamada `Tipo__metodo`, asi que se
         * registra con ESA clave -- la que vera el analizador. */
        for (const auto &m : *ms) {
            if (!m) continue;
            analyze::FunctionContracts c;
            c.pure = m->contract_pure;
            c.nothrow = m->contract_nothrow;
            c.nopanic = m->contract_nopanic;
            c.alloc_total = m->contract_alloc;
            c.alloc_partial = m->contract_alloc_partial;
            c.stack_total = m->contract_stack;
            c.stack_partial = m->contract_stack_partial;
            if (c.any()) out[*type_name + "__" + m->name] = c;
        }
    }
}

void collect_type_contracts(
    const std::vector<std::unique_ptr<ast::Node>> &decls,
    std::unordered_map<std::string, analyze::TypeContracts> &out) {
    for (const auto &d : decls) {
        if (!d) continue;
        if (d->kind == ast::NodeKind::NamespaceDecl) {
            collect_type_contracts(
                static_cast<const ast::NamespaceDecl *>(d.get())->decls, out);
            continue;
        }
        /* Los tres agregados declaran lo mismo sobre si mismos: cada rama dice
         * de donde se lee, y el contrato se construye en un solo sitio. */
        const std::string *name = nullptr;
        analyze::TypeContracts c;
        if (d->kind == ast::NodeKind::StructDecl) {
            const auto *sd = static_cast<const ast::StructDecl *>(d.get());
            name = &sd->name;
            c.pod = sd->contract_pod;
            c.no_heap = sd->contract_no_heap;
            c.size = sd->contract_size;
        } else if (d->kind == ast::NodeKind::ClassDecl) {
            const auto *cd = static_cast<const ast::ClassDecl *>(d.get());
            name = &cd->name;
            c.pod = cd->contract_pod;
            c.no_heap = cd->contract_no_heap;
            c.size = cd->contract_size;
        } else if (d->kind == ast::NodeKind::EnumDecl) {
            const auto *ed = static_cast<const ast::EnumDecl *>(d.get());
            name = &ed->name;
            c.pod = ed->contract_pod;
            c.no_heap = ed->contract_no_heap;
            c.size = ed->contract_size;
        }
        if (name != nullptr && c.any()) out[*name] = c;
    }
}

} // namespace vx
