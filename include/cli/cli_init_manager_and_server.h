/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file cli_init_manager_and_server.h
 * @brief Registro centralizado de gestores ManageVM accedido desde la CLI.
 *
 * Define:
 *   - ManageVMSnapshot: vista ligera (copiable) del estado de un ManageVM.
 *   - ManagerOfManagersAndServer: contenedor thread-safe de instancias ManageVM con
 *     operaciones de alta nivel para crear, consultar, modificar y eliminar gestores.
 */

#ifndef CLI_INIT_MANAGER_AND_SERVER_H
#define CLI_INIT_MANAGER_AND_SERVER_H

#include <vector>
#include <unordered_map>
#include <optional>
#include <mutex>
#include <atomic>
#include <memory>

#include "runtime/manager_runtime.h"

/**
 * @brief Instantanea ligera del estado de un ManageVM en un momento dado.
 *
 * Contiene unicamente campos copiables para poder devolverlos al llamante
 * sin exponer la referencia interna al ManageVM (que no es copiable).
 */
struct ManageVMSnapshot {
    std::string name_manager; ///< Nombre identificativo del gestor
    size_t      vm_count;     ///< Numero de instancias VM activas en el momento de la instantanea
    bool        has_listener; ///< true si el gestor tiene un listener TCP activo
};

/**
 * @brief Contenedor thread-safe de instancias ManageVM gestionadas por la CLI.
 *
 * Permite a la CLI crear, consultar, modificar y eliminar gestores ManageVM de forma
 * concurrente.  Cada gestor recibe un ID unico monotonamente creciente.  El acceso
 * al mapa interno esta protegido por un mutex para evitar carreras de datos.
 *
 * Operaciones publicas seguras para llamadas concurrentes:
 *   - add_manager()       : crea un nuevo gestor y devuelve su puntero.
 *   - snapshot()          : devuelve una lista de (id, ManageVMSnapshot) copiable.
 *   - size_managers()     : devuelve el numero de gestores actuales.
 *   - list_ids()          : devuelve los IDs activos.
 *   - remove_manager()    : elimina un gestor por ID.
 *   - get_manager_copy()  : devuelve una instantanea de un gestor concreto.
 *   - replace_manager()   : sustituye un gestor existente por uno nuevo.
 *   - modify_manager()    : ejecuta una funcion sobre un gestor bajo el mutex.
 *   - find_if()           : busca el primer gestor que cumpla un predicado.
 */
class ManagerOfManagersAndServer {
    /**
     * @brief Mutex que protege 'managers_map'.
     *
     * La CLI puede invocar operaciones desde multiples hilos (worker de comandos,
     * callbacks de red, etc.) por lo que el acceso al mapa debe serializar.
     */
    mutable std::mutex mtx;

    /// Mapa de ID a instancia ManageVM (propiedad exclusiva via unique_ptr)
    std::unordered_map<size_t, std::unique_ptr<runtime::ManageVM> > managers_map;

    /// Contador atomico para asignar IDs unicos sin necesidad del mutex
    std::atomic<size_t> next_id{1};

    /// No copiable ni movible: contiene mutex y unique_ptr
    ManagerOfManagersAndServer(const ManagerOfManagersAndServer &) = delete;
    ManagerOfManagersAndServer &operator=(const ManagerOfManagersAndServer &) = delete;

public:
    /**
     * @brief Construye el contenedor vacio.
     */
    ManagerOfManagersAndServer() = default;

    /**
     * @brief Destructor por defecto: libera todos los gestores.
     */
    ~ManagerOfManagersAndServer() = default;

    /**
     * @brief Crea un nuevo ManageVM con el nombre y listener indicados y lo registra.
     *
     * Construye internamente el unique_ptr<ManageVM>, le asigna un ID unico y lo
     * inserta en el mapa.  Devuelve un puntero directo al objeto para uso inmediato;
     * la propiedad permanece en el contenedor.
     *
     * @param name     Nombre identificativo del gestor (para logs y diagnostico).
     * @param listener Listener TCP opcional (puede ser nullptr para modo local).
     * @return Puntero al ManageVM recien creado (valido mientras no se elimine del contenedor).
     */
    runtime::ManageVM* add_manager(const std::string& name, runtime::ManagerTCPListener *listener);

    /**
     * @brief Devuelve una lista de instantaneas de todos los gestores activos.
     *
     * Las instantaneas son copias ligeras (ManageVMSnapshot) seguras para devolver
     * fuera del mutex.
     *
     * @return Vector de pares (id, ManageVMSnapshot) con el estado actual de cada gestor.
     */
    std::vector<std::pair<size_t, ManageVMSnapshot> > snapshot();

    /**
     * @brief Devuelve el numero de gestores actualmente registrados.
     *
     * @return Numero de gestores en el mapa interno.
     */
    size_t size_managers() const;

    /**
     * @brief Devuelve la lista de IDs de gestores activos.
     *
     * Util para iterar sin mantener referencias internas al mapa.
     *
     * @return Vector de IDs asignados a los gestores activos.
     */
    std::vector<size_t> list_ids() const;

    /**
     * @brief Elimina el gestor con el ID indicado y libera sus recursos.
     *
     * @param id ID del gestor a eliminar.
     * @return true si existia y fue eliminado; false si no se encontro.
     */
    bool remove_manager(size_t id);

    /**
     * @brief Devuelve una instantanea del gestor con el ID indicado.
     *
     * @param id ID del gestor a consultar.
     * @return std::optional con la ManageVMSnapshot; std::nullopt si no existe.
     */
    std::optional<ManageVMSnapshot> get_manager_copy(size_t id) const;

    /**
     * @brief Sustituye el gestor con el ID indicado por uno nuevo.
     *
     * El gestor anterior se destruye al realizar la sustitucion.
     *
     * @param id ID del gestor a reemplazar.
     * @param vm Nuevo ManageVM (se mueve al contenedor).
     * @return true si existia y fue reemplazado; false si el ID no se encontro.
     */
    bool replace_manager(size_t id, std::unique_ptr<runtime::ManageVM> vm);

    /**
     * @brief Ejecuta una funcion sobre el gestor identificado por @p id bajo el mutex.
     *
     * Permite modificar el estado interno de un ManageVM de forma thread-safe.
     * La funcion recibe una referencia directa al objeto almacenado en el mapa.
     *
     * @warning El callable se ejecuta con el mutex adquirido: debe ser rapido y no
     *          realizar operaciones bloqueantes externas para evitar deadlocks.
     *
     * @tparam Func Callable con la firma: void(runtime::ManageVM&)
     * @param id ID del gestor a modificar.
     * @param fn Funcion que recibe la referencia al gestor y puede modificarlo.
     * @return true si el gestor existia y la funcion fue ejecutada; false en caso contrario.
     */
    template<typename Func>
    bool modify_manager(size_t id, Func &&fn) {
        std::lock_guard lk(mtx);
        auto it = managers_map.find(id);
        if (it == managers_map.end()) return false;
        fn(it->second); // referencia directa bajo mutex
        return true;
    }

    /**
     * @brief Busca el primer gestor que cumpla el predicado y devuelve su ID y una instantanea.
     *
     * Itera el mapa bajo el mutex evaluando @p pred sobre cada ManageVM.  Al encontrar
     * uno que cumpla el predicado construye una ManageVMSnapshot copiable y retorna.
     *
     * @tparam Pred Callable con la firma: bool(const std::unique_ptr<runtime::ManageVM>&)
     * @param pred Predicado de busqueda.
     * @return std::optional con (id, ManageVMSnapshot); std::nullopt si ningun gestor cumple.
     */
    template<typename Pred>
    std::optional<std::pair<size_t, ManageVMSnapshot> >
    find_if(Pred &&pred) const {
        std::lock_guard<std::mutex> lk(mtx);

        for (const auto &[id, mgr]: managers_map) {
            if (pred(mgr)) {
                // construir instantanea ligera sin exponer la referencia interna
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

#endif // CLI_INIT_MANAGER_AND_SERVER_H
