/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 */

/**
 * @file vx/ufcs_scoped.h
 * @brief Que se puede llamar sobre un tipo DESDE AQUI.
 *
 * @par La pregunta, y por que es OTRA
 * La identidad de un tipo no cambia con quien la mire: `Punto` tiene los
 * metodos que tiene, y eso lo contesta @c comptime_method_count.  Lo que SI
 * cambia es lo que se le puede llamar por el punto, porque eso depende de las
 * funciones libres que este fichero alcance -- llamada uniforme --.  Son dos
 * preguntas distintas y se responden por separado a proposito; la familia de
 * aqui lleva `scoped` en el nombre para que la respuesta CONFIESE que depende
 * del ambito en vez de dejar que se descubra leyendo la doc.
 *
 * @par Un productor, dos consumidores
 * Lo que sale de aqui es lo MISMO que necesita el `.` del editor.  Producido
 * una vez, el LSP no lo vuelve a descubrir por su cuenta -- el invariante del
 * ASA aplicado a esto: un hecho, un productor --.
 *
 * @par Que NO decide
 * Aqui no se decide que esta en ambito: eso lo sigue diciendo
 * @c ufcs::Index::find, que es quien lo decide al resolver una llamada.  La
 * enumeracion recolecta por cubo y CONFIRMA cada nombre con esa misma
 * funcion.  Si el criterio de alcance se tocara y hubiera una copia aqui, el
 * editor ofreceria por el punto lo que la llamada rechaza.
 */

#ifndef VX_UFCS_SCOPED_H
#define VX_UFCS_SCOPED_H

#include "vx/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vx {

/// Que un metodo alcanzable no viene de ninguna funcion libre.
inline constexpr uint32_t kScopedNoSlot = 0xFFFFFFFFu;

struct ClassMethodInfo;
struct FunctionSig;

/**
 * @struct ScopedMethod
 * @brief Una entrada de lo alcanzable: como se escribe, y DONDE esta su firma.
 *
 * No lleva la firma DENTRO.  Copiar por entrada el tipo de retorno y el vector
 * de parametros seria duplicar, por cada pregunta, lo que el comprobador ya
 * tiene en una tabla -- y quien enumera casi siempre quiere la CUENTA o un
 * nombre, no las firmas --.  Asi que apunta a la suya y los accesores la leen
 * cuando alguien la pide.
 *
 * Que el receptor ocupe el parametro 0 es una regla de la LECTURA, no del
 * almacenamiento: la aplican @c scoped_arity y @c scoped_param_type, que es lo
 * que hace que un metodo real -- donde `this` es implicito y no esta en la
 * lista -- y una funcion libre se lean igual.  Sin esa uniformidad, cualquier
 * consumidor tendria que preguntar de que clase es cada entrada antes de poder
 * leer su firma.
 */
struct ScopedMethod {
    /// Como se escribe tras el punto, sin el prefijo del aplanado.  INTERNADO.
    const std::string *name = nullptr;
    /// De que namespace viene, con puntos.  Nulo = metodo real, o la raiz.
    const std::string *origin = nullptr;
    /// El tipo sobre el que se pregunto: es el parametro 0 de un metodo real.
    Type receiver;
    /// Su firma, si viene de una funcion libre.
    uint32_t slot = kScopedNoSlot;
    /// Su firma, si es un metodo real del tipo.  Excluyente con @c slot.
    const ClassMethodInfo *real = nullptr;
};

class TypeChecker;

/**
 * @brief Cuantos parametros tiene, RECEPTOR INCLUIDO.
 *
 * @param tc El comprobador, de donde sale la firma de una libre.
 * @param m  La entrada.
 * @return Su aridad, o 0 si la firma ya no esta.
 */
[[nodiscard]] uint32_t scoped_arity(const TypeChecker &tc,
                                    const ScopedMethod &m);

/**
 * @brief El tipo de su parametro @p j, y el 0 es SIEMPRE el receptor.
 *
 * @param tc El comprobador.
 * @param m  La entrada.
 * @param j  Cual, contando el receptor como 0.
 * @return Su tipo, o `void` si @p j se pasa de la aridad.
 */
[[nodiscard]] Type scoped_param_type(const TypeChecker &tc,
                                     const ScopedMethod &m, uint32_t j);

/**
 * @brief Lo que devuelve.
 *
 * @param tc El comprobador.
 * @param m  La entrada.
 * @return Su tipo de retorno, o `void` si la firma ya no esta.
 */
[[nodiscard]] Type scoped_return_type(const TypeChecker &tc,
                                      const ScopedMethod &m);

} // namespace vx

#endif // VX_UFCS_SCOPED_H
