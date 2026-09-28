/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vx/lowering/index_operands.cpp
 * @brief Los operandos de un subindice: el indice, y los limites de un rango
 *        normalizados a `[lo, hi)`.
 *
 * Un dueno para la regla que comparten todos los que cortan: cadenas hoy, y
 * arrays, punteros y vistas despues.  Escrita en cada uno, `a..=b` o el limite
 * omitido acabarian significando cosas distintas segun lo que se corte.
 */

#include "vx/lowering.h"

namespace vx {

ir::IrValueId Lowering::lower_expr_as_i64(ast::Expr *ex) {
    const ir::IrValueId v = lower_expr(ex);
    if (v == ir::IR_NO_VALUE) return v;
    return cast_if_needed(v, fn_->values[v].type, ir::IrType::I64,
                          ex->loc.line);
}

bool Lowering::lower_range_bounds(ast::IndexExpr *e, ir::IrValueId v_length,
                                  ir::IrValueId &v_lo, ir::IrValueId &v_hi) {
    const uint32_t line = e->loc.line;
    // Sin inferior (`x[..b]`), desde el principio.
    v_lo = e->index ? lower_expr_as_i64(e->index.get())
                    : emit_const(ir::IrType::I64, 0, line);
    if (v_lo == ir::IR_NO_VALUE) return false;

    if (e->range_hi) {
        v_hi = lower_expr_as_i64(e->range_hi.get());
        if (v_hi == ir::IR_NO_VALUE) return false;
        // `a..=b` incluye b: el superior exclusivo es b + 1.
        if (e->range_inclusive)
            v_hi = emit_ir_binop(ir::IrOp::ADD, v_hi,
                                 emit_const(ir::IrType::I64, 1, line),
                                 ir::IrType::I64, line);
        return true;
    }
    /* Sin superior (`x[a..]`), hasta el final: la longitud la da quien corta,
     * que es quien sabe cuanto mide lo suyo.  `x[a..=]` no llega aqui: lo
     * rechaza el parser (VXP097). */
    v_hi = v_length;
    return v_hi != ir::IR_NO_VALUE;
}

} // namespace vx
