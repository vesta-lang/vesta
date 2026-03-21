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

#ifndef MANAGER_RUNTIME_H
#define MANAGER_RUNTIME_H

#include <memory>
#include <vector>

#include "loader/loader.h"

namespace runtime {
    class VM;

    class ManageVM {
    public:

        /**
         * El manager puede gestionar memoria compartida entre instancias.
         */
        vm::ArenaManager manager_mem;

        /**
         * El manager puede tener un loader publico
         */
        loader::Loader loader;

        // Constructor por defecto
        ManageVM();

        // Destructor - libera recursos
        ~ManageVM();

        /**
         * @brief Crea una nueva VM con ID unico
         * @return ID de la VM creada (0 si falla)
         */
        uint64_t create_vm();

        /**
         * @brief Destruye una VM por su ID
         * @param vm_id ID de la VM a eliminar
         * @return true si se elimino correctamente
         */
        bool destroy_vm(uint64_t vm_id);

        /**
         * @brief Obtiene VM const por ID
         * @param vm_id ID de la VM
         * @return Referencia const a VM (undefined si no existe)
         */
        VM* get_vm(uint64_t vm_id);

        /**
         * @brief Verifica si existe una VM con el ID dado
         * @param vm_id ID a verificar
         * @return true si existe
         */
        bool has_vm(uint64_t vm_id) const;

        /**
         * @brief Obtiene numero total de VMs activas
         * @return Cantidad de VMs
         */
        size_t vm_count() const;

        /**
         * @brief Libera todas las VMs (emergency cleanup)
         */
        void destroy_all_vms();

        // contiene todas las instancias de maquinas
        std::vector<VM*> vms;

    private:
        // se usa para generar los ID's de la VM, las VM nuevas no tendran un mismo ID
        // que una instancia ya creada o muerta en el mismo manager.
        uint64_t counter_vm;
    };
};

#endif //MANAGER_RUNTIME_H
