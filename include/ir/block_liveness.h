/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file block_liveness.h
 * @brief Que valores estan vivos a la entrada y a la salida de cada bloque.
 *
 * UN productor para un hecho que antes calculaban por su cuenta varios
 * consumidores -- el coalescing de phis del JIT y la vivacidad del emisor --,
 * cada uno con su contenedor y, lo que es peor, con su propia idea de que es un
 * uso: uno contaba el puntero de funcion de @c CALLCLOSURE y el otro no.
 *
 * Y sin punto fijo.  El calculo clasico -- live_out = U live_in(sucesores),
 * live_in = gen U (live_out - kill), repetido hasta que nada cambia -- recorre
 * en cada vuelta una tabla de BLOQUES POR VALORES, que es cuadratica en el
 * tamano de la funcion: con el `main` de 205.000 instrucciones de un fuente
 * generado era el primer coste del compilador entero.  Aqui se usa que el IR es
 * SSA: cada valor tiene UNA definicion, asi que basta con subir desde cada uso
 * por los predecesores hasta encontrarla, marcando por donde se pasa.  El
 * trabajo es proporcional a lo que se escribe en los conjuntos -- la suma, por
 * valor, de los bloques en los que vive --, que es el tamano de la respuesta.
 *
 * Los conjuntos que salen son EXACTAMENTE los del punto fijo: el menor que lo
 * cumple, con la misma regla para los phi (el argumento vive a la SALIDA del
 * predecesor del que llega; el destino nace en el bloque del phi).
 */

#ifndef VESTA_IR_BLOCK_LIVENESS_H
#define VESTA_IR_BLOCK_LIVENESS_H

#include "ir/ssa_ir.h"
#include "util/named_alloc.h"

#include <cstdint>

namespace ir {

namespace scratch {
struct LiveInOffsets;  ///< Donde empieza la lista de vivos a la entrada.
struct LiveInValues;   ///< Los vivos a la entrada, bloque tras bloque.
struct LiveOutOffsets; ///< Donde empieza la lista de vivos a la salida.
struct LiveOutValues;  ///< Los vivos a la salida, bloque tras bloque.
} // namespace scratch

/**
 * @brief Posicion dentro de una de las listas aplanadas de vivos.
 *
 * Tipo propio para que un desplazamiento de la tabla no se confunda con un
 * identificador de valor ni de bloque, que tambien son de cuatro bytes.
 */
enum LiveSlot : uint32_t {};

/**
 * @brief Vivos a la entrada y a la salida de cada bloque, en forma CSR.
 *
 * La fila de cada bloque sale ORDENADA por identificador de valor: quien la
 * recorra obtiene siempre el mismo orden, compilacion tras compilacion.
 */
struct BlockLiveness {
    util::NamedVector<LiveSlot, scratch::LiveInOffsets> in_offsets;
    util::NamedVector<IrValueId, scratch::LiveInValues> in_values;
    util::NamedVector<LiveSlot, scratch::LiveOutOffsets> out_offsets;
    util::NamedVector<IrValueId, scratch::LiveOutValues> out_values;

    /**
     * @brief Los valores vivos al ENTRAR en un bloque.
     * @param b Bloque, menor que el numero de bloques de la funcion.
     * @return Vista de solo lectura; vale mientras viva este objeto.
     */
    IrValueList live_in(IrBlockId b) const noexcept {
        return IrValueList(in_values.data() + in_offsets[b],
                           in_offsets[b + 1] - in_offsets[b]);
    }

    /**
     * @brief Los valores vivos al SALIR de un bloque.
     * @param b Bloque, menor que el numero de bloques de la funcion.
     * @return Vista de solo lectura; vale mientras viva este objeto.
     */
    IrValueList live_out(IrBlockId b) const noexcept {
        return IrValueList(out_values.data() + out_offsets[b],
                           out_offsets[b + 1] - out_offsets[b]);
    }
};

/**
 * @brief Calcula los vivos por bloque de una funcion en SSA.
 *
 * Un USO es un operando de una instruccion que no sea @c PHI, mas su
 * @c func_ptr si lo lleva.  Los argumentos de un @c PHI se usan al final del
 * predecesor del que llegan, y solo si ese predecesor salta de verdad al
 * bloque del phi.
 *
 * @param fn Funcion a analizar.
 * @return Las dos listas por bloque; vacias si la funcion no tiene bloques.
 */
BlockLiveness compute_block_liveness(const IrFunction &fn);

} // namespace ir

#endif // VESTA_IR_BLOCK_LIVENESS_H
