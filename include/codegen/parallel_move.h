/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/parallel_move.h
 * @brief Secuenciador de movimientos PARALELOS (destruccion de PHI),
 *        TARGET-NEUTRAL y compartido por los 3 backends.
 *
 * Un "movimiento paralelo" es un conjunto de copias `dst_i <- src_i` que deben
 * verse como SIMULTANEAS (semantica de la destruccion de PHI: todos los args
 * del predecesor se leen del frame OLD antes de escribir cualquier phi dst).
 * Emitirlas en serie sin cuidado pisa un @c src aun necesitado.  Este modulo
 * calcula el ORDEN seguro y rompe los ciclos con un registro @c scratch; lo
 * consumen tanto el path INTERP (`ir_emitter`, emite `mov rN, rM` de bytecode)
 * como el path VREG (`regalloc_rewrite`, emite `MOp::MOV` de MachineIR).  Antes
 * cada uno reimplementaba este algoritmo por su lado (conocimiento duplicado);
 * ahora la DECISION vive aqui y cada backend solo EMITE los pasos en su idioma.
 *
 * El algoritmo es puro sobre IDENTIDADES de registro (enteros): no conoce
 * x86/arm64/bytecode.  Un @c src NO-registro (memoria/inmediato/derrame) se
 * codifica con un sentinela negativo (nunca "lee" un dst, pero su dst si puede
 * ser leido por otra copia -> participa del orden, no de los ciclos).
 */

#ifndef VESTA_CODEGEN_PARALLEL_MOVE_H
#define VESTA_CODEGEN_PARALLEL_MOVE_H

#include <cstdint>
#include <vector>

namespace codegen {

/**
 * @brief Un paso del movimiento paralelo: `reg[dst] <- <fuente>`.
 *
 * @c dst es siempre un id de registro (>= 0).  @c src:
 *   - `>= 0`: id de registro fuente (incluye el @c scratch tras romper un
 *     ciclo -> el backend emite un mov reg-reg normal).
 *   - `< 0`:  fuente NO-registro; el indice original es `-src - 1`.  El caller
 *     mantiene su propia tabla paralela (MOperand de memoria, inmediato, slot
 *     de derrame...) y la materializa en su idioma.  El secuenciador nunca la
 *     inspecciona: solo sabe que NO lee ningun dst.
 */
struct PMoveStep {
    int dst;
    int src;
};

/**
 * @brief Ordena los movimientos paralelos de @p moves para que ninguna copia
 *        pise un @c src todavia necesitado; rompe ciclos via @p scratch.
 *
 * @param moves  Lista de pares `{dst, src}`.  @c dst = id de registro.  @c src
 *               = id de registro (`>= 0`) o sentinela no-registro (`< 0`, con
 *               indice original `-src - 1`).  Los triviales (`dst == src`) se
 *               descartan.
 * @param scratch  Id de registro reservado para romper ciclos (no debe ser
 *                  ninguno de los @c dst/@c src reales).
 * @return Secuencia de pasos a emitir EN ORDEN; cada uno es `reg[dst] <- src`.
 *         Determinista (mismo input -> mismo output).
 *
 * Redirect-style (mas general que seguir la cadena de una permutacion): en un
 * ciclo, salva un @c dst a @c scratch, reencamina a @c scratch los @c src que
 * lo leen, y emite la copia; asi cubre tanto permutaciones puras como mapas con
 * fuentes no-registro intercaladas.
 */
inline std::vector<PMoveStep>
sequence_parallel_moves(std::vector<PMoveStep> moves, int scratch) {
    std::vector<PMoveStep> out;
    // Descartar triviales (dst == src): son no-ops.
    {
        std::vector<PMoveStep> filtered;
        filtered.reserve(moves.size());
        for (const auto &m : moves)
            if (m.dst != m.src) filtered.push_back(m);
        moves = std::move(filtered);
    }
    const size_t n = moves.size();
    out.reserve(n + 4);
    std::vector<bool> done(n, false);
    size_t remaining = n;

    // True si el dst de moves[i] es leido (como src registro) por otra copia
    // pendiente -> emitir moves[i] ahora pisaria ese valor.
    auto reads_dst = [&](size_t i) -> bool {
        const int d = moves[i].dst;
        for (size_t j = 0; j < n; ++j) {
            if (done[j] || j == i) continue;
            if (moves[j].src == d) return true; // src registro == dst_i
        }
        return false;
    };

    while (remaining > 0) {
        bool progress = false;
        // Fase topologica: emitir las que no introducen RAW.
        for (size_t i = 0; i < n; ++i) {
            if (done[i] || reads_dst(i)) continue;
            out.push_back({moves[i].dst, moves[i].src});
            done[i] = true;
            --remaining;
            progress = true;
        }
        if (progress) continue;
        // Ciclo: salvar un dst a scratch, reencaminar sus lectores, emitir.
        for (size_t i = 0; i < n; ++i) {
            if (done[i]) continue;
            const int d = moves[i].dst;
            out.push_back({scratch, d}); // scratch <- d (valor OLD)
            for (size_t j = 0; j < n; ++j)
                if (!done[j] && moves[j].src == d) moves[j].src = scratch;
            out.push_back({d, moves[i].src}); // d <- src
            done[i] = true;
            --remaining;
            break;
        }
    }
    return out;
}

} // namespace codegen

#endif // VESTA_CODEGEN_PARALLEL_MOVE_H
