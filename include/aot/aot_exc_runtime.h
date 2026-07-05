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
 * @file aot_exc_runtime.h
 * @brief Runtime de excepciones NATIVO para AOT (sin libc, sin GC, sin VM).
 *
 * Provee las primitivas que el codegen AOT usa para try/catch/throw:
 *   - @c __vx_setjmp(buf) -> i64 : salva el estado callee-saved + rsp + rip
 *     en @p buf y devuelve 0 (en el retorno normal) o el valor pasado a
 *     longjmp (en el retorno tras un longjmp).  Es el unico modo de obtener
 *     el "returns twice"; el AOT lo emite como un CALL real seguido de una
 *     rama sobre el resultado.
 *   - @c __vx_longjmp(buf, val) : restaura el estado de @p buf y reanuda
 *     justo despues del @c __vx_setjmp que lo salvo, devolviendo @p val (o 1
 *     si @p val es 0, semantica estandar de longjmp).
 *
 * Implementacion HAND-ROLLED minima-correcta (cero libc).  El layout del
 * buffer depende del ABI del TARGET (SysV vs Win64): SysV salva 6 GP
 * callee-saved (rbx, rbp, r12-r15); Win64 salva 8 (rbx, rbp, rdi, rsi,
 * r12-r15) porque rdi/rsi son callee-saved en Win64.  Ambos anaden rsp + rip.
 *
 * @note v1 entero: NO salva los XMM callee-saved de Win64 (xmm6-15).  Un valor
 *       float vivo a traves de un try en un XMM callee-saved se perderia en
 *       Win64; pendiente v2 (anadir el save/restore de xmm6-15).  En SysV no
 *       hay XMM callee-saved -> ya correcto para float.
 */

#ifndef VESTA_AOT_EXC_RUNTIME_H
#define VESTA_AOT_EXC_RUNTIME_H

#include <cstdint>
#include <vector>

namespace aot {

/// Offset (bytes) de @c rip dentro del buffer, por ABI.  El frame del catch
/// usa este layout; el codegen necesita el tamano total del buffer.
struct ExcBufLayout {
    int rsp_off;    ///< offset de rsp guardado
    int rip_off;    ///< offset de rip guardado
    int total_size; ///< tamano total del buffer (bytes)
};

/// Devuelve el layout del jmp-buffer para el ABI dado (sysv=true -> SysV/ELF,
/// false -> Win64/PE).
ExcBufLayout aot_exc_buf_layout(bool sysv);

/// Bytes x86-64 de @c __vx_setjmp(buf): salva callee-saved + rsp + rip,
/// devuelve 0.  Arg0 = @c buf (rdi en SysV, rcx en Win64).
std::vector<uint8_t> aot_exc_setjmp_bytes(bool sysv);

/// Bytes x86-64 de @c __vx_longjmp(buf, val): restaura el estado y salta al
/// rip guardado, devolviendo val (o 1 si val==0).  Args: buf, val
/// (rdi/rsi en SysV, rcx/rdx en Win64).  No retorna.
std::vector<uint8_t> aot_exc_longjmp_bytes(bool sysv);

} // namespace aot

#endif // VESTA_AOT_EXC_RUNTIME_H
