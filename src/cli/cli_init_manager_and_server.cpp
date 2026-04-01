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
#include "cli/cli_init_manager_and_server.h"

/**
 * Añadir la nueva VM al manager CLI
 * @param vm manager de instancias inicializado
 * @return posicion del manager de instancias en el manager CLI
 */
size_t ManagerOfManagersAndServer::add_manager(const runtime::ManageVM &vm) {
    managers.push_back(vm);
    return managers.size() - 1;           // índice del elemento recién añadido
}
