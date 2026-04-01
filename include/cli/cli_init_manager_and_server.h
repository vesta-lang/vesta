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
#ifndef CLI_INIT_MANAGER_AND_SERVER_H
#define CLI_INIT_MANAGER_AND_SERVER_H
#include <vector>

#include "runtime/manager_runtime.h"

/**
 * Esta clase permite almacenar todas las instancias de manager y servidores
 * de manager gestionadas por la cli.
 */
class ManagerOfManagersAndServer {
    std::vector<runtime::ManageVM> managers;

    ManagerOfManagersAndServer() = default;

    public:
        size_t add_manager(const runtime::ManageVM &vm);
};

#endif //CLI_INIT_MANAGER_AND_SERVER_H
