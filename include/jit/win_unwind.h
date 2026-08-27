/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file jit/win_unwind.h
 * @brief Describirle al sistema el marco del codigo que genera el JIT.
 *
 * POR QUE HACE FALTA.  En Windows de 64 bits el desenrollado de la pila no se
 * hace siguiendo punteros de marco: se hace por TABLAS.  Para cada trozo de
 * codigo el sistema busca una entrada que diga cuanto ocupa su prologo y que
 * guardo; si no la encuentra, da el trozo por hoja y supone que en la cima de
 * la pila esta la direccion de retorno.
 *
 * El codigo que el JIT genera en caliente no esta en ninguna tabla, porque las
 * tablas las trae el ejecutable.  Mientras nadie tenga que desenrollar por
 * encima de el, da igual.  Pero recoger un fallo del procesador ES un
 * desenrollado: el manejador desvia la ejecucion a un punto de recuperacion que
 * hace un salto largo, y en Windows un salto largo desenrolla.  El sistema
 * caminaba la pila, llegaba a un marco del JIT, no encontraba entrada, lo daba
 * por hoja, leia como direccion de retorno lo que hubiera ahi y se llevaba el
 * proceso por delante.
 *
 * El efecto visible: un fallo dentro de codigo compilado mataba el programa con
 * 0xC0000005 y la salida de error VACiA, mientras que el MISMO fallo
 * interpretado salia contado con su codigo, su `fichero:linea` y su cadena de
 * llamadas.  Y como lo que se lee depende de lo que hubiera en la pila, el
 * mismo programa moria o no segun el entorno -- bajo depurador se recuperaba.
 *
 * Es el mismo fallo que ya se cerro para el trampolin de ensamblador
 * (`inline_asm_trampoline.cpp`), donde el prologo es fijo y se describio a
 * mano.  Aqui el prologo cambia por funcion, asi que se describe a partir de lo
 * que anoto quien lo emitio (@c MFunction::UnwindDesc).
 *
 * FUERA DE WINDOWS no hace nada: en System V el desenrollado va por
 * `.eh_frame`, y la recuperacion del interprete no lo necesita.
 */
#ifndef VESTA_JIT_WIN_UNWIND_H
#define VESTA_JIT_WIN_UNWIND_H

#include <cstddef>
#include <cstdint>

namespace jit {

class CodeCache;
struct MFunction;

/**
 * @brief Registra en el sistema como desenrollar una funcion recien generada.
 *
 * @param code  Principio del codigo ya copiado y comiteado.
 * @param bytes Tamano del codigo.
 * @param fn    La funcion, de la que se leen @c unwind y @c prologue_bytes.
 * @param cc    Cache donde alojar la descripcion: el sistema guarda el
 *              PUNTERO, no una copia, asi que tiene que vivir tanto como el
 *              codigo.
 * @return true si quedo registrada.  Un false no rompe nada -- se vuelve al
 *         comportamiento de antes, en el que el fallo no se puede recoger --,
 *         asi que no aborta la compilacion.
 */
bool register_jit_unwind(uint8_t *code, size_t bytes, const MFunction &fn,
                         CodeCache &cc) noexcept;

} // namespace jit

#endif // VESTA_JIT_WIN_UNWIND_H
