/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file ir_pattern.h
 * @brief Registro de reconocedores de patrones del optimizador.
 *
 * Un patron es una forma de escribir algo que la maquina sabe hacer mejor: la
 * suma cuyo acarreo se deduce comparando, el bucle que copia byte a byte lo que
 * una sola instruccion mueve entero, la comparacion seguida de salto que la ISA
 * fusiona.  Reconocerlos estaba resuelto pase a pase, cada uno con su propio
 * recorrido del IR y sus propias reglas ad-hoc, y eso tenia dos consecuencias
 * malas: recorrer N veces lo mismo, y que cada patron se equivocase por su
 * cuenta ante variaciones que no cambian nada.
 *
 * Aqui cada patron declara CUATRO cosas separadas, que es el mismo reparto con
 * el que se construye una decision en el asignador de banco ancho:
 *
 *   - @c match     : QUE busca, preguntando por HECHOS del programa (ver
 *                    ir_facts.h) en vez de exigir una forma literal.
 *   - @c legal     : si reescribirlo seria CORRECTO.  Restricciones duras.
 *                    Ojo a la distincion que mas cara sale: "que no haya nada
 *                    entre la suma y su acarreo" NO es de aqui, es del emisor;
 *                    confundirlas dejo el patron del acarreo reconociendo uno
 *                    de cada siete casos.
 *   - @c benefit   : CUANTO gana, para poder ordenar candidatos que se solapan
 *                    y descartar los que no compensan.
 *   - @c rewrite   : la transformacion.
 *
 * Separarlas permite que un mismo recorrido ofrezca candidatos a todos los
 * patrones, y que la decision de aplicar sea comparable entre ellos.
 */

#ifndef VESTA_IR_IR_PATTERN_H
#define VESTA_IR_IR_PATTERN_H

#include "ir/ssa_ir.h"

#include <functional>
#include <string>
#include <vector>

namespace ir {

/**
 * @brief Un sitio del programa donde un patron podria aplicarse.
 *
 * Guarda solo la ubicacion; cada patron interpreta los indices a su manera y
 * recupera lo que necesite de la funcion.
 */
struct PatternMatch {
    IrBlockId block = 0;  ///< Bloque donde empieza.
    size_t index = 0;     ///< Instruccion dentro del bloque.
    size_t aux_block = 0; ///< Segunda ubicacion, si el patron abarca dos.
    size_t aux_index = 0; ///< Idem.
    bool valid = false;   ///< false = no hay patron aqui.
};

/**
 * @brief Cuanto se gana al aplicar un patron.
 *
 * De momento se mide en instrucciones ahorradas, que es lo que se sabe estimar
 * con confianza.  Cuando haya mas dimensiones (presion de registros, tamano de
 * codigo, coste de compilar) se anaden aqui y la comparacion pasa a ser por
 * funcion objetivo, no por un solo numero.
 */
struct PatternBenefit {
    int instructions_saved = 0; ///< Instrucciones que desaparecen.
    bool worth_it() const noexcept { return instructions_saved > 0; }
};

/**
 * @brief Un reconocedor de patrones.
 *
 * Los cuatro campos son independientes a proposito: se puede cambiar QUE se
 * busca sin tocar cuando es legal, y medir cuanto gana sin tocar como se
 * reescribe.
 */
struct Pattern {
    /// Nombre corto, para diagnosticos y para poder desactivarlo por separado.
    std::string name;

    /// Busca el patron a partir de una instruccion concreta.
    std::function<PatternMatch(const IrFunction &, IrBlockId, size_t)> match;

    /// True si reescribir ese sitio conserva el significado del programa.
    std::function<bool(const IrFunction &, const PatternMatch &)> legal;

    /// Cuanto se gana.  Sirve para ordenar candidatos que se pisan.
    std::function<PatternBenefit(const IrFunction &, const PatternMatch &)>
        benefit;

    /// Aplica la transformacion.  Devuelve false si al final no se toco nada.
    std::function<bool(IrFunction &, const PatternMatch &)> rewrite;
};

/**
 * @brief Recorre la funcion una sola vez ofreciendo cada instruccion a todos
 *        los patrones, y aplica los que casan, son legales y compensan.
 *
 * Cuando dos patrones reclaman el mismo sitio gana el de mayor beneficio; a
 * igualdad, el que se registro antes, para que el resultado no dependa del
 * orden en que el compilador recorra nada.
 *
 * @param fn Funcion a transformar.
 * @param patterns Patrones a considerar, en orden de preferencia.
 * @return true si se aplico alguno.
 */
bool ir_apply_patterns(IrFunction &fn, const std::vector<Pattern> &patterns);

} // namespace ir

#endif // VESTA_IR_IR_PATTERN_H
