/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/facts/value_range.h
 * @brief Entre que dos numeros esta un valor.  Hecho fundacional, sin dueno.
 *
 * Es la pieza que hace decidible lo que hoy se calla: una region de tamano
 * SIMBOLICO (`malloc(n)`) no se puede comprobar sin saber cuanto puede valer
 * `n`, y un acceso indexado (`buf[i]`) tampoco sin saber cuanto puede valer
 * `i`.  El comprobador de limites ya tiene las regiones; le falta esto.
 *
 * SEPARADO de "que bits estan a uno" a proposito: aqui se habla del VALOR
 * MATEMATICO (un `i8` vale entre -128 y 127), no de la representacion fisica.
 * Mezclarlos obliga a decidir en cada consulta cual de los dos se queria.
 *
 * INDEPENDIENTE DE ARQUITECTURA: se razona sobre el IR y sobre los anchos de
 * sus tipos.  No hay registros, ni acarreos, ni convenios de llamada; el mismo
 * hecho vale para x86-64, x86-32 y arm64.
 */
#ifndef ANALYSIS_FACTS_VALUE_RANGE_H
#define ANALYSIS_FACTS_VALUE_RANGE_H

#include "analysis/facts/ir_facts.h"

#include <cstdint>
#include <vector>

namespace ir {
struct IrFunction;
}

namespace analysis {

/**
 * @brief Intervalo cerrado [lo, hi] en el que vive un valor.
 *
 * @c conocido a false es "no se": NO es "cualquier cosa" ni "esta mal".  Esa
 * distincion es la que evita que un analisis se vuelva un estorbo, asi que
 * viaja dentro del hecho y no la decide quien pregunta.
 */
struct ValueRange {
    int64_t lo = 0;
    int64_t hi = 0;
    bool    conocido = false;

    bool contiene(int64_t v) const { return conocido && v >= lo && v <= hi; }
    /// Cota superior utilizable, o @p si_no cuando no se sabe.
    int64_t tope(int64_t si_no) const { return conocido ? hi : si_no; }
};

/* LIMITE DE ESTE HECHO, a proposito.
 *
 * `ValueRange` es un hecho ESPECIALIZADO y se queda ahi: no debe convertirse en
 * "el objeto que guarda todo lo que se sabe de un valor".  Un mismo valor
 * tendra a la vez rango, bits conocidos, procedencia, region, alineacion,
 * no-nulidad, constancia...  y juntarlos aqui obligaria a que cada consumidor
 * arrastrara todo para preguntar por una cosa.
 *
 * La composicion vive en el modelo de hechos, no dentro de cada hecho.
 *
 * PENDIENTE (A3): un rango simple no basta para `base + i*8`.  Lo que hace
 * falta ahi es el PASO ademas del intervalo -- `i en [0,63]`, `paso 8` ->
 * `offset en [0,504]` -- para poder demostrar `offset + sizeof(T) <= tamano`
 * sin conocer el valor concreto de `i`.  Es el salto de limites concretos a
 * limites simbolicos, y es lo que hara comprobables las vistas dinamicas
 * (`@overlay`), cuya geometria se compone de sumas de cotas: `rex_len en [0,1]`,
 * `op_bytes en [1,2]`, `modrm_off = rex_len + op_bytes`...
 */

/// Rango de cada valor SSA de una funcion, indexado por value-id.
struct RangeFacts {
    std::vector<ValueRange> r;
    /**
     * @brief Si el calculo llego a PUNTO FIJO o se paro por el tope de vueltas.
     *
     * El tope no significa "analisis terminado".  Significa "hasta aqui he
     * llegado", y la diferencia importa: un consumidor que va a DEMOSTRAR algo
     * tiene derecho a saber si lo que lee es una conclusion o una parada.
     *
     * Hoy pararse antes solo cuesta PRECISION y nunca correccion -- cada vuelta
     * unicamente estrecha, y el punto de partida (el ancho del tipo) ya es una
     * sobre-aproximacion valida --, pero el dato viaja igual: en cuanto haya
     * ensanchamiento o uniones, no converger dejara de ser inocuo, y para
     * entonces el hecho ya lo dira en vez de haberlo perdido.
     */
    bool convergio = true;
    int  vueltas = 0; ///< Cuantas hicieron falta (o el tope, si no convergio).

    const ValueRange &at(ir::IrValueId v) const {
        static const ValueRange kNada{};
        return v < r.size() ? r[v] : kNada;
    }
};

/**
 * @brief Calcula los rangos de @p fn.
 *
 * Fuentes, de mas a menos firme: constantes; el ANCHO del tipo (un `u8` no
 * pasa de 255 -- vale para cualquier arquitectura); aritmetica con extremos
 * conocidos; mascaras (`x & 0x0F` no pasa de 15); y las variables de induccion
 * de los bucles, que ya las descubre @c analysis::find_loop_iv -- no se
 * reimplantan aqui.
 *
 * Sin sensibilidad al camino: un rango que solo vale dentro de un `if` no se
 * afirma todavia.  Eso llega con los hechos por camino.
 */
RangeFacts compute_ranges(const ir::IrFunction &fn, const IrFacts &facts);

/// Marcador para el AnalysisManager (cachea por funcion; depende de IRFacts).
struct RangeAnalysis {
    using Result = RangeFacts;
    static char ID;
};

} // namespace analysis

#endif // ANALYSIS_FACTS_VALUE_RANGE_H
