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
 * Es la pieza que hace decidible lo que si no habria que callar: una region de
 * tamano SIMBOLICO (`malloc(n)`) no se puede comprobar sin saber cuanto puede
 * valer `n`, y un acceso indexado (`buf[i]`) tampoco sin saber cuanto puede
 * valer `i`.
 *
 * El dominio es un RETICULO de intervalos con tres estados, no dos:
 *
 *     TOP      no se nada del valor
 *     [lo,hi]  esta entre esos dos
 *     BOTTOM   este punto del programa NO SE ALCANZA
 *
 * BOTTOM no es "no se": es "aqui no se llega".  Confundirlos hace que una rama
 * imposible -- `x = 20; if (x < 10)` -- se trate como una rama de la que no se
 * sabe nada, y con eso el analisis pierde justo la conclusion mas fuerte que
 * podia sacar.
 *
 * SEPARADO de "que bits estan a uno" a proposito: aqui se habla del VALOR
 * MATEMATICO (un `i8` vale entre -128 y 127), no de la representacion fisica.
 *
 * INDEPENDIENTE DE ARQUITECTURA: se razona sobre el IR y sobre los anchos de
 * sus tipos.  Ni registros, ni acarreos, ni convenios de llamada; el mismo
 * hecho vale para x86-64, x86-32 y arm64.
 */
#ifndef ANALYSIS_FACTS_VALUE_RANGE_H
#define ANALYSIS_FACTS_VALUE_RANGE_H

#include "analysis/facts/ir_facts.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace ir {
struct IrFunction;
}

namespace analysis {

/// Los tres estados del reticulo.  Ver la nota del fichero: BOTTOM y TOP NO son
/// lo mismo, y tratarlos igual es el fallo clasico de estos analisis.
enum class RangeKind : uint8_t { Bottom, Bounded, Top };

/**
 * @brief Intervalo cerrado [lo, hi], o TOP, o BOTTOM.
 *
 * Toda la aritmetica del dominio vive aqui como operaciones del reticulo
 * (`unir`, `cortar`, `ensanchar`), en vez de repartida por el motor.  Asi el
 * motor decide QUE compone y el dominio decide COMO se compone.
 */
struct ValueRange {
    RangeKind kind = RangeKind::Top;
    int64_t   lo = 0;
    int64_t   hi = 0;

    static ValueRange top() { return ValueRange{RangeKind::Top, 0, 0}; }
    static ValueRange bottom() { return ValueRange{RangeKind::Bottom, 0, 0}; }
    /// Un intervalo vacio ES bottom: no hay valor que lo cumpla.
    static ValueRange acotado(int64_t l, int64_t h) {
        if (l > h) return bottom();
        return ValueRange{RangeKind::Bounded, l, h};
    }
    static ValueRange constante(int64_t v) { return acotado(v, v); }

    bool es_top() const { return kind == RangeKind::Top; }
    bool es_bottom() const { return kind == RangeKind::Bottom; }
    /// Hay un intervalo concreto que afirmar (lo unico sobre lo que se prueba).
    bool acotada() const { return kind == RangeKind::Bounded; }

    bool operator==(const ValueRange &o) const {
        if (kind != o.kind) return false;
        return kind != RangeKind::Bounded || (lo == o.lo && hi == o.hi);
    }

    /// UNION: lo que puede valer si viene por cualquiera de dos caminos.
    /// `bottom` es el neutro (ese camino no aporta valores); `top` absorbe.
    ValueRange unir(const ValueRange &o) const {
        if (es_bottom()) return o;
        if (o.es_bottom()) return *this;
        if (es_top() || o.es_top()) return top();
        return acotado(std::min(lo, o.lo), std::max(hi, o.hi));
    }

    /// CORTE: lo que cumple las dos afirmaciones a la vez sobre el MISMO punto.
    /// Si no queda nada, el punto no se alcanza -> bottom (no "no se").
    ValueRange cortar(const ValueRange &o) const {
        if (es_bottom() || o.es_bottom()) return bottom();
        if (es_top()) return o;
        if (o.es_top()) return *this;
        return acotado(std::max(lo, o.lo), std::min(hi, o.hi));
    }

    /**
     * @brief ENSANCHAMIENTO: el extremo que crece se suelta hasta su limite.
     *
     * Es lo que hace que el analisis TERMINE.  Se suelta SOLO el extremo que se
     * movio -- soltar el intervalo entero tira tambien lo que no habia cambiado,
     * y en `for (i = 100; i < 200)` lo que no cambia es la cota inferior, que es
     * justo lo que permite demostrar algo.  Ensanchar no es olvidar.
     *
     * @param nuevo  el valor recien calculado (este objeto es el anterior).
     * @param suelo  hasta donde soltar; si no acota, se suelta al infinito.
     */
    ValueRange ensanchar(const ValueRange &nuevo, const ValueRange &suelo) const {
        if (es_bottom()) return nuevo;
        if (!acotada() || !nuevo.acotada()) return nuevo.es_bottom() ? *this : nuevo;
        int64_t l = nuevo.lo, h = nuevo.hi;
        if (l < lo) l = suelo.acotada() ? suelo.lo : INT64_MIN;
        if (h > hi) h = suelo.acotada() ? suelo.hi : INT64_MAX;
        return acotado(l, h);
    }
};

/* PENDIENTE, y dicho aqui para que no se pierda: un rango simple no basta para
 * `base + i*8`.  Hace falta el PASO ademas del intervalo -- `i en [0,63]` paso 8
 * -> `offset en [0,504]` -- para demostrar `offset + sizeof(T) <= tamano` sin
 * conocer `i`.  Es el salto de limites concretos a simbolicos, y lo que hara
 * comprobables las vistas dinamicas (`@overlay`), cuya geometria se compone de
 * sumas de cotas.
 *
 * Y este hecho se queda ESPECIALIZADO: no debe convertirse en el objeto que
 * guarda todo lo que se sabe de un valor -- rango, bits, procedencia, region,
 * alineacion, no-nulidad viven juntos en el modelo de hechos, no dentro de cada
 * hecho. */

/// Ajustes del analisis.  Los presupuestos son una RED ante un fallo del propio
/// motor, no el mecanismo de terminacion (de eso se encarga el ensanchamiento);
/// van aqui para que los tests puedan medir precision con distintos valores.
struct RangeOptions {
    uint32_t retardo_ensanche = 3;   ///< revisitas de una cabecera antes de soltar.
    uint32_t pasos_por_bloque = 64;  ///< presupuesto = bloques * esto + extra.
    uint32_t pasos_extra = 256;
};

/// Rango de cada valor SSA de una funcion, indexado por value-id.
struct RangeFacts {
    std::vector<ValueRange> r;
    /**
     * @brief Si el calculo llego a PUNTO FIJO o se paro por presupuesto.
     *
     * El tope no significa "analisis terminado".  Significa "hasta aqui he
     * llegado", y la diferencia importa: quien va a DEMOSTRAR algo tiene derecho
     * a saber si lee una conclusion o una parada.  Sin convergencia no se
     * afirma ningun rango.
     */
    bool convergio = true;
    int  vueltas = 0;

    const ValueRange &at(ir::IrValueId v) const {
        static const ValueRange kTop = ValueRange::top();
        return v < r.size() ? r[v] : kTop;
    }
};

/**
 * @brief Calcula los rangos de @p fn, con sensibilidad al FLUJO.
 *
 * Contrato del motor -- tres estados, no uno:
 *
 *     IN[B]              = union de OUT_ARISTA[P -> B]
 *     OUT[B]             = transferencia(B, IN[B])
 *     OUT_ARISTA[P -> S] = cortar(OUT[P], lo que afirma la guarda de esa arista)
 *
 * Las PHI se resuelven leyendo el estado DE LA ARISTA por la que llega cada
 * argumento: sin eso, una guarda no puede afectar a la PHI que depende de ella.
 *
 * El resultado por valor es su rango EN SU PUNTO DE DEFINICION.  Es una
 * proyeccion derivada, no el transporte del analisis: vale en cualquier uso
 * porque en SSA la definicion domina a todos ellos y el valor no cambia -- en un
 * uso concreto el rango solo puede ser mas estrecho (una guarda de por medio),
 * nunca mas ancho.  Quien necesite esa precision extra pregunta por el punto.
 */
RangeFacts compute_ranges(const ir::IrFunction &fn, const IrFacts &facts,
                          const RangeOptions &op = RangeOptions{});

/// Marcador para el AnalysisManager (cachea por funcion; depende de IRFacts).
struct RangeAnalysis {
    using Result = RangeFacts;
    static char ID;
};

} // namespace analysis

#endif // ANALYSIS_FACTS_VALUE_RANGE_H
