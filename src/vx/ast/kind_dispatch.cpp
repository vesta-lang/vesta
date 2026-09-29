/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file kind_dispatch.cpp
 * @brief El aborto ruidoso de los recorridos del arbol (VXE943).
 */

#include "vx/ast/kind_dispatch.h"

#include "vx/diag/diag_catalog.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace vx::ast {

void abort_unexpected_node(const Node &n, const char *family,
                           const char *walker) {
    const std::string msg = vx::diag::format(
        "VXE943", {std::to_string(static_cast<unsigned>(n.kind)), family,
                   walker});
    std::fprintf(stderr, "\n%s\n\n", msg.c_str());
    std::fflush(stderr);
    std::abort();
}

} // namespace vx::ast
