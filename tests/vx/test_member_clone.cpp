/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/vx/test_member_clone.cpp
 * @brief Comprueba que copiar un miembro lo copia ENTERO, y la sustitucion por
 *        tipos escritos.
 *
 * Las copias sueltas que habia perdian cosas en silencio: `@Override`, la
 * direccion `inout` de un parametro, que un campo fuera `static` o anonimo.
 * Aqui se comprueba justo eso, y que un concepto puede sustituir sus
 * parametros por los tipos TAL COMO SE ESCRIBIERON (antes de que existan los
 * layouts que los resolverian).
 */
#include "vx/generics/field_copy.h"
#include "vx/generics/generic_infer.h"
#include "vx/generics/member_clone.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

int g_failures = 0; ///< Cuantas comprobaciones han fallado.

/**
 * @brief Deja constancia si una condicion no se cumple.
 * @param ok   La condicion.
 * @param what Que se estaba comprobando.
 */
void check(bool ok, const char *what) {
    if (!ok) {
        std::printf("FALLO: %s\n", what);
        ++g_failures;
    }
}

/**
 * @brief Un tipo con nombre, sin argumentos.
 * @param name El nombre.
 * @return El nodo.
 */
std::unique_ptr<vx::ast::TypeNode> named(const char *name) {
    auto n = std::make_unique<vx::ast::NamedTypeNode>();
    n->name = name;
    return n;
}

} // namespace

int main() {
    using namespace vx;

    /* La sustitucion de un concepto `<Self, E>` para `Punto` con `i64`,
     * por tipos escritos. */
    const std::vector<std::string> params = {"Self", "E"};
    auto self_node = named("Punto");
    auto e_node = std::make_unique<ast::PrimitiveTypeNode>();
    e_node->prim = PrimitiveKind::I64;
    const std::vector<const ast::TypeNode *> arg_nodes = {self_node.get(),
                                                          e_node.get()};
    vxgen::GenSubst g;
    g.params = &params;
    g.arg_nodes = &arg_nodes;
    check(g.active(), "la sustitucion por tipos escritos esta activa");

    // Un metodo `@Override E dame(inout Self otro)` con procedencia.
    ast::ClassMethodDecl m;
    m.name = "dame";
    m.return_type = named("E");
    m.is_override = true;
    m.property_kind = 1;
    m.property_name = "valor";
    m.origin.kind = ast::MemberOriginKind::Concept;
    m.origin.via.name = util::InternedName::intern("Da");
    auto p = std::make_unique<ast::ParamDecl>();
    p->name = "otro";
    p->type = named("Self");
    p->dir = ParamDir::InOut;
    p->abi_reg = "rdi";
    m.params.push_back(std::move(p));
    m.body = std::make_unique<ast::BlockStmt>();

    auto c = vxgen::clone_method_with_subst(m, g, vxgen::MethodBodyCopy::Skip);
    check(generics::type_node_text(c->return_type.get()) == "i64",
          "el retorno E pasa a i64");
    check(c->params.size() == 1 &&
              generics::type_node_text(c->params[0]->type.get()) == "Punto",
          "el parametro Self pasa a Punto");
    check(c->params[0]->dir == ParamDir::InOut, "el `inout` se conserva");
    check(c->params[0]->abi_reg == "rdi", "el registro ABI se conserva");
    check(c->is_override, "`@Override` se conserva");
    check(c->property_kind == 1 && c->property_name == "valor",
          "la propiedad se conserva");
    check(c->origin.kind == ast::MemberOriginKind::Concept &&
              c->origin.via.name.str() == "Da" && c->origin.received(),
          "la procedencia se conserva");
    check(c->body == nullptr, "sin cuerpo si no se pide");
    check(vxgen::clone_method_with_subst(m, g, vxgen::MethodBodyCopy::Clone)
                  ->body != nullptr,
          "con cuerpo si se pide");

    // Un campo `static E total` anonimo.
    ast::StructFieldDecl f;
    f.name = "total";
    f.type = named("E");
    f.is_static = true;
    f.is_anonymous = true;
    f.dir = ParamDir::In;
    const ast::StructFieldDecl cf = vxgen::parsed_copy(f, g);
    check(generics::type_node_text(cf.type.get()) == "i64",
          "el campo E pasa a i64");
    check(cf.is_static, "`static` se conserva");
    check(cf.is_anonymous, "anonimo se conserva");
    check(cf.dir == ParamDir::In, "la direccion del campo se conserva");
    check(!cf.origin.received(), "un campo escrito sigue siendo escrito");

    // Un campo de clase con valor por defecto.
    ast::ClassFieldDecl kf;
    kf.name = "n";
    kf.type = named("Self");
    kf.is_final = true;
    kf.lombok_getter = true;
    const ast::ClassFieldDecl ckf = vxgen::parsed_copy(kf, g);
    check(generics::type_node_text(ckf.type.get()) == "Punto",
          "el campo de clase Self pasa a Punto");
    check(ckf.is_final && ckf.lombok_getter,
          "las marcas del campo de clase se conservan");

    if (g_failures == 0) std::printf("test_member_clone: OK\n");
    return g_failures == 0 ? 0 : 1;
}
