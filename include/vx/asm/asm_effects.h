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
 * @file asm_effects.h
 * @brief Tabla plana por-arquitectura de efectos de instruccion + inferencia
 *        PROPIA de clobbers para inline asm.
 *
 * Tabla @c mnemonic -> efectos (registros escritos implicitamente, bitmask de
 * operandos escritos, si toca memoria/flags/es call) por arquitectura (x86 y
 * arm64) + @c asm_infer_clobbers que tokeniza un cuerpo NASM Intel e infiere la
 * lista de clobbers que GCC
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

#include <cstdint>
#include <string>
#include <utility>
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
 * @brief Canonicaliza @p raw con los registros de @p arch, sin mirar el
 *        objetivo activo.
 *
 * La version de un solo argumento resuelve la arquitectura por el objetivo que
 * se este compilando, que es lo que quiere casi todo el mundo.  Esta hace falta
 * cuando se analiza un cuerpo de una arquitectura CONCRETA que no tiene por que
 * ser esa -- las variantes por `@Target`, o un test que comprueba las dos --:
 * ahi el arch es un dato del analisis, no del entorno.
 *
 * @param raw  Nombre tal cual (case-insensitive).
 * @param arch `"x86_64"`, `"x86"`, `"arm64"`.
 * @return Nombre canonico, o cadena vacia si no se reconoce en esa
 *         arquitectura.
 */
std::string asm_canonical_reg(const std::string &raw, const std::string &arch);

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
 * @brief Efectos de un mnemonico sobre registros/memoria/flags.
 *
 * Los registros implicitos van en forma CANONICA (rax..r15, vN).
 * @c known=false indica que el mnemonico no esta en la tabla -> el analizador
 * pide clobbers explicitos (conservador).
 */
struct AsmEffects {
    /// Registros escritos que NO aparecen como operandos (p.ej. @c rdtsc
    /// escribe rax:rdx, @c cmpxchg escribe rax implicito).  Canonicos.
    std::vector<std::string> implicit_write;

    /// Registros LEIDOS que NO aparecen como operandos (p.ej. @c syscall lee el
    /// numero de servicio en RAX y los argumentos en RDI/RSI/RDX/R10/R8/R9).
    /// Sin esto el regalloc no sabe que un `register("rdi")` vive HASTA el asm y
    /// lo elimina (DCE) o no coloca el arg en su registro.  Canonicos.
    std::vector<std::string> implicit_read;

    /// Que OPERANDOS escribe la instruccion, como bitmask: bit0=1er operando,
    /// bit1=2o, bit2=3o, ...  Un mask generaliza casos que un solo bool no
    /// cubre: @c xchg escribe los dos (0b011), @c casp/@c ldxp escriben pares,
    /// una CAS escribe el destino.  0 = no escribe ningun operando.
    uint8_t operand_write_mask = 0;

    bool touches_mem = false;   ///< toca memoria implicitamente
    bool touches_flags = false; ///< modifica RFLAGS/condition codes
    bool is_call = false;       ///< call/syscall: clobber de caller-saved
    /// Entrada/salida por PUERTO (`in`/`out`).  No toca memoria, pero es un
    /// efecto observable del exterior: no se puede eliminar ni reordenar como
    /// si fuera aritmetica, y quien la use no es codigo autonomo.  Sin
    /// distinguirla, tabular estas instrucciones las haria parecer PURAS.
    bool port_io = false;
    /// BARRERA de memoria explicita (`mfence`/`lfence`/`sfence`).  No lee ni
    /// escribe nada, pero ORDENA lo que hay a su alrededor: tratarla como una
    /// instruccion cualquiera permitiria justo lo que existe para impedir.
    bool barrier = false;

    /// Registros por los que la instruccion LEE memoria sin decirlo en sus
    /// operandos.  Canonicos.
    ///
    /// Que una instruccion no escriba los corchetes no significa que no se sepa
    /// por donde accede: `movsb` lee por `rsi` y escribe por `rdi` porque lo
    /// dice la arquitectura, igual de fijo que si estuviera escrito.  Ponerlo
    /// aqui es la diferencia entre saberlo y rendirse -- rendirse seria dar por
    /// bueno "toca memoria en algun sitio", que es rodear el analisis en vez de
    /// hacerlo.
    std::vector<std::string> implicit_mem_read;
    /// Registros por los que ESCRIBE memoria implicitamente.  Ver el anterior.
    std::vector<std::string> implicit_mem_write;

    /**
     * Alineacion que la instruccion EXIGE de su direccion de memoria, en
     * bytes.  0 = no exige ninguna.
     *
     * No es una preferencia ni un consejo de rendimiento: `movdqa` sobre una
     * direccion que no es multiplo de 16 no va mas lenta, lanza una excepcion
     * y el programa cae.  Que el efecto de una instruccion incluya lo que
     * PIDE, y no solo lo que hace, es lo que permite comprobarlo en vez de
     * descubrirlo ejecutando -- que es como se descubrio la ultima vez.
     *
     * @c kAlignAnchoOperando significa "tanto como mida su operando
     * vectorial", que es como lo define la arquitectura: la misma `vmovdqa`
     * exige 16 con xmm, 32 con ymm y 64 con zmm.  Ponerlo como un numero fijo
     * seria elegir uno de los tres y equivocarse en los otros dos.
     */
    uint16_t align_req = 0;

    bool known = false;         ///< false si no esta en la tabla
};

/// Valor de @c AsmEffects::align_req que significa "el ancho de su operando
/// vectorial" en vez de un numero concreto.
static constexpr uint16_t kAlignAnchoOperando = 0xFFFFu;

/**
 * @brief Consulta la tabla de efectos de un mnemonico para una arquitectura.
 *
 * @param mnemonic Nombre de la instruccion (cualquier case).
 * @param arch     Arquitectura de destino.  @c "x86_64" / @c "x86" (32-bit) /
 *                 @c "x86_16" comparten la MISMA tabla de efectos (el efecto de
 *                 @c add / @c mov / @c cmp no cambia con el ancho; lo que cambia
 *                 -- ancho de registro, tamano del slot de pila -- se resuelve
 *                 fuera de aqui).  @c "arm64" tiene su propia tabla (LL/SC,
 *                 branches, cset/csel, barreras...).  Un arch no reconocido cae
 *                 a la tabla x86.
 * @return @c AsmEffects con @c known=false si el mnemonico no esta tabulado
 *         para ese arch.
 */
AsmEffects asm_effects_for(const std::string &mnemonic,
                           const std::string &arch);

/**
 * @brief Atajo de @ref asm_effects_for para x86-64 (compat de llamadas viejas).
 */
AsmEffects asm_effects_for(const std::string &mnemonic);

/**
 * @brief Resultado de inferir clobbers de un cuerpo de inline asm.
 */
/**
 * @brief Una instruccion del cuerpo que EXIGE su direccion alineada.
 *
 * Se recoge para poder decirlo.  Una exigencia que el compilador conoce y no
 * comunica no sirve de nada: el programa sigue cayendo en ejecucion, solo que
 * ahora ademas se sabia.
 */
struct AsmAlignReq {
    std::string mnemonic; ///< la instruccion, tal como se escribio.
    std::string operando; ///< el operando de memoria, con sus corchetes.
    /**
     * Alineacion exigida, EN BYTES y ya resuelta.  0 = no se pudo determinar.
     *
     * Sale resuelta a proposito.  Que una instruccion exija tanto como mida su
     * operando -- `vmovdqa` pide 16 con xmm, 32 con ymm y 64 con zmm -- es
     * conocimiento sobre INSTRUCCIONES, y vive donde vive el resto: aqui.
     * Devolver el nombre del operando y que otro dedujera el ancho partiria
     * una sola regla entre dos ficheros, y una regla en dos sitios acaba
     * siendo dos reglas.
     */
    uint16_t bytes = 0;
};

/**
 * @brief Resultado de inferir clobbers de un cuerpo de inline asm.
 */
struct AsmInferResult {
    std::vector<std::string> clobber_regs; ///< nombres GCC-ready (rax.., xmmN)
    bool clobber_memory = false;
    bool clobber_flags = false;
    std::vector<std::string> unknown_mnemonics; ///< para emitir warning
    /// Instrucciones que exigen alineacion.  Vacio = ninguna la pide.
    std::vector<AsmAlignReq> align_reqs;
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

/**
 * @brief Igual que la anterior, sabiendo ademas de que CLASE es cada operando.
 *
 * Hay instrucciones cuyo efecto depende del ancho de su operando -- las que
 * exigen alineacion son el caso claro --, y en el cuerpo el operando se llama
 * como lo bautizo quien escribio el bloque, no `xmm0`.  Con el mapa
 * `nombre -> clase` la inferencia puede resolverlo ella misma en vez de
 * devolver una respuesta a medias para que la complete otro.
 *
 * @param nasm_body Cuerpo NASM Intel.
 * @param bound_canon Registros ya ligados por `register(...)`, canonicos.
 * @param clases_operando Pares (nombre en el cuerpo, clase declarada:
 *        `"reg"`, `"xmm"`, `"ymm"`, `"zmm"`, `"mem"`...).
 * @return El mismo resultado, con las exigencias ya resueltas a bytes.
 */
AsmInferResult asm_infer_clobbers(
    const std::string &nasm_body, const std::vector<std::string> &bound_canon,
    const std::vector<std::pair<std::string, std::string>> &clases_operando);

} // namespace vx

#endif // VX_ASM_EFFECTS_H
