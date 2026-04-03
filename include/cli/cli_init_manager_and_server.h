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

struct ManageVMSnapshot {
    std::string name_manager;
    size_t vm_count;
    bool has_listener;
};


/**
 * @brief Esta clase permite almacenar todas las instancias de manager y servidores
 * de manager gestionadas por la cli.
 *
 * Usa IDs únicos (size_t) para identificar managers. Internamente protege
 * el contenedor con un mutex; las operaciones públicas son seguras para
 * llamadas concurrentes.
 */
class ManagerOfManagersAndServer {
    /**
     * El mutex protege 'managers'. Al ser una CLI
     * asincrona que usa multi-hilo, puede darse el caso
     * de que varios hilos intenten hacer alguna operacion a la vez aqui.
     */
    mutable std::mutex mtx;

    std::unordered_map<size_t, std::unique_ptr<runtime::ManageVM> > managers_map;

    std::atomic<size_t> next_id{1};

    // No copy ni move por simplicidad; si quierer soportar, implementar con cuidado.
    ManagerOfManagersAndServer(const ManagerOfManagersAndServer &) = delete;

    ManagerOfManagersAndServer &operator=(const ManagerOfManagersAndServer &) = delete;

public:
    ManagerOfManagersAndServer() = default;

    ~ManagerOfManagersAndServer() = default;

    /**
     * @brief Añade un manager y devuelve su id único.
     *
     * @code{.cpp}
     * auto vm = std::make_unique<runtime::ManageVM>(/ ctor args /);
     * size_t id = mgr.add_manager(std::move(vm));
     * @endcode
     *
     * @param vm Manager a añadir (se mueve).
     * @return id asignado al manager.
     */
    runtime::ManageVM* add_manager(const std::string& name, runtime::ManagerTCPListener *listener);

    /**
     * @brief Devuelve una snapshot (copia) de todos los managers como vector de pares (id, copia).
     * @return vector de pares (id, runtime::ManageVM).
     */
    std::vector<std::pair<size_t, ManageVMSnapshot> > snapshot();

    /**
     * @brief Devuelve la cantidad de managers almacenados.
     */
    size_t size_managers() const;

    /**
     * @brief Lista de ids actuales (útil para iterar sin mantener referencias internas).
     */
    std::vector<size_t> list_ids() const;

    /**
     * @brief Elimina el manager con id dado.
     * @param id Id del manager.
     * @return true si existía y fue eliminado.
     */
    bool remove_manager(size_t id);

    /**
     * @brief Devuelve una copia del manager si existe.
     * @param id Id del manager.
     * @return std::optional con la copia; std::nullopt si no existe.
     */
    std::optional<ManageVMSnapshot> get_manager_copy(size_t id) const;

    /**
     * @brief Reemplaza el manager con id por uno nuevo.
     * @param id Id del manager a reemplazar.
     * @param vm Nuevo manager (se mueve).
     * @return true si existía y fue reemplazado.
     */
    bool replace_manager(size_t id, std::unique_ptr<runtime::ManageVM> vm);

    /**
     * @brief Ejecuta una función sobre el manager identificado por id bajo protección de mutex.
     *
     * Este metodo permite modificar un manager existente de forma segura en un entorno multihilo.
     * La función proporcionada recibe una referencia directa al objeto ManageVM almacenado en
     * managers_map, por lo que puede modificar su estado interno.
     *
     * El mutex interno del ManagerOfManagersAndServer se mantiene bloqueado durante toda la
     * ejecución del callable, por lo que:
     *   - La operación es thread-safe.
     *   - El callable debe ser rápido y no realizar operaciones bloqueantes externas
     *     (E/S, locks adicionales, esperas activas), para evitar deadlocks o contención excesiva.
     *
     * @tparam Func Callable con la firma: void(runtime::ManageVM&)
     * @param id Identificador del manager a modificar.
     * @param fn Función que recibe una referencia al manager y lo modifica.
     * @return true si el manager existía y la función fue ejecutada; false en caso contrario.
     */
    template<typename Func>
    bool modify_manager(size_t id, Func &&fn) {
        std::lock_guard lk(mtx);
        auto it = managers_map.find(id);
        if (it == managers_map.end()) return false;
        fn(it->second); // referencia directa al manager real
        return true;
    }


    /**
     * @brief Busca el primer manager que cumpla el predicado y devuelve su id y un snapshot ligero.
     *
     * Este metodo permite inspeccionar managers sin exponer referencias internas ni intentar copiar
     * objetos ManageVM completos (lo cual no es posible debido a que ManageVM no es copiable).
     *
     * En su lugar, se genera un ManageVMSnapshot, una estructura ligera que contiene únicamente
     * información segura y copiables del manager (nombre, número de VMs, estado del listener, etc.).
     *
     * El acceso al mapa está protegido por mutex, garantizando seguridad en entornos multihilo.
     *
     * @tparam Pred Callable con la firma: bool(const runtime::ManageVM&)
     * @param pred Predicado que decide si un manager coincide.
     * @return std::optional con (id, snapshot) si se encuentra un manager que cumpla el predicado;
     *         std::nullopt si no existe ninguno.
     */

    template<typename Pred>
    std::optional<std::pair<size_t, ManageVMSnapshot> >
    find_if(Pred &&pred) const {
        std::lock_guard<std::mutex> lk(mtx);

        for (const auto &[id, mgr]: managers_map) {
            if (pred(mgr)) {
                ManageVMSnapshot snap{
                    mgr->name_manager,
                    mgr->vm_count(),
                    mgr->listener != nullptr
                };
                return std::make_pair(id, std::move(snap));
            }
        }
        return std::nullopt;
    }
};

#endif //CLI_INIT_MANAGER_AND_SERVER_H
