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
 *   - los PINES de la convencion de llamada: @c params[i] vive en @c r(i+1)
 *     para i < 12, y a partir de ahi el parametro va DIRECTO a memoria.  No es
 *     una decision del asignador sino una restriccion del ABI de la VM, asi que
 *     pertenece al problema, no a quien lo resuelve.  (En la primera version
 *     del adaptador se dejaron fuera para no alterar lo que se comparaba en el
 *     modo sombra; medido entonces: el modelo derramaba +1 en 12 funciones de
 *     4291, todas con muchos valores vivos a la vez -- justo donde no saber que
 *     un parametro esta clavado obliga a mover otra cosa.)
 */

#ifndef VESTA_CODEGEN_VM_PROBLEM_H
#define VESTA_CODEGEN_VM_PROBLEM_H

#include "codegen/rbank/abstract_problem.h"
#include "codegen/rbank/adapters/liveness_adapter.h"
#include "codegen/vm_isa_facts.h"
#include "ir/liveness.h"
#include "ir/ssa_ir.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace codegen {

/**
 * @brief Construye el @c AbstractProblem del allocator desde la vivacidad del
 *        IR de @p fn.
 *
 * @param fn    funcion SSA (para localizar las llamadas).
 * @param live  vivacidad de @c compute_liveness (dominio IR).
 * @param coalesce_remap  congruencias de PHI (indexado por IrValueId) o
 * nullptr. Con el, los valores de una clase se funden en su root con el
 * intervalo unido -- porque congruentes SON el mismo valor.
 * @return el problema en el dominio IR, listo para @c color_smart_spill.
 *
 * Los @c value_id son @c IrValueId directamente: el modelo los trata como
 * identificadores opacos, asi que no hace falta renumerar y el resultado se
 * puede devolver al emisor sin tabla de traduccion.
 */
inline rbank::AbstractProblem
liveness_to_problem(const ir::IrFunction &fn, const ir::LivenessResult &live,
                    const std::vector<uint32_t> *coalesce_remap = nullptr) {
    /* CANONICALIZACION por congruencia de PHI.  Cuando hay remap, los valores
     * de una misma clase COMPARTEN registro, asi que el problema real tiene un
     * unico valor por clase con el intervalo UNIDO (def = min, end = max).  Si
     * se pasaran los intervalos en crudo, el modelo veria mas valores vivos de
     * los que hay y una presion que no existe -- medido: derramaba +1 en 12
     * funciones, todas con bucles (que es donde hay PHIs).  No es una decision
     * del asignador: es que dos valores congruentes SON el mismo valor. */
    if (coalesce_remap && !coalesce_remap->empty()) {
        const std::vector<uint32_t> &remap = *coalesce_remap;
        auto root = [&](ir::IrValueId v) -> ir::IrValueId {
            return (v != ir::IR_NO_VALUE && v < remap.size()) ? remap[v] : v;
        };
        std::unordered_map<ir::IrValueId, ir::LiveInterval> merged;
        for (const ir::LiveInterval &li : live.intervals) {
            const ir::IrValueId r = root(li.id);
            auto it = merged.find(r);
            if (it == merged.end()) {
                ir::LiveInterval ni = li;
                ni.id = r;
                merged.emplace(r, ni);
            } else {
                it->second.def = std::min(it->second.def, li.def);
                it->second.end = std::max(it->second.end, li.end);
            }
        }
        ir::LivenessResult canon;
        canon.block_start = live.block_start;
        canon.block_end = live.block_end;
        canon.num_instrs = live.num_instrs;
        canon.intervals.reserve(merged.size());
        for (auto &kv : merged)
            canon.intervals.push_back(kv.second);
        std::sort(canon.intervals.begin(), canon.intervals.end(),
                  [](const ir::LiveInterval &a, const ir::LiveInterval &b) {
                      return a.def < b.def || (a.def == b.def && a.id < b.id);
                  });
        return liveness_to_problem(fn, canon, nullptr); // ya canonico
    }

    rbank::AbstractProblem p;
    p.values.reserve(live.intervals.size());

    /* Posiciones que DESTRUYEN una lane volatil, UNA vez (no por valor): el
     * adaptador las linealiza en el mismo espacio que def/end, que es lo que
     * hace comparable el @c covers.
     *
     * Una LLAMADA es un caso de esto, no la definicion.  En la VM tambien lo
     * son las instrucciones que dejan su resultado o su estado en R0 sin que
     * R0 sea operando (@c deffield, @c spawn, @c future, @c msgrecv...): un
     * valor colocado ahi y vivo despues se pierde igual que si lo hubiera
     * pisado un retorno.  Quien dice cuales son es @c vm_isa_facts.h -- aqui
     * solo se recogen sus posiciones. */
    std::vector<uint32_t> calls = rbank::collect_call_positions(fn, live);
    for (size_t b = 0; b < fn.blocks.size() && b < live.block_start.size();
         ++b) {
        const uint32_t base = live.block_start[b];
        const std::vector<ir::IrInstr> &ins = fn.blocks[b].instrs;
        for (size_t j = 0; j < ins.size(); ++j)
            if (vm_op_clobbers_ret(ins[j].op) &&
                !rbank::ir_op_is_call(ins[j].op))
                calls.push_back(base + static_cast<uint32_t>(j));
    }
    std::sort(calls.begin(), calls.end());
    calls.erase(std::unique(calls.begin(), calls.end()), calls.end());

    /* Pines del ABI de la VM: params[i] -> r(i+1) hasta 12; del 13 en adelante
     * el parametro NO cabe en registro y vive en memoria.  Se indexa por
     * IrValueId para no buscar dentro del bucle. */
    constexpr size_t kMaxRegParams = 12;
    std::vector<int16_t> pin;       // -1 = sin pin
    std::vector<uint8_t> in_memory; // 1 = parametro que no cabe en registro
    for (size_t i = 0; i < fn.params.size(); ++i) {
        const ir::IrValueId pid = fn.params[i];
        if (pid >= pin.size()) {
            pin.resize(static_cast<size_t>(pid) + 1, -1);
            in_memory.resize(static_cast<size_t>(pid) + 1, 0);
        }
        if (i < kMaxRegParams)
            pin[pid] = static_cast<int16_t>(i + 1);
        else
            in_memory[pid] = 1;
    }

    for (const ir::LiveInterval &iv : live.intervals) {
        if (iv.end < iv.def)
            continue; // intervalo vacio: el allocator lo ignora.
        rbank::AbstractValue av;
        av.value_id = iv.id;
        av.start = iv.def;
        av.end = iv.end; // ya es INCLUSIVO, igual que AbstractValue.
        av.req.value_id = iv.id;
        av.req.cls = rbank::ResourceClass::GP;
        av.req.width = rbank::ViewWidth::W8; // registros de 64 bits.
        av.req.fixed_reg = (iv.id < pin.size()) ? pin[iv.id] : -1;
        if (iv.id < in_memory.size() && in_memory[iv.id])
            av.req.residency = rbank::Residency::MEMORY;
        // crosses_call: lo rellena el adaptador de vivacidad, no este fichero
        // -- asi la respuesta a "por que cruza" es siempre suya.
        rbank::populate_liveness_requirements(av.req, iv, calls);
        p.values.push_back(av);
    }
    return p;
}

} // namespace codegen

#endif // VESTA_CODEGEN_VM_PROBLEM_H
