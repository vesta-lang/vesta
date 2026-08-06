/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/memory/points_to.h
 * @brief Resolvedor de direcciones COMPARTIDO: para cada valor SSA puntero,
 *        su localizacion abstracta (clase + raiz + offset const).  Es la UNICA
 *        fuente de "a que memoria apunta este puntero" para TODO el compilador
 *        y el tooling: el modelo de efectos (classify_ptr) y las optimizaciones
 *        de memoria (DSE/alias) preguntan aqui, en lugar de tener cada una su
 *        propia resolucion (antes habia dos: la del DSE y la de effects).
 *
 * La resolucion sigue las cadenas de derivacion: ALLOCA/alloc-site/global/param
 * son RAICES; MOV/BITCAST/CAST y equivalentes HEREDAN la raiz; ADD/GEP con
 * offset CONSTANTE acumulan el offset (off_exact); cualquier derivacion con
 * offset NO constante o desconocida degrada a offset inexacto (whole-root) o a
 * Unknown.  Nunca afirma un offset que no puede probar -> sound para el DSE.
 */
#ifndef ANALYSIS_MEMORY_POINTS_TO_H
#define ANALYSIS_MEMORY_POINTS_TO_H

#include "analysis/effects/effects.h" // AbstractLoc
#include "analysis/facts/ir_facts.h"  // IrFacts (def_of, param_of)

#include <cstdint>
#include <vector>

namespace ir {
struct IrFunction;
}

namespace analysis {

/// Resultado de resolver un valor SSA a su localizacion.  Interno al resolvedor;
/// los consumidores usan @c AbstractLoc via @c loc_of.
struct PointsToEntry {
    effects::AbstractLoc::Kind kind = effects::AbstractLoc::Kind::Unknown;
    uint32_t root = effects::LOC_GENERIC; ///< value-id de la raiz (o indice de param).
    int64_t  off = 0;                     ///< offset const desde la raiz.
    bool     off_exact = false;           ///< false = offset no probado (whole-root).
};

/// Tabla points-to de una funcion: value-id -> localizacion resuelta.  O(n).
struct PointsTo {
    std::vector<PointsToEntry> loc; ///< indexada por value-id.
    const PointsToEntry &at(ir::IrValueId v) const {
        static const PointsToEntry kUnknown{};
        return v < loc.size() ? loc[v] : kUnknown;
    }
};

/// Construye la tabla points-to de @p fn usando los hechos @p facts (def-use +
/// param-of).  Resolucion recursiva con memoizacion y guardia de ciclos (PHI).
PointsTo compute_points_to(const ir::IrFunction &fn, const IrFacts &facts);

/// Marcador de analisis para el AnalysisManager (cachea la tabla points-to por
/// funcion).  Depende de IRFactsAnalysis.  El resultado (PointsTo) se invalida
/// cuando la funcion muta (la resolucion raiz+offset cambia con el IR).
struct PointsToAnalysis {
    using Result = PointsTo;
    static char ID;
};

/// Proyecta el valor SSA @p ptr a un @c AbstractLoc con el ancho de acceso
/// @p width (bytes; 0 = desconocido).  Si el offset no es exacto, degrada a
/// whole-root (width 0) para no afirmar bytes concretos que no se probaron.
effects::AbstractLoc loc_of(const PointsTo &pt, ir::IrValueId ptr, int32_t width);

/**
 * @brief Valor que contiene el hueco @p slot, si se puede afirmar cual es.
 *
 * Responde a "¿que hay guardado aqui?" cuando la respuesta es UNA sola y
 * segura: exactamente una escritura al hueco en toda la funcion.  Con dos o
 * mas, el contenido depende de por donde se haya pasado y no se afirma nada.
 *
 * Hace falta para llegar de una variable a lo que apunta.  Un `register("rdi")
 * i64* q` deja el PUNTERO en el registro, pero lo que el compilador tiene a
 * mano es el hueco de la variable; sin esta consulta, un `asm` que escribe en
 * `[rdi]` solo puede decir "escribe en algun sitio".
 *
 * @param fn    Funcion.
 * @param slot  Valor del @c ALLOCA.
 * @return El valor guardado, o @c ir::IR_NO_VALUE si no es unico o no se sabe.
 */
ir::IrValueId valor_unico_del_hueco(const ir::IrFunction &fn,
                                    ir::IrValueId slot);

} // namespace analysis

#endif // ANALYSIS_MEMORY_POINTS_TO_H
