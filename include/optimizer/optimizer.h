/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file optimizer.h
 * @brief Optimizador de bytecode para VestaVM.
 *
 * Expone la clase @c BytecodeOptimizer que aplica transformaciones de
 * optimizacion sobre un buffer de bytecode ya generado por el ensamblador.
 *
 * Optimizaciones previstas:
 *  - Eliminacion de NOPs redundantes.
 *  - Fusion de instrucciones triviales contiguas.
 *  - Eliminacion de bloques de codigo muerto (dead code elimination).
 *  - Compactacion de saltos (shrink de desplazamientos).
 *
 * @note El optimizador opera in-place sobre el vector de bytes y puede
 * modificar los offsets de instrucciones.  El linker debe recalcular las
 * relocalizaciones tras aplicar optimizaciones.
 */

#ifndef OPTIMIZER_H
#define OPTIMIZER_H

#include <cstdint>
#include <vector>

namespace Assembly::Bytecode::Optimizer {

/**
 * @class BytecodeOptimizer
 * @brief Aplica optimizaciones de peephole y eliminacion de codigo muerto al
 * bytecode.
 *
 * La clase se instancia sin estado y su unico punto de entrada es @c
 * optimize(), que modifica el buffer de bytecode proporcionado.
 *
 * Uso tipico (llamado por el linker):
 * @code
 *   BytecodeOptimizer opt;
 *   opt.optimize(module_bytecode);
 * @endcode
 */
class BytecodeOptimizer {
  public:
    /**
     * @brief Aplica todas las optimizaciones disponibles sobre el bytecode
     * dado.
     *
     * El contenido de @p code puede reducirse o modificarse.  Los offsets
     * absolutos embebidos en instrucciones de salto son actualizados de forma
     * correspondiente.
     *
     * @param code Buffer de bytecode a optimizar; se modifica in-place.
     */
    void optimize(std::vector<uint8_t> &code);
};

} // namespace Assembly::Bytecode::Optimizer

#endif // OPTIMIZER_H
