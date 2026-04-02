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

    std::unordered_map<size_t, runtime::ManageVM> managers_map;
    std::atomic<size_t> next_id{1};

    // No copy ni move por simplicidad; si quierer soportar, implementar con cuidado.
    ManagerOfManagersAndServer(const ManagerOfManagersAndServer &) = delete;

    ManagerOfManagersAndServer &operator=(const ManagerOfManagersAndServer &) = delete;

public:
    ManagerOfManagersAndServer() = default;

    ~ManagerOfManagersAndServer() = default;

    /**
     * @brief Añade un manager y devuelve su id único.
     * @param vm Manager a añadir (se mueve).
     * @return id asignado al manager.
     */
    size_t add_manager(const runtime::ManageVM vm);

    /**
     * @brief Devuelve una snapshot (copia) de todos los managers como vector de pares (id, copia).
     * @return vector de pares (id, runtime::ManageVM).
     */
    std::vector<std::pair<size_t, runtime::ManageVM> > snapshot() const;

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
    std::optional<runtime::ManageVM> get_manager_copy(size_t id) const;

    /**
     * @brief Reemplaza el manager con id por uno nuevo.
     * @param id Id del manager a reemplazar.
     * @param vm Nuevo manager (se mueve).
     * @return true si existía y fue reemplazado.
     */
    bool replace_manager(size_t id, runtime::ManageVM vm);

    /**
     * @brief Ejecuta una función sobre el manager identificado por id bajo mutex.
     *
     * La función recibe una referencia no const al manager y puede modificarlo.
     * La función se ejecuta mientras se mantiene el bloqueo, por lo que debe ser
     * rápida y no bloquear recursos externos para evitar deadlocks.
     *
     * @tparam Func Tipo callable: void(runtime::ManageVM &)
     * @param id Id del manager.
     * @param fn Callable que modifica el manager.
     * @return true si el manager existía y la función fue ejecutada.
     */
    template<typename Func>
    bool modify_manager(size_t id, Func &&fn) {
        std::lock_guard lk(mtx);
        auto it = managers_map.find(id);
        if (it == managers_map.end()) return false;
        fn(it->second);
        return true;
    }

    /**
     * @brief Busca el primer manager que cumpla el predicado y devuelve su id y copia.
     * @tparam Pred Callable que recibe (const runtime::ManageVM&) y devuelve bool.
     * @return optional<pair<id, copia>> si se encuentra.
     */
    template<typename Pred>
    std::optional<std::pair<size_t, runtime::ManageVM> > find_if(Pred &&pred) const {
        std::lock_guard<std::mutex> lk(mtx);
        for (const auto &p: managers_map) {
            if (pred(p.second)) return std::make_pair(p.first, p.second);
        }
        return std::nullopt;
    }
};

#endif //CLI_INIT_MANAGER_AND_SERVER_H
