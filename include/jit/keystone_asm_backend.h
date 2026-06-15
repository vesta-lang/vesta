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
 * @file keystone_asm_backend.h
 * @brief Phase AS inc.4b: registro del backend de ensamblado Keystone.
 *
 * Declara @c register_keystone_asm_backend, llamado UNA vez por el ejecutable
 * (target @c vm, que enlaza Keystone) para instalar la impl de
 * @c vex::AsmBackend en @c vex::g_asm_backend.  El frontend Vex no incluye
 * este header ni keystone.h: solo usa la interfaz abstracta.
 */

#ifndef VEX_JIT_KEYSTONE_ASM_BACKEND_H
#define VEX_JIT_KEYSTONE_ASM_BACKEND_H

namespace jit {

    /**
     * @brief Instala una @c KeystoneAsmBackend (singleton) en
     *        @c vex::g_asm_backend.  Idempotente.  Si Keystone no se puede
     *        abrir, @c g_asm_backend queda en @c nullptr (la validacion de
     *        sintaxis en compile-time se omite; GCC valida en port-C).
     */
    void register_keystone_asm_backend();

} // namespace jit

#endif // VEX_JIT_KEYSTONE_ASM_BACKEND_H
