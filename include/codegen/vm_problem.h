/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/vm_problem.h
 * @brief Adaptador del camino del INTERPRETE al modelo del allocator:
 *        @c ir::LivenessResult -> @c AbstractProblem.
 *
 * Es la pieza que permite que los TRES modos usen el mismo allocator.  El JIT y
 * el AOT entran al modelo por @c intervals_to_problem, que parte de
 * @c jit::IntervalResult; el interprete no tiene MachineIR, asi que entra desde
 * la vivacidad del IR.  Mismo destino, distinta puerta.
 *
 * ------------------------------------------------------------------------
 * DOMINIOS: por que NO se reutiliza @c intervals_to_problem
 *
 * No es una cuestion de comodidad, son ESPACIOS DE POSICIONES DISTINTOS:
 *
 *     dominio IR         1 posicion por instruccion  (@c ir::LinearPos)
 *     dominio MachineIR  2 por instruccion: use=2*gi, def=2*gi+1
 *                                                    (@c codegen::LinearPos)
 *
 * Son tipos FUERTES y separados a proposito -- mezclarlos es un error que el
 * compilador debe rechazar, no algo que se revise a ojo.  Un intervalo del IR
 * interpretado como MachineIR mediria la mitad y produciria interferencias
 * falsas (o peor: perdidas).  Por eso este adaptador construye el problema en
 * SU dominio y no traduce nada.
 *
 * ADAPTADOR FINO (la misma disciplina que @c shadow.h exige a su gemelo): aqui
 * solo se EXTRAEN Facts que el IR ya tiene.  Si empezara a "arreglar" el
 * problema -- redondear intervalos, inventar restricciones, corregir lo que el
 * frontend produjo -- la logica volveria a repartirse entre dos sitios y el
 * modelo dejaria de ser LA representacion del allocator.
 *
 * QUE SE EXTRAE HOY:
 *   - el intervalo [def, end] de cada valor vivo,
 *   - la clase de recurso (GP: la VM no tiene banco FP asignable en este
 *     camino; los ZMM se manejan aparte en el emisor),
 *   - @c crosses_call, via el @c liveness_adapter ya existente.
 *
 * QUE NO se extrae todavia, y por que no es un olvido: los pines de
 * argumento (r1-r12) y el retorno (r0) los pre-asigna hoy el propio
 * @c ir::allocate_regs.  Se anyadiran como @c fixed_reg cuando el modelo
 * SUSTITUYA a ese allocator, no antes -- mientras se corre en SOMBRA, meterlos
 * cambiaria lo que se compara.
 */

#ifndef VESTA_CODEGEN_VM_PROBLEM_H
#define VESTA_CODEGEN_VM_PROBLEM_H

#include "codegen/rbank/abstract_problem.h"
#include "codegen/rbank/adapters/liveness_adapter.h"
#include "ir/liveness.h"
#include "ir/ssa_ir.h"

#include <cstdint>
#include <vector>

namespace codegen {

/**
 * @brief Construye el @c AbstractProblem del allocator desde la vivacidad del
 *        IR de @p fn.
 *
 * @param fn    funcion SSA (para localizar las llamadas).
 * @param live  vivacidad de @c compute_liveness (dominio IR).
 * @return el problema en el dominio IR, listo para @c color_smart_spill.
 *
 * Los @c value_id son @c IrValueId directamente: el modelo los trata como
 * identificadores opacos, asi que no hace falta renumerar y el resultado se
 * puede devolver al emisor sin tabla de traduccion.
 */
inline rbank::AbstractProblem
liveness_to_problem(const ir::IrFunction &fn, const ir::LivenessResult &live) {
    rbank::AbstractProblem p;
    p.values.reserve(live.intervals.size());

    // Posiciones de llamada UNA vez (no por valor): el adaptador las linealiza
    // en el mismo espacio que def/end, que es lo que hace comparable el covers.
    const std::vector<uint32_t> calls = rbank::collect_call_positions(fn, live);

    for (const ir::LiveInterval &iv : live.intervals) {
        if (iv.end < iv.def) continue; // intervalo vacio: el allocator lo ignora.
        rbank::AbstractValue av;
        av.value_id = iv.id;
        av.start = iv.def;
        av.end = iv.end; // ya es INCLUSIVO, igual que AbstractValue.
        av.req.value_id = iv.id;
        av.req.cls = rbank::ResourceClass::GP;
        av.req.width = rbank::ViewWidth::W8; // registros de 64 bits.
        av.req.fixed_reg = -1;               // sin pines mientras corre en sombra.
        // crosses_call: lo rellena el adaptador de vivacidad, no este fichero
        // -- asi la respuesta a "por que cruza" es siempre suya.
        rbank::populate_liveness_requirements(av.req, iv, calls);
        p.values.push_back(av);
    }
    return p;
}

} // namespace codegen

#endif // VESTA_CODEGEN_VM_PROBLEM_H
