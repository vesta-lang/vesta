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
#ifndef SYNC_IO_H
#define SYNC_IO_H

#include <mutex>

namespace vesta {
    /**
     * @brief Mutex global para sincronizar accesos a std::cout / std::cerr entre hilos.
     *
     * Defínelo en un único fichero .cpp (sync_io.cpp). Incluye este header donde
     * necesites imprimir desde varios hilos para evitar salidas mezcladas.
     */
    extern std::mutex cout_mutex;
}

#endif //SYNC_IO_H
