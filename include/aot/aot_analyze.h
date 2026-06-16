/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 */

/**
 * @file aot/aot_analyze.h
 * @brief Phase AOT.1 -- analisis de compatibilidad nativa (is_pure_native).
 *
 * Clasifica cada @c ir::IrOp en una de tres categorias segun lo que
 * necesita para ejecutarse:
 *
 *   - PURE_NATIVE:        baja 1:1 a instrucciones x86-64 sin runtime ni libc.
 *                         Incluye aritmetica, logica, control de flujo,
 *                         LOAD/STORE, ALLOCA (host stack via @c sub rsp),
 *                         MEMCPY (inline @c rep movsb), inline asm, y CUALQUIER
 *                         llamada resuelta por el linker (CALL/CALLN/CALLIND a
 *                         simbolos del propio programa, del kernel, o libs que
 *                         el usuario enlace).  Nada de esto depende de libc.
 *   - LIBC_MAPPED:        dependencia de libc que el COMPILADOR sintetiza por
 *                         semantica del lenguaje: RAW_ALLOC -> malloc,
 *                         RAW_FREE -> free, PANIC -> fputs+exit.  En
 *                         @c freestanding estas requieren un hook del usuario
 *                         (@AllocatorOverride / @PanicHandler) o se rechazan.
 *   - RUNTIME_DEPENDENT:  necesita libvesta_rt (GC, scheduler, monitores,
 *                         futures, mailboxes, strings GC, reflexion, AOP,
 *                         excepciones, classregistry).
 *
 * = Independencia total (kernels / bootloaders) =
 *   El objetivo de Phase AOT es permitir binarios TOTALMENTE independientes
 *   del runtime y de cualquier libreria.  La memoria de pila (ALLOCA) nunca
 *   pasa por un allocator -- es @c sub rsp puro.  Un programa que solo usa
 *   PURE_NATIVE compila a un .o/.elf/.exe freestanding sin un solo simbolo
 *   externo no resuelto por el propio usuario.
 *
 * = Tiers y freestanding =
 *   - BARE  admite PURE_NATIVE + LIBC_MAPPED.
 *   - BARE + freestanding admite SOLO PURE_NATIVE (la libc se sustituye por
 *     hooks de usuario en AOT.2).
 *   - EMBED admite ademas GC/strings/closures/excepciones/monitores.
 *   - FULL  admite todo (libvesta_rt completo enlazado).
 *
 * Este pase NO genera codigo ni muta el IR: solo decide si un modulo es
 * compilable a nativo en un target dado y, si no, dice por que (con la linea
 * fuente de la op ofensora, igual que un error de compilacion estandar).
 */

#ifndef AOT_AOT_ANALYZE_H
#define AOT_AOT_ANALYZE_H

#include "ir/ssa_ir.h"

#include <cstdint>
#include <string>
#include <vector>

namespace aot {

    /**
     * @brief Tier de compilacion AOT (cuanto runtime se enlaza al binario).
     */
    enum class Tier : uint8_t {
        FULL  = 0,  ///< Runtime completo (libvesta_rt): GC, scheduler, async, distribucion.
        EMBED = 1,  ///< Mini-runtime: GC/strings/excepciones/monitores; sin async/distrib/reflexion.
        BARE  = 2,  ///< Freestanding-capable: PURE_NATIVE (+ libc opcional).  Kernels/drivers/embedded.
    };

    /**
     * @brief Target de compilacion AOT: tier + independencia de libc.
     */
    struct AotTarget {
        Tier tier         = Tier::BARE;  ///< tier (FULL/EMBED/BARE).
        bool freestanding = false;       ///< true => sin libc; RAW_ALLOC/FREE/PANIC
                                         ///< requieren hooks de usuario (@AllocatorOverride/
                                         ///< @PanicHandler).  Solo significativo con BARE.
        /// AOT.2.d: el usuario proporciona el rol via @AllocatorOverride /
        /// @PanicHandler -> la op LIBC_MAPPED correspondiente se admite tambien
        /// en --freestanding (el simbolo lo aporta el usuario, no la libc).
        bool alloc_provided = false;     ///< RAW_ALLOC ok en freestanding.
        bool free_provided  = false;     ///< RAW_FREE / SMARTPTR_FREE ok en freestanding.
        bool panic_provided = false;     ///< PANIC ok en freestanding.
    };

    /**
     * @brief Clase de una operacion IR segun lo que necesita para ejecutarse.
     */
    enum class AotOpClass : uint8_t {
        PURE_NATIVE       = 0,  ///< 1:1 a x86-64; sin runtime ni libc.
        LIBC_MAPPED       = 1,  ///< dependencia de libc sintetizada por el compilador.
        RUNTIME_DEPENDENT = 2,  ///< necesita libvesta_rt.
    };

    /**
     * @brief Clasifica una operacion IR (independiente del target).
     * @param op Operacion a clasificar.
     * @return Categoria PURE_NATIVE / LIBC_MAPPED / RUNTIME_DEPENDENT.
     */
    AotOpClass aot_classify_op(ir::IrOp op) noexcept;

    /**
     * @brief Simbolo de libc que el compilador sintetiza para una op LIBC_MAPPED.
     * @param op Operacion (idealmente LIBC_MAPPED).
     * @return "malloc"/"free"/"exit"/... ; "" si la op no es LIBC_MAPPED.
     */
    const char *aot_libc_symbol(ir::IrOp op) noexcept;

    /**
     * @brief Decide si una operacion es admisible en un target dado.
     *
     *   - PURE_NATIVE:        admisible siempre (incluido freestanding).
     *   - LIBC_MAPPED:        admisible salvo en freestanding (donde requiere
     *                         hook de usuario; AOT.2 lo reescribe).
     *   - RUNTIME_DEPENDENT:  FULL la admite; EMBED solo su subset; BARE no.
     *
     * @param op     Operacion a evaluar.
     * @param target Target de compilacion (tier + freestanding).
     * @return true si la op se puede materializar en ese target.
     */
    bool aot_op_allowed(ir::IrOp op, const AotTarget &target) noexcept;

    /**
     * @brief Razon legible por la que una op no es admisible.
     *
     * Para RUNTIME_DEPENDENT devuelve el subsistema requerido ("GC heap",
     * "dispatch virtual", "strings GC", ...).  Para LIBC_MAPPED en freestanding
     * devuelve la dependencia de libc ("libc malloc", ...).
     *
     * @param op     Operacion.
     * @param target Target (para distinguir el caso libc/freestanding).
     * @return Cadena ASCII; "" si la op es admisible en ese target.
     */
    const char *aot_op_requirement(ir::IrOp op, const AotTarget &target) noexcept;

    /**
     * @brief Una incompatibilidad concreta encontrada en el analisis.
     */
    struct AotIncompat {
        std::string fn_name;      ///< funcion donde aparece la op.
        uint32_t    source_line;  ///< linea fuente (0 = desconocida).
        ir::IrOp    op;           ///< operacion ofensora.
        std::string reason;       ///< subsistema/dependencia requerida + contexto.
    };

    /**
     * @brief Resultado del analisis de un modulo para un target dado.
     */
    struct AotCompatReport {
        AotTarget                target;             ///< target evaluado.
        bool                     compatible = true;  ///< true si NO hay incompatibilidades.
        std::vector<AotIncompat> issues;             ///< incompatibilidades (vacio si compatible).

        /// Nombres de funciones completamente compilables en este target.
        std::vector<std::string> ok_functions;

        /**
         * @brief Renderiza el reporte como texto estilo error de compilacion.
         * @return Cadena multilinea ASCII; vacia si @c compatible.
         */
        std::string render() const;
    };

    /**
     * @brief Analiza un modulo IR completo para un target de compilacion.
     * @param mod    Modulo IR (idealmente ya optimizado).
     * @param target Target objetivo (tier + freestanding).
     * @return Reporte con incompatibilidades y funciones OK.  No muta el IR.
     */
    AotCompatReport aot_analyze_module(const ir::IrModule &mod, const AotTarget &target);

    /**
     * @brief Analiza una sola funcion IR para un target.
     * @param fn         Funcion IR.
     * @param target     Target objetivo.
     * @param out_issues Vector destino (se anyaden entradas, no se limpia).
     * @return true si la funcion es completamente compilable en el target.
     */
    bool aot_analyze_function(const ir::IrFunction &fn, const AotTarget &target,
                              std::vector<AotIncompat> &out_issues);

} // namespace aot

#endif // AOT_AOT_ANALYZE_H
