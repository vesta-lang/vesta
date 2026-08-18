/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file fold.h
 * @brief CTPE -- pase de PLEGADO: ejecuta los candidatos de precomputo en el
 *        ComptimeRuntime (sandbox) e inyecta el resultado escalar como CONST.
 *
 * DESACOPLADO del wiring (opcion C): el pase recibe un @c ComptimeRuntime YA
 * CARGADO con el modulo; el compilador decide como cargarlo.  El pase solo:
 *   1. detecta los candidatos (fn evaluable, zero-param, retorno escalar);
 *   2. por cada uno lo ejecuta en modo CTPE RESTRINGIDO (@c try_invoke_ctpe);
 *   3. reescribe el cuerpo de la funcion a `return CONST(resultado)`.
 *
 * CACHE: el resultado se cachea por HASH del IR del modulo (@c .cache/ctpe/).
 * Si se recompila el mismo programa sin cambios, se reusa el valor precomputado
 * sin volver a ejecutar nada -- determinista: mismo IR -> mismo resultado.
 *
 * Si un candidato aborta (trap del sandbox / timeout / oom), NO se pliega
 * (fallback: la funcion se ejecuta en runtime).  Nunca inyecta un valor
 * incorrecto.
 */
#ifndef CTPE_FOLD_H
#define CTPE_FOLD_H

#include "ir/ssa_ir.h"

// Fwd-decl para no arrastrar todo el header del ComptimeRuntime.
namespace vx {
class ComptimeRuntime;
}

namespace ctpe {

/// Presupuesto del modo CTPE (espejo de ComptimeRuntime::CtpeBudget).
struct FoldBudget {
    uint32_t millis = 3000;
    uint64_t max_heap_bytes = 512ull * 1024 * 1024;
};

/**
 * @brief Ejecuta los candidatos de precomputo del modulo e inyecta los CONST.
 * @param mod    modulo (se modifica: cuerpos plegados).
 * @param rt     ComptimeRuntime YA CARGADO con el modulo (invocaciones
 * in-memory).
 * @param budget presupuesto (tiempo/heap) del modo CTPE.
 * @return numero de funciones plegadas.
 */
int fold(ir::IrModule &mod, vx::ComptimeRuntime &rt,
         const FoldBudget &budget = {});

} // namespace ctpe

#endif // CTPE_FOLD_H
