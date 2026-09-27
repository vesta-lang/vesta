/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file block_order.h
 * @brief El orden FISICO de los bloques de una funcion tras cirugia de CFG.
 */

#ifndef IR_PASSES_BLOCK_ORDER_H
#define IR_PASSES_BLOCK_ORDER_H

namespace ir {

struct IrFunction;

/**
 * @brief Reordena los bloques de @p fn a Reverse Post-Order desde la entrada,
 *        con los inalcanzables al final.
 *
 * Quien parte bloques los anade al final, en el orden en que los crea, y el
 * array queda en orden NO topologico.  El emisor de bytecode y su
 * asignador/vida asumen un orden cercano al flujo de control (el fall-through
 * a `bid+1`, la vida lineal), asi que un orden mezclado producia codigo
 * incorrecto -- resultados que se pisaban entre sitios -- y no determinista.
 * El RPO es el orden canonico, independiente de como se crearon.
 *
 * @param fn Funcion; se reescriben los ids de bloque y todas sus referencias.
 */
void reorder_blocks_rpo(IrFunction &fn);

} // namespace ir

#endif // IR_PASSES_BLOCK_ORDER_H
