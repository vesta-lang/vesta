/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file value_for_target.cpp
 * @brief Comprobar un valor sabiendo a que destino va.
 *
 * Varias expresiones no tienen tipo propio y lo toman de donde se guardan:
 * `Some(4)` / `Ok(7)` / `Err("x")` de su `Optional` / `Result`, y una lista
 * `{...}` del struct o array que construye.  La declaracion, el `return`, la
 * asignacion y cada elemento de una lista lo repetian cada uno a su manera --
 * y a la lista le faltaba --.  Aqui esta una vez.
 */

#include "vx/type_checker.h"

namespace vx {

Type TypeChecker::check_value_for(ast::Expr *value, const Type &target) {
    const Type saved_optional = expected_optional_type_;
    const Type saved_result = expected_result_type_;
    const Type saved_value = expected_value_type_;
    const ast::Expr *saved_node = expected_value_node_;
    if (target.kind == PrimitiveKind::OPTIONAL)
        expected_optional_type_ = target;
    else if (target.kind == PrimitiveKind::RESULT)
        expected_result_type_ = target;
    expected_value_type_ = target;
    expected_value_node_ = value;
    type_init_list_from_target(value, target);
    const Type t = check_expr(value);
    expected_optional_type_ = saved_optional;
    expected_result_type_ = saved_result;
    expected_value_type_ = saved_value;
    expected_value_node_ = saved_node;
    return t;
}

} // namespace vx
