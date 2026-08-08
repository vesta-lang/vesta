/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/facts/alignment.h
 * @brief De cuanto es multiplo un valor: el hecho de la alineacion.
 *
 * Hay preguntas que un programa se hace en EJECUCION teniendo el compilador la
 * respuesta:
 *
 *     if ((((uintptr) dst) & 31) == 0)  memcpy_alineada(...);
 *     else                              memcpy_normal(...);
 *
 * Esa comparacion cuesta una rama en cada llamada, y muchas veces su resultado
 * es el mismo siempre -- porque el destino sale de una reserva que ya garantiza
 * como esta alineada.  Con el hecho, la comparacion se pliega y de las dos
 * ramas queda una sola.
 *
 * Y sirve para lo contrario, que es lo que costo caro: hay instrucciones que
 * EXIGEN su direccion alineada (`movdqa` y compania), y sin poder demostrarlo
 * lo unico que cabe es avisar.  Con esto se puede decidir: o se demuestra, o el
 * aviso pasa a ser un error con su prueba.
 *
 * El valor es siempre una POTENCIA DE DOS, y 1 significa "no se sabe nada" --
 * todo entero es multiplo de 1.  Nunca se devuelve una alineacion que no se
 * pueda justificar: quedarse corto solo pierde una optimizacion, pasarse deja
 * pasar un programa que revienta.
 */

#ifndef ANALYSIS_FACTS_ALIGNMENT_H
#define ANALYSIS_FACTS_ALIGNMENT_H

#include <cstdint>
#include <vector>

#include "ir/ssa_ir.h"

namespace analysis {

/**
 * @struct AlignmentFacts
 * @brief Alineacion demostrable de cada valor de una funcion.
 */
struct AlignmentFacts {
    /// Por valor: la mayor potencia de dos de la que es multiplo con certeza.
    /// 1 = no se sabe nada.
    std::vector<uint32_t> de_valor;

    /// Consulta segura.  Fuera de rango -> 1 (no se sabe nada).
    uint32_t de(ir::IrValueId v) const noexcept {
        return v < de_valor.size() ? de_valor[v] : 1u;
    }

    /// @c true si @p v es multiplo de @p n con certeza.
    bool multiplo_de(ir::IrValueId v, uint32_t n) const noexcept {
        return n != 0 && (de(v) % n) == 0;
    }
};

/**
 * @brief Calcula la alineacion demostrable de los valores de @p fn.
 *
 * Las reglas son las de la aritmetica, no una lista de casos:
 *
 *   - una constante es multiplo de la mayor potencia de dos que la divide;
 *   - una reserva lo es de lo que garantice quien reserva;
 *   - una SUMA es multiplo del maximo comun divisor de sus dos partes -- que
 *     con potencias de dos es la menor de las dos;
 *   - multiplicar o desplazar MULTIPLICA la alineacion;
 *   - un PHI vale lo que su peor rama, porque cualquiera puede darse.
 *
 * @param fn Funcion a examinar.
 * @return Los hechos de alineacion de sus valores.
 */
AlignmentFacts compute_alignment(const ir::IrFunction &fn);

} // namespace analysis

#endif // ANALYSIS_FACTS_ALIGNMENT_H
