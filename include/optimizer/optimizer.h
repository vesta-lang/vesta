/*
 * VestaVM - Máquina Virtual Distribuida
 *
 * Copyright © 2026 David López.T (DesmonHak) (Castilla y León, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribución obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

#ifndef OPTIMIZER_H
#define OPTIMIZER_H
#include <cstdint>
#include <vector>


namespace Assembly::Bytecode::Optimizer {
    class BytecodeOptimizer {
    public:
        void optimize(std::vector<uint8_t> &code);
    };
}


#endif //OPTIMIZER_H
