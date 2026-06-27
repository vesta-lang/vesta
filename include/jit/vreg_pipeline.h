/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/vreg_pipeline.h
 * @brief Orquestador del path de registros virtuales (Phase D.7, commit 5c).
 *
 * Encadena selector vreg -> intervalos -> linear-scan -> rewrite (VM_ABI) ->
 * encoder -> code cache -> registro en el JitRegistry.  Es el punto de entrada
 * que @c auto_jit invoca cuando @c VESTA_JIT_VREGS esta activo; si la funcion
 * no es del subset soportado por el selector vreg, devuelve @c nullptr y el
 * caller hace fallback al path de slots.
 */

#ifndef VESTA_JIT_VREG_PIPELINE_H
#define VESTA_JIT_VREG_PIPELINE_H

#include "jit/vreg_select.h" // CallResolver

#include <cstdint>
#include <string>
#include <vector>

namespace ir {
struct IrFunction;
}

namespace jit {

class CodeCache;

/**
 * @struct NativeReloc
 * @brief Relocation sin resolver del codigo nativo de UNA funcion AOT
 *        (Phase AOT.3 Paso 2b-ii).
 *
 * @c vreg_compile_native compila cada funcion de forma aislada; las
 * referencias a otras funciones del modulo (CALL) o a datos de @c .rodata
 * (direccion absoluta) no se pueden resolver hasta el layout final.  Cada
 * @c NativeReloc dice "en el byte @c offset de esta funcion, parchear segun
 * @c kind con la direccion del simbolo @c symbol".  El driver AOT, tras
 * colocar las funciones en @c .text y los datos en @c .rodata, resuelve
 * @c symbol a su direccion y aplica el parche.  Es arch-agnostica.
 */
struct NativeReloc {
    /** @brief Como parchear (espejo de @c MRelocKind, sin acoplar el header
     *  del back-end al driver). */
    enum class Kind : uint8_t {
        CALL_REL32 =
            0, ///< rel32 a una FUNCION (call/jmp) -> el driver lo encola (BFS).
        ABS64 = 1, ///< direccion absoluta 64-bit a un DATO (--no-pie).
        DATA_REL32 =
            2, ///< RIP-relativo a un DATO (.rodata), position-independent.
    };
    Kind kind = Kind::CALL_REL32;
    uint32_t offset = 0; ///< byte offset dentro de los bytes de la funcion
    std::string symbol;  ///< nombre del simbolo referenciado
    int64_t addend = 0;  ///< desplazamiento adicional dentro del simbolo
};

/**
 * @brief Compila @p fn por el path de registros virtuales (VM_ABI) y la
 *        registra en el JitRegistry.
 *
 * @param fn  Funcion IR a compilar.
 * @param cc  Code cache donde alojar el codigo nativo.
 * @return    Puntero al codigo nativo (invocable via @c enter_jit), o
 *            @c nullptr si la funcion no esta soportada por el selector
 *            vreg (el caller debe hacer fallback).
 */
uint8_t *vreg_compile(const ir::IrFunction &fn, CodeCache &cc,
                      const CallResolver &resolve_call = {},
                      const VregEntries &ent = {},
                      const CallResolver &resolve_native = {},
                      const CallResolver &resolve_symbol = {});

/**
 * @brief Compila @p fn por el path vreg en ABI HOST_LEAF y devuelve los
 *        BYTES nativos (Phase AOT.3 Paso 2).
 *
 * A diferencia de @c vreg_compile (VM_ABI: @c ProcessVM* en RBX + runtime
 * entries + escritura del retorno en @c proc->registers.regs[0]), esta
 * variante usa @c AbiKind::HOST_LEAF: argumentos en los @c arg_regs del ABI
 * nativo del host, retorno en RAX, sin asumir un @c ProcessVM* ni runtime
 * entries.  Es la base del codegen AOT: el blob resultante es una funcion C
 * nativa autonoma, lista para escribirse en la seccion @c .text de un
 * ejecutable PE/ELF.
 *
 * No registra nada en el @c JitRegistry ni aloja en un @c CodeCache: solo
 * produce bytes (que el caller coloca donde quiera).  Para el hito minimo
 * (funcion sin CALL ni datos) los bytes son position-independent y no
 * necesitan relocations.
 *
 * @param fn             Funcion IR a compilar.
 * @param resolve_call   Resolver de CALLs a user-fns (vacio en el hito 1).
 * @param ent            Entradas runtime del selector (vacio en BARE puro).
 * @param resolve_native Resolver de CALLN nativas (vacio en el hito 1).
 * @param resolve_symbol Resolver de simbolos del linker (vacio en el hito 1).
 * @param relocs_out     [out, opcional] si != nullptr, se rellena con las
 *                       relocations sin resolver de @p fn (CALL cross-funcion
 *                       y refs a @c .rodata); el driver AOT las aplica tras el
 *                       layout.  Se SOBRESCRIBE (clear + push).
 * @param pic            true (default) = referencias a datos position-
 *                       independent (lea [rip+disp32]); false (--no-pie) =
 *                       absolutas (mov reg,imm64, dependen de base fija).
 * @param target_sysv    ABI del TARGET (no del host): true = System V AMD64
 *                       (ELF/Linux), false = Win64 (PE/Windows).  Determina
 *                       arg_regs + caller/callee-saved -> permite cross-target
 *                       (generar ELF en Windows y PE en Linux).
 * @param mode32         true = codegen x86-32 (modo protegido, kernels): 8 GP
 *                       eax-edi, sin REX, operando 32-bit, regparm(3).  El
 *                       subset es entero de 32-bit (i32/u32/ptr32).
 * @return Bytes nativos del cuerpo de @p fn (ABI HOST_LEAF), o vector vacio
 *         si la funcion no es del subset soportado por el selector vreg.
 */
std::vector<uint8_t> vreg_compile_native(
    const ir::IrFunction &fn, const CallResolver &resolve_call = {},
    const VregEntries &ent = {}, const CallResolver &resolve_native = {},
    const CallResolver &resolve_symbol = {},
    std::vector<NativeReloc> *relocs_out = nullptr, bool pic = true,
    bool target_sysv =
#if defined(_WIN32)
        false
#else
        true
#endif
    ,
    bool mode32 = false, FloatIsa fisa = FloatIsa::SSE2,
    /* Solo-LSP (vista "Godbolt"): si @p emit_line_map es true y
     * @p line_map_out != nullptr, se rellena con la correlacion
     * byte_offset -> source_line del codigo AOT generado.  OFF por defecto
     * -> cero efecto para el resto del proyecto. */
    bool emit_line_map = false,
    std::vector<LineMapEntry> *line_map_out = nullptr,
    /* Solo-LSP: etiquetas internas de bloques inline-asm (byte_offset ->
     * nombre).  Se rellena si emit_line_map y este puntero != nullptr. */
    std::vector<std::pair<uint32_t, std::string>> *asm_labels_out = nullptr,
    /* Phase AOT-GC (Inc 1): stackmaps de raices GC por safepoint (pc_offset
     * relativo a la funcion + slots con GcHandle).  Se rellena si != nullptr.
     * Vacios salvo que el codigo tenga valores GC (gc<T>, Inc 3).  El driver
     * los serializa en la seccion .vexgc_smap para el scan preciso en runtime. */
    std::vector<Stackmap> *stackmaps_out = nullptr);

/**
 * @brief Compila @p fn por el path vreg con un OSR-entry para el loop cuyo
 *        header es @p header_block (on-stack replacement, Phase D.8, 2c).
 *
 * Identico a @c vreg_compile pero (a) NO emite el contador/trigger C1
 * (suprimido en modo OSR) y (b) APPENDEA un bloque OSR-entry que carga el
 * estado del header desde @c proc->osr_buffer y salta al header.  El blob
 * resultante tiene DOS entradas: la normal (offset 0, no usada por el OSR)
 * y la OSR-entry, cuya direccion absoluta se devuelve en @p osr_entry_out.
 *
 * @param fn             Funcion IR (la misma que el C1; recompile plano).
 * @param cc             Code cache.
 * @param resolve_call   Resolver de CALLs a user-fns (igual que el C1).
 * @param ent            Entradas runtime del selector vreg.
 * @param resolve_native Resolver de CALLN nativas.
 * @param resolve_symbol Resolver de simbolos del linker (STR_LIT_ADDR /
 *                       LABEL_ADDR); igual que el C1.
 * @param header_block   MBlock del loop header a reanudar (== IR block id).
 * @param osr_entry_out  [out] direccion absoluta del OSR-entry (o nullptr).
 * @param required_captures  Red de seguridad: VIDs que el C1 capturo.  Si
 *                       != nullptr, el OSR-entry verifica que su live-in sea
 *                       subconjunto; si no, no emite el entry (osr_entry_out
 *                       queda nullptr -> sin swap).  Critico para el C2
 *                       OPTIMIZADO (cuyo live-in puede diferir del C1).
 * @return               Codigo del blob C2 (entrada normal), o nullptr si
 *                       la funcion no es del subset vreg o no se emitio el
 *                       OSR-entry (incl. mismatch del live-in).
 */
uint8_t *
vreg_compile_osr(const ir::IrFunction &fn, CodeCache &cc,
                 const CallResolver &resolve_call, const VregEntries &ent,
                 const CallResolver &resolve_native,
                 const CallResolver &resolve_symbol, uint32_t header_block,
                 uint8_t **osr_entry_out,
                 const std::vector<uint32_t> *required_captures = nullptr);

} // namespace jit

#endif // VESTA_JIT_VREG_PIPELINE_H
