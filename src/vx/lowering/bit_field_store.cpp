/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file bit_field_store.cpp
 * @brief Escribir un campo de bits: leer la palabra, cambiar sus bits,
 *        guardarla.
 *
 * Lo hacian tres sitios con su propia copia -- la asignacion `s.f = v`, la
 * lista de una declaracion y (sin hacerlo: lo rechazaba) la de un `(T){...}`
 * --.  Es una sola operacion y vive aqui.
 */

#include "vx/lowering.h"

namespace vx {

void Lowering::emit_bit_field_store(ir::IrValueId word_addr,
                                    const StructFieldInfo &f,
                                    ir::IrValueId value, uint32_t line) {
    const ir::IrType ft = ir_type_from_primitive(f.type.kind);
    // 1. La palabra entera, tal como esta.
    const ir::IrValueId v_old = emit_load_typed(word_addr, ft, line);
    // 2. mask = (1 << ancho) - 1, en el tipo de la palabra.
    const uint64_t mask = (f.bit_width == 64)
                              ? UINT64_MAX
                              : ((uint64_t(1) << f.bit_width) - 1);
    const uint64_t inv_mask = ~(mask << f.bit_offset);
    // 3. Los bits del campo, a cero.
    const ir::IrValueId v_clr = emit_ir_binop(
        ir::IrOp::AND, v_old, emit_const(ft, inv_mask, line), ft, line);
    // 4. El valor, recortado a su ancho: lo que no cabe no pisa al vecino.
    const ir::IrValueId v_tr = emit_ir_binop(
        ir::IrOp::AND, value, emit_const(ft, mask, line), ft, line);
    // 5. Llevado a su sitio.
    ir::IrValueId v_sh = v_tr;
    if (f.bit_offset > 0)
        v_sh = emit_ir_binop(ir::IrOp::SHL, v_tr,
                             emit_const(ft, (uint64_t)f.bit_offset, line), ft,
                             line);
    // 6. Juntar y guardar.
    const ir::IrValueId v_new =
        emit_ir_binop(ir::IrOp::OR, v_clr, v_sh, ft, line);
    emit_store_typed(word_addr, v_new, ft, line);
}

} // namespace vx
