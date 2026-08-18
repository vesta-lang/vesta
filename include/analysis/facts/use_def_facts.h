/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/facts/use_def_facts.h
 * @brief UseDefFacts (Tipo A, IR-driven): por cada valor SSA, las POSICIONES de
 *        sus usos en el orden lineal del IR -- el NEXT-USE del NIVEL IR.
 *        query<UseDefFacts>() sobre el FunctionSnapshot, como Liveness /
 *        LoopFacts / RematFacts.
 *
 * NIVEL (leer antes de usarlo).  Este es el next-use del dominio IR: numeracion
 * de @c compute_liveness, indexado por @c IrValueId, posiciones @c
 * ir::LinearPos. Lo consumen analisis y transformaciones SOBRE EL IR -- p.ej.
 * remat (recomputar un valor justo antes de su next-use) o un scheduling a
 * nivel IR.  El ALLOCATOR NO lo consume: trabaja en el dominio MachineIR (2
 * posiciones por instruccion, otra numeracion) y su @c now no pertenece a este
 * espacio.  Para la eleccion de victima (Belady) el allocator usa el hecho
 * HOMOLOGO @c jit::MachineNextUseFacts (jit/interval.h), construido con la
 * numeracion de @c build_intervals.  El tipo fuerte @c ir::LinearPos de la
 * interfaz IMPIDE en compilacion cruzar un @c now del allocator hasta aqui (la
 * regla de nivel movida al sistema de tipos).
 *
 * POR QUE ESTE FACT: "cuando se vuelve a usar cada valor" es conocimiento del
 * IR (los operandos de cada instruccion) que se pierde al bajar a maquina; se
 * captura donde existe.  A nivel IR habilita decisiones IR (remat /
 * scheduling); la eleccion de victima del allocator usa el homologo MachineIR,
 * no esto.
 *
 * COHERENCIA CON EL LIVENESS IR (misma numeracion): este Fact LINEARIZA IGUAL
 * que
 * @c compute_liveness (el liveness del IR, no el del allocator): cada bloque
 * ocupa
 * @c max(1, instrs) posiciones desde @c block_start[b]; el uso de un operando
 * es la posicion de su instruccion; y el uso de un ARGUMENTO PHI se cuenta al
 * FINAL del bloque predecesor (@c block_end[pred]), no en la instruccion PHI --
 * exacto como el liveness IR modela que el valor debe estar vivo al salir del
 * predecesor. Asi @c next_use_after(v, pos) es coherente con el intervalo IR @c
 * [def, end].
 *
 * SEPARACION DE NIVELES (multi-nivel): UseDefFacts es IR-driven (DONDE se usa
 * un valor).  El COSTE de derramar/recargar NO vive aqui -- vive en
 * MachineCostFacts (ASM/uarch).  La DECISION (que victima) FUSIONA next-use
 * (este Fact) con el coste.  El Fact no decide; solo aporta lo que el IR sabe.
 *
 * FORMA CSR (arrays contiguos, no node-based): todas las posiciones viven en UN
 * solo buffer @c use_pos, agrupadas por valor via @c off
 * (compressed-sparse-row).
 * @c next_use_after es una busqueda binaria sobre el segmento ordenado del
 * valor
 * -- cache-friendly y O(log usos), sin fragmentar en un vector por valor.
 *
 * DOS BELADY (mismo concepto, dos dominios) + FACTORIZACION PENDIENTE: este
 * Fact (Belady IR) y @c jit::MachineNextUseFacts (Belady Machine) comparten
 * SOLO el molde CSR de ALMACENAR + CONSULTAR (@c off/@c use_pos + @c
 * next_use_after + sentinela @c invalid()).  Cuando pague, ese molde se
 * factoriza a un
 * @c NextUseTable<ValueId, Position> del que ambos deriven; los PRODUCTORES
 * (@c compute_use_def aqui, @c compute_next_use en jit) NO -- son especificos
 * del nivel.  Detalle en @c jit/interval.h (MachineNextUseFacts).
 *
 * SINERGIA con RematFacts: si un valor es recomputable (RematFacts) y su
 * next-use esta lejos, la DECISION puede preferir RECOMPUTARLO justo antes de
 * ese next-use en vez de reservarle un registro -- los dos Facts se combinan en
 * la politica.
 *
 * i18n: produce DATOS.  ADITIVO.
 */

#ifndef VESTA_ANALYSIS_FACTS_USE_DEF_FACTS_H
#define VESTA_ANALYSIS_FACTS_USE_DEF_FACTS_H

#include "ir/linear_pos.h"
#include "ir/ssa_ir.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace analysis {

/**
 * @struct UseDefFacts
 * @brief Por cada valor SSA, las posiciones (indice lineal, igual que liveness)
 *        de sus usos, ORDENADAS ascendente.  Forma CSR: @c use_pos son todas
 * las posiciones concatenadas; @c off[v]..off[v+1] delimita las de @p v.
 *
 * El asignador lo CONSULTA (no lo posee): la consulta clave es
 * @c next_use_after(v, pos) -- el primer uso de @p v ESTRICTAMENTE despues de
 * @p pos, o @c NO_NEXT_USE si @p v no se vuelve a usar (muerto -> victima
 * ideal).
 */
struct UseDefFacts {
    std::vector<uint32_t>
        off; ///< off[v]..off[v+1] = rango de v en use_pos; size = nvalues+1.
    std::vector<uint32_t>
        use_pos; ///< posiciones de uso, agrupadas por valor y ordenadas.
    uint32_t num_instrs =
        0; ///< total de posiciones lineales (= liveness.num_instrs).

    /// Sentinela: el valor no tiene ningun uso posterior (muerto tras @c pos).
    /// Es el ESTADO invalido de la posicion, no una posicion real.
    static constexpr ir::LinearPos NO_NEXT_USE = ir::LinearPos::invalid();

    /** @brief Numero de valores cubiertos (off tiene un extra al final). */
    uint32_t num_values() const noexcept {
        return off.empty() ? 0u : static_cast<uint32_t>(off.size() - 1);
    }

    /** @brief ¿El valor @p v se usa en algun sitio? */
    bool has_uses(ir::IrValueId v) const noexcept {
        return v < num_values() && off[v + 1] > off[v];
    }

    /**
     * @brief Posicion IR del PROXIMO uso de @p v estrictamente despues de @p
     * pos, o @c NO_NEXT_USE si no hay ninguno (valor muerto).  El tipo fuerte
     *        @c ir::LinearPos garantiza que @p pos pertenece al dominio IR
     *        (no se puede pasar por error un @c now del allocator, MachineIR).
     */
    ir::LinearPos next_use_after(ir::IrValueId v,
                                 ir::LinearPos pos) const noexcept {
        if (v >= num_values()) return NO_NEXT_USE;
        const uint32_t lo = off[v], hi = off[v + 1];
        const uint32_t *b = use_pos.data() + lo;
        const uint32_t *e = use_pos.data() + hi;
        const uint32_t *it =
            std::upper_bound(b, e, pos.value); // primer uso > pos
        return it == e ? NO_NEXT_USE : ir::LinearPos{*it};
    }

    /**
     * @brief Distancia al proximo uso desde @p pos (mayor = mejor victima).
     *        @c 0xFFFFFFFF si no hay proximo uso (distancia infinita).
     */
    uint32_t distance_to_next_use(ir::IrValueId v,
                                  ir::LinearPos pos) const noexcept {
        const ir::LinearPos nu = next_use_after(v, pos);
        return nu.is_valid() ? (nu - pos)
                             : UINT32_MAX; // UINT32_MAX = distancia infinita.
    }
};

/**
 * @brief Produce @c UseDefFacts de una funcion: recolecta las posiciones de uso
 *        de cada valor con la MISMA linealizacion que @c compute_liveness (para
 *        que @c next_use_after sea coherente con los intervalos @c [def, end]).
 *        Funcion PURA (mismo IR -> mismos hechos), IR-driven, sin coste
 * maquina.
 */
inline UseDefFacts compute_use_def(const ir::IrFunction &fn) {
    UseDefFacts f;
    const uint32_t nvalues = static_cast<uint32_t>(fn.values.size());
    const size_t nblocks = fn.blocks.size();

    // --- Linealizacion IDENTICA a compute_liveness: cada bloque ocupa
    //     max(1, instrs) posiciones desde block_start; block_end =
    //     start+span-1. ---
    std::vector<uint32_t> block_start(nblocks, 0), block_end(nblocks, 0);
    uint32_t pos = 0;
    for (size_t b = 0; b < nblocks; ++b) {
        block_start[b] = pos;
        const uint32_t cnt = static_cast<uint32_t>(fn.blocks[b].instrs.size());
        pos += (cnt > 0 ? cnt : 1); // un bloque vacio igual ocupa una posicion.
        block_end[b] = pos - 1;
    }
    f.num_instrs = pos;

    if (nvalues == 0) {
        f.off.assign(1, 0);
        return f;
    }

    // --- Pasada 1: contar usos por valor (para dimensionar el CSR). ---
    std::vector<uint32_t> count(nvalues, 0);
    auto bump = [&](ir::IrValueId v) {
        if (v != ir::IR_NO_VALUE && v < nvalues) ++count[v];
    };
    for (const ir::IrBlock &bb : fn.blocks)
        for (const ir::IrInstr &in : bb.instrs) {
            for (ir::IrValueId op : in.operands)
                bump(op);
            if (in.op == ir::IrOp::CALLIND || in.op == ir::IrOp::CALLCLOSURE)
                bump(in.func_ptr);
            for (const ir::IrPhiArg &pa : in.phi_args)
                bump(pa.value);
        }

    // Offsets = prefix-sum de los conteos.
    f.off.resize(nvalues + 1, 0);
    for (uint32_t v = 0; v < nvalues; ++v)
        f.off[v + 1] = f.off[v] + count[v];
    f.use_pos.resize(f.off[nvalues]);

    // --- Pasada 2: llenar el buffer (un cursor por valor). ---
    std::vector<uint32_t> cur(f.off.begin(),
                              f.off.end() - 1); // cur[v] = off[v].
    auto put = [&](ir::IrValueId v, uint32_t p) {
        if (v != ir::IR_NO_VALUE && v < nvalues) f.use_pos[cur[v]++] = p;
    };
    for (size_t b = 0; b < nblocks; ++b) {
        const ir::IrBlock &bb = fn.blocks[b];
        for (size_t i = 0; i < bb.instrs.size(); ++i) {
            const ir::IrInstr &in = bb.instrs[i];
            const uint32_t ip = block_start[b] + static_cast<uint32_t>(i);
            for (ir::IrValueId op : in.operands)
                put(op, ip);
            if (in.op == ir::IrOp::CALLIND || in.op == ir::IrOp::CALLCLOSURE)
                put(in.func_ptr, ip);
            // Argumento PHI: uso al FINAL del bloque predecesor (como
            // liveness).
            for (const ir::IrPhiArg &pa : in.phi_args) {
                if (pa.value == ir::IR_NO_VALUE) continue;
                const uint32_t pe =
                    (pa.block < static_cast<ir::IrBlockId>(nblocks))
                        ? block_end[pa.block]
                        : ip;
                put(pa.value, pe);
            }
        }
    }

    // --- Ordenar cada segmento: un phi-arg de back-edge apunta a un block_end
    //     ANTERIOR a otros usos ya recolectados, asi que el orden no es
    //     monotono. ---
    for (uint32_t v = 0; v < nvalues; ++v)
        std::sort(f.use_pos.begin() + f.off[v],
                  f.use_pos.begin() + f.off[v + 1]);

    return f;
}

} // namespace analysis

#endif // VESTA_ANALYSIS_FACTS_USE_DEF_FACTS_H
