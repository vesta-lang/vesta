/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file jit_branch_prof.cpp
 * @brief Definicion de la tabla de contadores de branch del codigo JIT.
 *
 * Ver @c include/jit/jit_branch_prof.h para el diseno.
 */

#include "util/env_flags.h"
#include "jit/jit_branch_prof.h"

#include <cstdlib>

namespace jit {

// Zero-init estatico: todas las ranuras a {0,0} al arranque.
JitLineCtr g_jit_line_ctrs[kJitLineSlots];

// Flag runtime del guard inline (lo pone main al activar el JIT+PGO).
bool g_jit_tier2_on = false;

bool jit_branch_prof_emit_enabled() {
    // Cacheado (1 getenv).  Por defecto ON cuando el auto-PGO no esta
    // desactivado; VESTA_NO_JIT_PGO=1 lo apaga (mismo escape que el resto del
    // auto-PGO).  El JIT solo emite los contadores si esto es true.
    static const bool on = util::flag_on(util::FlagId::NoJitPgo);
    return on;
}

} // namespace jit
