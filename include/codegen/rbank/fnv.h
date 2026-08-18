/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/rbank/fnv.h
 * @brief Util de hashing FNV-1a de 64 bits, compartido por quien computa
 * huellas (identidad del problema, del banco, ...).  Sin dependencias.
 */

#ifndef VESTA_CODEGEN_RBANK_FNV_H
#define VESTA_CODEGEN_RBANK_FNV_H

#include <cstdint>

#include "util/fnv.h"

namespace codegen {
namespace rbank {

/* El algoritmo vive ahora en `util/fnv.h`: tambien lo necesita el subsistema de
 * depuracion, y ni tenia sentido que dependiera del asignador de registros ni
 * copiar el codigo.  Estos nombres siguen aqui para que nada de lo que ya lo
 * usaba tenga que cambiar. */
using util::fnv_mix;
using util::kFnvOffset;
using util::kFnvPrime;

} // namespace rbank
} // namespace codegen

#endif // VESTA_CODEGEN_RBANK_FNV_H
