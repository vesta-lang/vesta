/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file struct_static.cpp
 * @brief El almacen de un campo `static` de struct.
 *
 * Un campo `static` no vive en cada valor: vive UNA vez, en una global con el
 * nombre del struct que lo declara (@c ir::struct_static_symbol).  El campo
 * se queda en el struct -- el comprobador lo registra aparte y no lo cuenta
 * en el layout de instancia --, y la global se crea aqui.
 *
 * Lo creaban dos copias -- una por declarador --, y la del declarador
 * multiple (`static T a, b = 3;`) perdia el valor inicial.
 */

#include "vx/parser.h"

#include "ir/synthetic_symbols.h"
#include "vx/generics/generic_clone.h"

namespace vx {

void Parser::synth_struct_static_global_(const ast::StructDecl &s,
                                         const ast::StructFieldDecl &f) {
    auto gvar = std::make_unique<ast::GlobalVarDecl>();
    gvar->loc = f.loc;
    gvar->name = ir::struct_static_symbol(s.name, f.name);
    gvar->type = vxgen::clone_type_with_subst(f.type.get());
    // Y su valor inicial: sin el, `static i32 nivel = 3;` valia 0 y nadie lo
    // decia.
    if (f.default_init) gvar->init = vxgen::clone_expr(f.default_init.get());
    gvar->is_public = s.is_public;
    pending_before_decls_.push_back(std::move(gvar));
}

} // namespace vx
