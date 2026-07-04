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
 * @file asm_effects.h
 * @brief Phase AS inc.4: inferencia PROPIA de clobbers para inline asm.
 *
 * Tabla plana @c mnemonic -> efectos (registros escritos implicitamente,
 * si escribe el primer operando, si toca memoria/flags) + @c asm_infer_clobbers
 * que tokeniza un cuerpo NASM Intel e infiere la lista de clobbers que GCC
 * (port-C) o el JIT (inc.5) deben declarar.  Es 100% propia: NO usa Keystone
 * ni Capstone (la inferencia debe seguir funcionando sin esas libs).  El
 * ENSAMBLADO (texto->bytes) es un eje ortogonal que vive tras la interfaz
 * @c AsmBackend; la inferencia NUNCA lo consume.
 *
 * Tambien expone @c asm_canonical_reg: canonicaliza cualquier alias de ancho
 * (eax/ax/al/ah, r8d/r8w/r8b, xmm/ymm/zmm) a su registro fisico de 64 bits
 * (rax..r15, vN para el banco SIMD).  Usado por el type checker (validacion +
 * deteccion de conflicto same-reg) y por la inferencia (excluir los registros
 * ligados por register() del set de clobbers).
 */

#ifndef VX_ASM_EFFECTS_H
#define VX_ASM_EFFECTS_H

#include <string>
#include <vector>

namespace vx {

/**
 * @brief Canonicaliza un nombre de registro x86-64 a su fisico de 64 bits.
 *
 * @param raw Nombre tal cual (case-insensitive): rax/eax/ax/al/ah,
 *            r8/r8d/r8w/r8b, xmm0/ymm0/zmm0, etc.
 * @return Nombre canonico (rax..r15 para GP; @c "vN" para el banco
 *         vectorial xmm/ymm/zmm), o cadena vacia si no se reconoce.
 */
std::string asm_canonical_reg(const std::string &raw);

/**
 * @brief Normaliza los literales numericos de un cuerpo NASM Intel a hex
 *        explicito (@c 0x...), detectando la base de entrada.
 *
 * El ensamblador (Keystone) interpreta los enteros BARE como HEX (no
 * decimal); para que @c shl rdx, 32 signifique 32 (no 0x32) y para soportar
 * binario/octal, reescribimos cada literal a @c 0x<hex>:
 *   - @c 0x.. / @c 0X..  -> hexadecimal
 *   - @c 0b.. / @c 0B..  -> binario
 *   - @c 0o.. / @c 0O..  -> octal
 *   - resto              -> DECIMAL (convencion NASM)
 * Permite @c _ como separador de digitos.  NO toca identificadores ni
 * nombres de registro (r8/r15/xmm0): un literal es un digito cuyo char
 * previo no es parte de un identificador.
 *
 * @param body Cuerpo NASM Intel.
 * @return Cuerpo con todos los literales en @c 0x<hex>.
 */
std::string asm_normalize_numbers(const std::string &body);

/**
 * @brief Efectos de un mnemonico x86-64 sobre registros/memoria/flags.
 *
 * Los registros van en forma CANONICA (rax..r15, vN).  @c known=false
 * indica que el mnemonico no esta en la tabla -> el analizador pide
 * clobbers explicitos (conservador).
 */
struct AsmEffects {
    std::vector<std::string>
        implicit_write; ///< regs escritos sin aparecer en operandos
    bool writes_first_operand =
        false;                  ///< el 1er operando (si es reg) se escribe
    bool touches_mem = false;   ///< toca memoria implicitamente
    bool touches_flags = false; ///< modifica RFLAGS
    bool is_call = false;       ///< call/syscall: clobber de caller-saved
    bool known = false;         ///< false si no esta en la tabla
};

/**
 * @brief Consulta la tabla de efectos para un mnemonico (cualquier case).
 * @return @c AsmEffects con @c known=false si el mnemonico no esta tabulado.
 */
AsmEffects asm_effects_for(const std::string &mnemonic);

/**
 * @brief Resultado de inferir clobbers de un cuerpo de inline asm.
 */
struct AsmInferResult {
    std::vector<std::string> clobber_regs; ///< nombres GCC-ready (rax.., xmmN)
    bool clobber_memory = false;
    bool clobber_flags = false;
    std::vector<std::string> unknown_mnemonics; ///< para emitir warning
};

/**
 * @brief Infiere los clobbers de un cuerpo NASM Intel.
 *
 * Tokeniza linea a linea (descarta comentarios @c ; y @c //, labels
 * @c name: y prefijos @c lock/rep/...), consulta @c asm_effects_for por
 * mnemonico, UNE los registros escritos (implicitos + 1er operando si la
 * instruccion lo escribe) y EXCLUYE los registros canonicos ligados por
 * @c register() (estan en @p bound_canon: son operandos, no clobbers).
 * @c touches_mem se activa si algun operando tiene @c [...] o el mnemonico
 * toca memoria.  Un @c call/syscall marca clobber conservador de TODO el
 * caller-saved del ABI.  Mnemonicos desconocidos se acumulan en
 * @c unknown_mnemonics para que el caller emita un warning pidiendo
 * clobbers explicitos.
 *
 * @param nasm_body   Cuerpo verbatim del bloque @c asm.
 * @param bound_canon Registros canonicos (rax.., vN) ligados por register().
 * @return Clobbers inferidos (nombres listos para la clobber-list de GCC).
 */
AsmInferResult asm_infer_clobbers(const std::string &nasm_body,
                                  const std::vector<std::string> &bound_canon);

} // namespace vx

#endif // VX_ASM_EFFECTS_H
