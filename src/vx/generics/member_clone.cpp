/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 */

/**
 * @file member_clone.cpp
 * @brief La copia de un metodo, con o sin su cuerpo.
 *
 * El porque, en @c vx/generics/member_clone.h.
 */

#include "vx/generics/member_clone.h"

#include "vx/generics/field_copy.h"

namespace vx {
namespace vxgen {

std::unique_ptr<ast::ClassMethodDecl>
clone_method_with_subst(const ast::ClassMethodDecl &m, const GenSubst &g,
                        MethodBodyCopy body) {
    auto nm = std::make_unique<ast::ClassMethodDecl>();
    // Todo lo escrito salvo el cuerpo, que solo se clona si se pide.  Los
    // parametros de tipo del METODO (`mezcla<U>`) no los toca @p g: el metodo
    // sigue siendo generico y se instancia en cada llamada.
    copy_parsed(m, *nm, g, &ast::ClassMethodDecl::body);
    if (body == MethodBodyCopy::Clone && m.body)
        nm->body = clone_parsed(*m.body, g);
    return nm;
}

} // namespace vxgen
} // namespace vx
