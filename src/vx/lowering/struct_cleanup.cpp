/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file struct_cleanup.cpp
 * @brief El destructor de un struct local, a la salida de su ambito.
 *
 * Lo registraba solo la declaracion sin lista; la de lista construia por otro
 * camino y se lo saltaba.  Vive aqui para que lo usen todas.
 */

#include "vx/lowering.h"
#include "vx/method_names.h"
#include "vx/type_classify.h"

namespace vx {

void Lowering::register_struct_dtor_cleanup(const std::string &var,
                                            ir::IrValueId addr,
                                            const StructLayout &lay,
                                            uint32_t line) {
    if (escaping_locals_.find(var) != escaping_locals_.end()) return;
    if (!struct_has_destructor(lay)) return;
    CleanupAction act;
    act.kind = CleanupAction::Kind::STRUCT_DTOR;
    act.operands = {addr};
    act.source_line = line;
    act.refresh_name = var;
    act.func_name = destructor_symbol(lay.name);
    cleanup_stack_.push_back(std::move(act));
}

} // namespace vx
