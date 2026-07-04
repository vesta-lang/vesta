/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file asm_backend.cpp
 * @brief Phase AS inc.4b: definicion del puntero global @c g_asm_backend.
 *
 * El frontend Vesta (vx_lib) define el puntero a @c nullptr.  El ejecutable
 * que enlaza Keystone (target @c vm) registra una @c KeystoneAsmBackend en el
 * arranque.  Asi vx_lib NO depende de Keystone.
 */

#include "vx/asm_backend.h"

namespace vx {
AsmBackend *g_asm_backend = nullptr;
}
