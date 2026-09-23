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
 * @par Esto NO es de las syscalls, ni de una arquitectura, ni tiene tope
 * Su alcance es el de la DECLARACION, y por eso aqui no hay ni un numero
 * fijado: ni cuantos argumentos caben, ni cuantos registros tiene el banco, ni
 * cuantos bits ocupa un identificador de registro.  Cada uno de esos numeros
 * es del OBJETIVO -- x86-64 tiene dieciseis generales, arm64 treinta y uno --,
 * y escribirlo aqui convertiria una decision de transporte en un limite del
 * lenguaje.  Los argumentos, su reparto y el banco viajan por puntero y
 * cuenta.
 *
 * @par Quien sabe de una ISA
 * Solo el GENERADOR, que es uno por arquitectura, igual que el resto del
 * codegen (@c CodegenTarget).  Un objetivo sin generador NO se apana: lo dice
 * y no llama, porque llamar sin cumplir la convencion es exactamente el fallo
 * que esto viene a arreglar.  Los NOMBRES de los registros salen de
 * @c asm_phys_reg_name, que es la misma tabla por la que se lee
 * @c register("rXX") en la declaracion: una sola opinion sobre que numero es
 * cada registro, en vez de dos que pueden separarse.
 *
 * @par El reparto se hornea, no se recalcula
 * Que argumento va a que registro lo dice la DECLARACION, que es constante.
 * Asi que el thunk se genera por CONVENCION -- no por cuantos argumentos hay
 * -- y el codigo generado carga cada registro directamente de su hueco del
 * bloque de valores.  No hay ni banco intermedio ni reparto en cada llamada:
 * en el camino caliente no se reserva memoria.
 */
#ifndef JIT_ABI_CALL_THUNK_H
#define JIT_ABI_CALL_THUNK_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace jit {

class CodeCache;

/// Este argumento NO va en registro: va por la pila, segun la plataforma.
/// Fuera del espacio de identificadores de cualquier banco real.
constexpr uint16_t kAbiArgOnStack = 0xFFFFu;

/**
 * @brief Donde va cada argumento: un identificador de registro por posicion.
 *
 * Un array y no bits empaquetados: empaquetar obliga a fijar cuantos bits
 * lleva un identificador, y eso es del objetivo.  Con una ranura por
 * argumento, anadir una arquitectura con otro banco no cambia nada de aqui.
 */
struct AbiArgSlots {
    const uint16_t *at = nullptr; ///< uno por argumento, en orden.
    size_t count = 0;

    /// El destino del argumento @p i, o @ref kAbiArgOnStack si va por pila.
    uint16_t slot_of(size_t i) const {
        return (at != nullptr && i < count) ? at[i] : kAbiArgOnStack;
    }
};

/**
 * @brief Lo que el thunk consume.  Lo arma quien llama.
 *
 * Dos campos, porque el reparto ya va horneado en el codigo generado: a donde
 * saltar, y donde estan los valores.  Un struct con nombre y no dos argumentos
 * sueltos porque el codigo generado direcciona por desplazamiento, asi que el
 * reparto es parte del contrato y tiene que leerse al lado del generador.
 */
struct AbiCallCtx {
    /// A donde saltar.  El thunk lo copia a la pila ANTES de cargar los
    /// registros, porque despues no queda ninguno libre con el que alcanzarlo.
    uint64_t target = 0;
    /// Los valores, uno por argumento y en el orden de la declaracion.  El
    /// thunk lee de aqui el hueco que le toca a cada uno.
    const uint64_t *args = nullptr;
};

/// Lo que el codigo generado espera recibir.
using AbiCallThunkFn = uint64_t (*)(const AbiCallCtx *ctx);

/**
 * @brief Por que no se pudo cumplir una convencion declarada.
 *
 * Un codigo y no una frase: lo que se lee sale del catalogo multi-idioma.  Y
 * el motivo IMPORTA, no basta con "no se pudo" -- "esta arquitectura no tiene
 * generador" lo arregla quien porta, "el registro no existe" lo arregla quien
 * escribio la declaracion, y "no cabe en el cache" no lo arregla ninguno de
 * los dos --.
 */
enum class AbiCallReason : uint8_t {
    Ok = 0,           ///< se genero.
    NoGenerator,      ///< esta ISA no tiene generador de thunk.
    NoAsmBackend,     ///< no hay ensamblador registrado.
    AssembleFailed,   ///< el ensamblador rechazo el texto.
    NoCodeSpace,      ///< no cabe en el cache de codigo.
    RegNotInIsa,      ///< la declaracion pide un registro que no existe aqui.
    RegIsStackPointer ///< la declaracion pide pasar algo en el puntero de pila.
};

/// El codigo del catalogo con el que se le cuenta @p r a quien lo lea.
const char *abi_call_reason_code(AbiCallReason r);

/**
 * @brief La ranura que nombra @p name en el banco del objetivo activo.
 *
 * Vive aqui y no en quien pregunta porque un nombre de registro es de una
 * ISA: `r10` es de x86-64, `x10` de arm64, y el numero que le toca a cada uno
 * lo sabe el GENERADOR.  Quien baja una llamada solo tiene el texto que puso
 * el programador en `register("...")`, y no tiene por que saber de que
 * arquitectura es.
 *
 * @param name Nombre del registro, tal cual se escribio en la declaracion.
 * @param out  Recibe la ranura si el nombre es de este objetivo.
 * @return @c false si el nombre no nombra ningun registro de aqui -- que NO es
 *         lo mismo que "va por la pila", y por eso se distingue.
 */
bool abi_call_slot_of(const std::string &name, uint16_t *out);

/**
 * @brief Construye (o reutiliza) el thunk del objetivo activo para la
 *        convencion @p slots.
 *
 * Uno por CONVENCION: el reparto es constante -- sale de la declaracion --,
 * asi que resolverlo al generar deja el camino de llamada sin trabajo y sin
 * reservas.  Y el marco depende ademas de cuantos van por la pila, que tiene
 * que quedar alineado como pida la ABI en el momento del salto.  Se generan
 * bajo demanda y viven lo que el proceso.
 *
 * @param slots Donde va cada argumento.  Se copia: el thunk le sobrevive.
 * @param why   Si no es nulo, recibe el motivo -- @ref AbiCallReason::Ok
 *              cuando devuelve un thunk.
 * @return El thunk, o @c nullptr si no se pudo generar.
 */
AbiCallThunkFn abi_call_thunk_for(AbiArgSlots slots, AbiCallReason *why);

/**
 * @brief Da de alta @c vrt:call_with_abi, por el que el bytecode llega aqui.
 *
 * Se llama una vez al arranque, junto a @c register_naked_fnaddr_runner.
 */
void register_abi_call_runner();

} // namespace jit

#endif // JIT_ABI_CALL_THUNK_H
