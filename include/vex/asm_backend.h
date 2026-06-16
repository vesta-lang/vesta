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
 * @file asm_backend.h
 * @brief Phase AS inc.4b: interfaz @c AsmBackend para ENSAMBLAR inline asm.
 *
 * EJE ORTOGONAL a la inferencia de clobbers (asm_effects.h).  @c AsmBackend
 * cubre SOLO el ensamblado texto->bytes (lo "pesado").  La inferencia es
 * propia y NO consume esta interfaz.
 *
 * Decision de diseno (cerrada): ningun fichero del frontend Vex incluye
 * @c keystone.h directamente.  El unico contacto vive en UN solo .cpp
 * (@c src/jit/keystone_asm_backend.cpp) detras de esta interfaz pura.  El
 * ejecutable que enlaza Keystone (target @c vm) registra una instancia en
 * @c g_asm_backend; el frontend la usa via el puntero (sin dependencia de
 * Keystone).  Swap futuro a un encoder hand-rolled = otra impl de la interfaz.
 *
 * Hoy (inc.4b) se usa para VALIDAR la sintaxis del body en compile-time
 * (errores con la linea Vex).  En inc.5 (JIT) el mismo @c assemble produce
 * los BYTES que van al code-cache.
 */

#ifndef VEX_ASM_BACKEND_H
#define VEX_ASM_BACKEND_H

#include <cstdint>
#include <string>
#include <vector>

namespace vex {

    /// Arquitectura destino del ensamblado.
    enum class AsmArch { X86_64, X86_32, X86_16, ARM64, ARM32 };

    /// Resultado de ensamblar un fragmento de asm.
    struct AsmAssembleResult {
        bool                 ok = false;     ///< true si ensamblo sin errores
        std::vector<uint8_t> bytes;          ///< bytes maquina (inc.5 JIT)
        std::string          error;          ///< mensaje si @c ok==false
        size_t               error_offset = 0; ///< offset aprox en el body del fallo
    };

    /**
     * @brief Interfaz pura: ensambla texto NASM Intel a bytes maquina.
     *
     * Impl1 = @c KeystoneAsmBackend (src/jit/keystone_asm_backend.cpp).  Impl2
     * futuro = encoder hand-rolled del JIT.  La inferencia de clobbers NO usa
     * esta interfaz (vive en asm_effects.{h,cpp}).
     */
    struct AsmBackend {
        virtual ~AsmBackend() = default;
        /**
         * @brief Ensambla @p nasm (sintaxis Intel/NASM) para @p arch.
         * @return Bytes + ok, o ok=false + error si la sintaxis es invalida.
         */
        virtual AsmAssembleResult assemble(const std::string &nasm,
                                           AsmArch arch) = 0;
    };

    /**
     * @brief Backend de ensamblado activo (registrado por el ejecutable que
     *        enlaza Keystone).  @c nullptr si no hay backend disponible (e.g.
     *        tests que invocan el compilador sin pasar por main.cpp); en ese
     *        caso la validacion de sintaxis en compile-time se omite y GCC
     *        valida al compilar el .c (port-C).
     */
    extern AsmBackend *g_asm_backend;

} // namespace vex

#endif // VEX_ASM_BACKEND_H
