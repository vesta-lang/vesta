/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file keystone_asm_backend.h
 * @brief  AS inc.4b: registro del backend de ensamblado Keystone.
 *
 * Declara @c register_keystone_asm_backend, llamado UNA vez por el ejecutable
 * (target @c vm, que enlaza Keystone) para instalar la impl de
 * @c vx::AsmBackend en @c vx::g_asm_backend.  El frontend Vesta no incluye
 * este header ni keystone.h: solo usa la interfaz abstracta.
 */

#ifndef VX_JIT_KEYSTONE_ASM_BACKEND_H
#define VX_JIT_KEYSTONE_ASM_BACKEND_H

namespace jit {

/**
 * @brief Instala una @c KeystoneAsmBackend (singleton) en
 *        @c vx::g_asm_backend.  Idempotente.  Si Keystone no se puede
 *        abrir, @c g_asm_backend queda en @c nullptr (la validacion de
 *        sintaxis en compile-time se omite; GCC valida en port-C).
 */
void register_keystone_asm_backend();

} // namespace jit

#endif // VX_JIT_KEYSTONE_ASM_BACKEND_H
