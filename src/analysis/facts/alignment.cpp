/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file alignment.cpp
 * @brief Implementacion del hecho de alineacion (ver el header para el porque).
 */

#include "analysis/facts/alignment.h"

#include <algorithm>

namespace analysis {

namespace {

/// Tope de la escala.  Mas alla de una linea de cache no hay instruccion que
/// pida mas, asi que seguir subiendo solo daria numeros que nadie usa.
constexpr uint32_t kTope = 64;

/// La mayor potencia de dos que divide a @p k, acotada al tope.  Para 0 se
/// devuelve el tope: el cero es multiplo de todo.
uint32_t potencia_que_divide(uint64_t k) {
    if (k == 0) return kTope;
    uint32_t a = 1;
    while (a < kTope && (k & a) == 0) a <<= 1;
    return ((k & (a - 1)) == 0) ? a : 1;
}

/// Alineacion que garantiza el asignador para un bloque de @p bytes.
///
/// No es una suposicion: es lo que el asignador del lenguaje hace.  Los
/// bloques que pasan por el slab se entregan con la cabecera de 8 delante de
/// un chunk alineado, y los grandes con una de 64 sobre una arena del sistema
/// -- que llega alineada a pagina.  Si esa politica cambia, cambia aqui, y por
/// eso esta en un solo sitio en vez de repartida por quien la aproveche.
uint32_t alineacion_de_reserva(int64_t bytes) {
    if (bytes > 4096) return kTope; // arena directa: cabecera de 64.
    return 8;                       // slab: payload tras 8 bytes de cabecera.
}

} // namespace

AlignmentFacts compute_alignment(const ir::IrFunction &fn) {
    AlignmentFacts f;
    f.de_valor.assign(fn.values.size(), 1u);
    if (fn.blocks.empty()) return f;

    /* Se recorre en el orden de los bloques y se repite hasta que nada cambia.
     * Hace falta por los PHI: la rama que viene del final del bucle define un
     * valor que aun no se ha visto, y con una sola pasada se le daria el peor
     * caso sin razon.  El punto fijo baja siempre -- una alineacion solo se
     * revisa a la baja --, asi que termina. */
    bool cambio = true;
    int vueltas = 0;
    while (cambio && vueltas < 8) {
        cambio = false;
        ++vueltas;
        for (const ir::IrBlock &b : fn.blocks) {
            for (const ir::IrInstr &in : b.instrs) {
                if (in.dst == ir::IR_NO_VALUE || in.dst >= f.de_valor.size())
                    continue;
                uint32_t nueva = 1;
                switch (in.op) {
                case ir::IrOp::CONST:
                    nueva = potencia_que_divide((uint64_t)in.imm);
                    break;
                case ir::IrOp::ALLOCA:
                    // Una reserva de pila la coloca el marco; el minimo que
                    // garantiza cualquier ABI de los que se compilan es 8.
                    nueva = 8;
                    break;
                case ir::IrOp::RAW_ALLOC:
                case ir::IrOp::GC_ALLOC:
                case ir::IrOp::GC_ALLOCP:
                case ir::IrOp::NEWOBJ: {
                    int64_t bytes = 0;
                    if (!in.operands.empty()) {
                        const ir::IrValueId v = in.operands[0];
                        if (v < fn.values.size() && fn.values[v].is_const)
                            bytes = fn.values[v].const_val;
                    }
                    nueva = alineacion_de_reserva(bytes);
                    break;
                }
                case ir::IrOp::MOV:
                case ir::IrOp::BITCAST:
                case ir::IrOp::CAST:
                    // No cambian el valor: heredan su alineacion.
                    if (!in.operands.empty()) nueva = f.de(in.operands[0]);
                    break;
                case ir::IrOp::ADD:
                case ir::IrOp::SUB:
                    /* Sumar dos multiplos de 8 da un multiplo de 8; sumar uno
                     * de 8 y uno de 4 solo garantiza 4.  Con potencias de dos,
                     * el maximo comun divisor es la menor de las dos. */
                    if (in.operands.size() == 2)
                        nueva = std::min(f.de(in.operands[0]),
                                         f.de(in.operands[1]));
                    break;
                case ir::IrOp::MUL: {
                    /* Multiplicar por una constante MULTIPLICA la alineacion:
                     * un multiplo de 8 por 4 es multiplo de 32.  Es lo que
                     * hace que `base + i * 32` se sepa alineado a 32 aunque
                     * `i` sea cualquier cosa. */
                    if (in.operands.size() != 2) break;
                    uint64_t k = 0;
                    ir::IrValueId otro = ir::IR_NO_VALUE;
                    const ir::IrValueId a = in.operands[0], c = in.operands[1];
                    if (c < fn.values.size() && fn.values[c].is_const) {
                        k = (uint64_t)fn.values[c].const_val;
                        otro = a;
                    } else if (a < fn.values.size() && fn.values[a].is_const) {
                        k = (uint64_t)fn.values[a].const_val;
                        otro = c;
                    }
                    if (otro == ir::IR_NO_VALUE) break;
                    const uint64_t prod =
                        (uint64_t)f.de(otro) * potencia_que_divide(k);
                    nueva = (uint32_t)std::min<uint64_t>(prod, kTope);
                    break;
                }
                case ir::IrOp::SHL: {
                    if (in.operands.size() != 2) break;
                    const ir::IrValueId s = in.operands[1];
                    if (s >= fn.values.size() || !fn.values[s].is_const) break;
                    const int64_t sh = fn.values[s].const_val;
                    if (sh < 0 || sh > 6) break;
                    const uint64_t prod = (uint64_t)f.de(in.operands[0])
                                          << (uint64_t)sh;
                    nueva = (uint32_t)std::min<uint64_t>(prod, kTope);
                    break;
                }
                case ir::IrOp::AND: {
                    /* `p & ~(k-1)` alinea a la fuerza: el resultado es
                     * multiplo de k pase lo que pase con `p`.  Es el idioma de
                     * redondear hacia abajo, y reconocerlo aqui es lo que
                     * permite que la cabeza de un tramo alineado se sepa
                     * alineada. */
                    if (in.operands.size() != 2) break;
                    for (int lado = 0; lado < 2; ++lado) {
                        const ir::IrValueId m = in.operands[lado];
                        if (m >= fn.values.size() || !fn.values[m].is_const)
                            continue;
                        const uint64_t mk = (uint64_t)fn.values[m].const_val;
                        // ~(k-1) tiene los bits bajos a cero: cuantos, es la
                        // alineacion que impone.
                        uint32_t a2 = 1;
                        while (a2 < kTope && (mk & a2) == 0) a2 <<= 1;
                        if ((mk & (a2 - 1)) == 0) nueva = std::max(nueva, a2);
                    }
                    break;
                }
                case ir::IrOp::PHI: {
                    /* Vale lo que su PEOR rama: cualquiera puede darse.  Una
                     * rama sin ver todavia no baja el valor -- se resuelve en
                     * la vuelta siguiente del punto fijo. */
                    uint32_t peor = kTope;
                    bool alguna = false;
                    for (const auto &pa : in.phi_args) {
                        if (pa.value >= f.de_valor.size()) continue;
                        peor = std::min(peor, f.de(pa.value));
                        alguna = true;
                    }
                    nueva = alguna ? peor : 1u;
                    break;
                }
                default:
                    nueva = 1;
                    break;
                }
                if (nueva < 1) nueva = 1;
                if (nueva != f.de_valor[in.dst]) {
                    /* Solo a la baja tras la primera vuelta: si subiera, el
                     * punto fijo podria no terminar. */
                    if (vueltas == 1 || nueva < f.de_valor[in.dst]) {
                        f.de_valor[in.dst] = nueva;
                        cambio = true;
                    }
                }
            }
        }
    }
    return f;
}

} // namespace analysis
