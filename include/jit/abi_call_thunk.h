/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file jit/abi_call_thunk.h
 * @brief Llamar a codigo nativo por una convencion DECLARADA, desde el
 *        interprete.
 *
 * Vesta deja escribir la convencion de una funcion: `register("rXX")` fija en
 * que registro entra cada parametro y `...` pasa el resto por la regla de la
 * plataforma.  Los caminos nativos la cumplen -- el sitio de llamada coloca
 * cada valor en su registro y desborda a la pila con el desplazamiento del
 * objetivo --.  El interprete cumplia la MITAD y no lo decia: su puente nativo
 * es una llamada C corriente, asi que los argumentos de pila salen bien pero
 * los registros fijos no -- el primero acaba en el registro que diga la ABI de
 * la plataforma, no en el que pidio la declaracion --.
 *
 * Media convencion no da un error: da OTRO RESULTADO.  Una syscall invocada
 * con el numero en el registro equivocado devuelve un codigo cualquiera, y
 * para el nucleo NT el cero es @c STATUS_SUCCESS.
 *
 * ESTO NO ES DE LAS SYSCALLS.  Su alcance es el de la declaracion: cualquier
 * subconjunto de los parametros fijado a cualquiera de los registros
 * generales, y el resto por la plataforma.  Sirve igual para la convencion de
 * Linux (siete fijos), la de NT (cinco) o la que se invente quien escriba una
 * rutina suya en ensamblador.
 */
#ifndef JIT_ABI_CALL_THUNK_H
#define JIT_ABI_CALL_THUNK_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace jit {

class CodeCache;

/**
 * @brief Cuantos argumentos como mucho.
 *
 * Doce, que es lo que ya admite el puente nativo del interprete y cubre la
 * syscall mas ancha de NT (`NtCreateFile`, once mas el numero de servicio).
 */
constexpr size_t kAbiCallMaxArgs = 12;

/**
 * @brief Donde va cada argumento, empaquetado.
 *
 * CINCO bits por argumento: el identificador fisico del registro (0-15), o
 * @ref kAbiArgOnStack para decir que va por la pila.  Cinco y no cuatro
 * porque "por la pila" tiene que poder DECIRSE: con cuatro habria que
 * deducirlo de la posicion, y entonces una convencion que fije el sexto
 * argumento y deje sueltos el segundo y el tercero no se podria escribir.
 */
using AbiArgDesc = uint64_t;

/// Bits por argumento en @ref AbiArgDesc.
constexpr unsigned kAbiArgBits = 5;
/// Este argumento NO va en registro: va por la pila, segun la plataforma.
constexpr unsigned kAbiArgOnStack = 0x1Fu;

/// El destino del argumento @p i, tal y como lo lleva @p desc.
inline unsigned abi_arg_slot(AbiArgDesc desc, size_t i) {
    return static_cast<unsigned>((desc >> (i * kAbiArgBits)) &
                                 ((1u << kAbiArgBits) - 1u));
}

/**
 * @brief Contexto que el thunk consume.  Lo arma quien llama.
 *
 * Un struct con nombre y no tres punteros sueltos: lo que el thunk lee esta
 * PUESTO en un sitio concreto y el codigo generado direcciona por
 * desplazamiento, asi que el reparto es parte del contrato y tiene que poder
 * leerse al lado del generador.
 */
struct AbiCallCtx {
    /// Valor de cada registro general, por identificador fisico.  El indice 4
    /// (rsp) no se usa: el thunk no pisa el puntero de pila.
    uint64_t gp[16] = {0};
    /// A donde saltar.  Se copia a la pila ANTES de cargar los registros,
    /// porque despues no queda ninguno libre con el que alcanzarlo.
    uint64_t target = 0;
    /// Los que van por la pila, en orden.
    uint64_t stack[kAbiCallMaxArgs] = {0};
};

/// Lo que el codigo generado espera recibir.
using AbiCallThunkFn = uint64_t (*)(const AbiCallCtx *ctx);

/**
 * @brief Construye (o reutiliza) el thunk para @p n_stack argumentos de pila.
 *
 * Uno por CUENTA y no uno generico con bucle: el marco depende de cuantos hay
 * -- y tiene que quedar alineado a 16 en el momento del salto --, asi que
 * calcularlo al generar sale mas corto y mas facil de leer que calcularlo al
 * ejecutar.  Son ocho como mucho y se generan una vez en la vida del proceso.
 *
 * @param n_stack Cuantos argumentos van por la pila.
 * @param err     Si no es nulo, recibe el motivo cuando devuelve nulo.
 * @return El thunk, o @c nullptr si no se pudo generar.
 */
AbiCallThunkFn abi_call_thunk_for(size_t n_stack, std::string *err);

/**
 * @brief Da de alta @c vrt:call_with_abi, por el que el bytecode llega aqui.
 *
 * Se llama una vez al arranque, junto a @c register_naked_fnaddr_runner.
 */
void register_abi_call_runner();

} // namespace jit

#endif // JIT_ABI_CALL_THUNK_H
