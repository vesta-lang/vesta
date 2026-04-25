/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (c) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file gc_heap.h
 * @brief Heap generacional de recoleccion de basura por proceso.
 *
 * @section architecture Arquitectura del GcHeap
 *
 * El GcHeap implementa un recolector de basura generacional de dos generaciones
 * (Young/Old) con los siguientes componentes:
 *
 *   Nursery (Young generation)
 *   --------------------------
 *   Bloque contiguo de memoria host asignado via ArenaManager al construir el
 *   heap. La asignacion usa un bump-pointer (puntero de avance lineal), lo que
 *   la hace O(1) y cache-friendly. Cuando el bump-pointer alcanza el limite del
 *   bloque se dispara el minor GC.
 *
 *   OldGen (Old generation)
 *   -----------------------
 *   Conjunto de bloques asignados dinamicamente via ArenaManager conforme se
 *   necesitan. Cada bloque es un array lineal de objetos precedidos por GcHeader.
 *   La asignacion usa first-fit: primero busca slots DEAD reutilizables; si no
 *   hay, avanza al final del ultimo bloque (bump); si tampoco hay espacio pide
 *   un bloque nuevo al ArenaManager.
 *
 *   HandleTable
 *   -----------
 *   Vector de HandleEntry que mapea un GcHandle (uint32_t opaco) a la direccion
 *   host actual del objeto. Permite que el GC mueva objetos durante la evacuacion
 *   sin invalidar ninguna referencia en registros o stack del bytecode, ya que
 *   todo el bytecode usa handles, nunca punteros directos.
 *   Slots libres se reciclan via free_handles_ (freelist LIFO).
 *
 *   RememberedSet
 *   -------------
 *   Vector de GcHandle de objetos OLD que contienen referencias a objetos YOUNG.
 *   Necesario para que el minor GC sea completo sin escanear OldGen entero.
 *   El programador (o el compilador del lenguaje Vesta) debe llamar a
 *   write_barrier() cada vez que escribe una referencia old->young.
 *
 * @section lifecycle Ciclo de vida de un objeto
 *
 * @code
 *   1. NEWOBJ  --> alloc() --> objeto en Nursery, color=WHITE, gen=YOUNG, handle H.
 *   2. [uso]   --> deref(H) devuelve puntero al payload.
 *   3. [muerte]--> drop(H)  --> handle liberado; objeto permanece fisicamente
 *                               en Nursery/OldGen hasta el proximo GC.
 *   4. minor_gc()
 *        - Evacua objetos YOUNG con handle vivo --> OldGen, color=BLACK.
 *        - Objetos YOUNG sin handle (soltados) simplemente se abandonan;
 *          el nursery_bump_ se resetea, reclamando todo el espacio de golpe.
 *   5. major_gc()
 *        - PRE-MARK: todos los objetos OldGen no-DEAD --> WHITE.
 *        - MARK:     handles vivos en OldGen --> BLACK.
 *        - SWEEP:    WHITE --> DEAD (slot reutilizable, old_used_ decrementado).
 * @endcode
 *
 * @section tri_color Algoritmo tri-color
 *
 *   WHITE  -- no visitado; candidato a recolectar si sigue WHITE tras MARK.
 *   GREY   -- alcanzable pero referencias pendientes de procesar (reservado).
 *   BLACK  -- alcanzable y completamente procesado; sobrevive este ciclo.
 *   DEAD   -- liberado por sweep; slot fisico disponible para reutilizacion.
 *             El campo GcHeader::size se preserva para que el scanner pueda
 *             avanzar correctamente y alloc_in_old pueda calcular el tamano.
 *
 * @section stats Estadisticas sin overhead
 *
 * Todos los contadores de GcStats son plain uint64_t (sin atomics ni ramas
 * adicionales). Cada evento incrementa exactamente un campo, y el CPU ya tiene
 * el objeto GcHeap en L1 cache durante la asignacion. El costo adicional es
 * de una instruccion ADD por evento en el hot-path.
 *
 * @section thread_safety Seguridad de hilos
 *
 * GcHeap NO es thread-safe. Esta disenado para uso exclusivo del proceso
 * propietario (un ProcessVM). Si el scheduler migra el proceso a otro hilo
 * debe garantizar exclusion mutua antes de llamar a cualquier metodo del heap.
 */

#ifndef GC_HEAP_H
#define GC_HEAP_H

#include <cstdint>
#include <cstddef>
#include <vector>
#include <unordered_set>

#include "arena/VirtualMemory.h"
#include "arena/arena_manager.h"

namespace gc {
    /**
     * @brief Handle opaco para referenciar objetos gestionados por el GcHeap.
     *
     * El bytecode de Vesta usa GcHandle en lugar de punteros directos. Esto
     * desacopla las referencias del proceso de la ubicacion fisica del objeto,
     * permitiendo que el GC mueva objetos (compactacion/evacuacion) sin
     * invalidar ningun registro de la VM.
     *
     * Un GcHandle es simplemente un indice en la HandleTable interna del heap.
     * El valor GC_NULL_HANDLE (UINT32_MAX) representa una referencia nula.
     *
     * @note Un GcHandle no es un puntero. Nunca hacer reinterpret_cast sobre el.
     */
    using GcHandle = uint32_t;

    /** @brief Valor centinela que representa un handle nulo o invalido. */
    static constexpr GcHandle GC_NULL_HANDLE = UINT32_MAX;

    /**
     * @brief Estado del objeto dentro del algoritmo de marcado tri-color.
     *
     * Los valores estan codificados en 2 bits dentro de GcHeader para minimizar
     * el tamano de la cabecera y mantener los objetos alineados a 8 bytes.
     *
     * Transiciones durante major_gc():
     * @code
     *   (todos los no-DEAD) --(PRE-MARK)--> WHITE
     *   WHITE (handle vivo)  --(MARK)-----> BLACK
     *   WHITE (sin handle)   --(SWEEP)----> DEAD
     *   BLACK                --(SWEEP)----> sin cambio (sobrevive)
     *   DEAD                 --(SWEEP)----> sin cambio (ya liberado)
     * @endcode
     */
    enum class GcColor : uint8_t {
        WHITE = 0, /**< No visitado; candidato a recolectar. */
        GREY  = 1, /**< Alcanzable; referencias pendientes (reservado). */
        BLACK = 2, /**< Alcanzable y completamente procesado. */
        DEAD  = 3  /**< Liberado por sweep; slot fisico reutilizable. */
    };

    /**
     * @brief Generacion del objeto dentro del heap generacional.
     */
    enum class GcGen : uint8_t {
        YOUNG = 0, /**< Recien asignado; reside en Nursery. */
        OLD   = 1  /**< Sobrevivio al menos un minor GC; reside en OldGen. */
    };

    /**
     * @brief Cabecera de metadatos que precede a cada objeto en el heap GC.
     *
     * Precede inmediatamente al payload del objeto en memoria. El layout es:
     * @code
     *   [GcHeader (8 bytes)] [payload (size bytes)] [padding hasta alinear a 8]
     * @endcode
     *
     * El tamano total de un slot es: (sizeof(GcHeader) + size + 7) & ~7
     *
     * @note alignas(8) garantiza que el payload que sigue a la cabecera este
     *       alineado a 8 bytes, lo que permite almacenar cualquier tipo primitivo
     *       sin violaciones de alineacion.
     *
     * @warning El campo size se preserva incluso cuando color == DEAD, porque
     *          alloc_in_old() necesita calcular el tamano del slot para decidir
     *          si puede reutilizarlo. Nunca poner size = 0 en un slot DEAD.
     */
    struct alignas(8) GcHeader {
        uint32_t size;         /**< Bytes del payload (sin incluir esta cabecera). */
        GcColor  color: 2;     /**< Estado tri-color mas DEAD. */
        GcGen    gen  : 1;     /**< Generacion: YOUNG o OLD. */
        uint8_t  _pad : 5;     /**< Bits reservados para uso futuro. */
        uint8_t  _reserved[3]; /**< Padding hasta 8 bytes; reservado. */
    };

    static_assert(sizeof(GcHeader) == 8,
                  "GcHeader debe medir exactamente 8 bytes");

    /**
     * @brief Entrada en la tabla de handles del GcHeap.
     *
     * La HandleTable es un vector<HandleEntry> indexado por GcHandle.
     * Cuando un handle se libera (drop()), addr se pone a nullptr y live a false,
     * y el slot se recicla via free_handles_ para la siguiente asignacion.
     */
    struct HandleEntry {
        uint8_t *addr; /**< Puntero host al inicio del GcHeader del objeto.
                        *   nullptr si el slot esta libre (live == false). */
        bool     live; /**< true si el handle referencia un objeto valido. */
    };

    /**
     * @brief Entrada en la tabla de referencias debiles del GcHeap.
     *
     * Una referencia debil apunta a un objeto sin impedir su recoleccion.
     * Durante la fase de barrido del major GC, si el objeto apuntado ha muerto,
     * target se pone a GC_NULL_HANDLE automaticamente.
     *
     * Se accede mediante un indice uint32_t (WeakHandle).
     */
    struct WeakEntry {
        GcHandle target; ///< handle del objeto referenciado (GC_NULL_HANDLE si recolectado)
        bool     live;   ///< true si esta entrada esta en uso
    };

    /**
     * @brief Estadisticas acumuladas del GcHeap.
     *
     * Todos los campos son plain uint64_t sin sincronizacion. El heap es
     * per-proceso (un solo hilo lo usa a la vez), por lo que no se necesitan
     * atomics. El overhead de actualizarlas es de una instruccion ADD por evento,
     * ya que el objeto GcHeap suele estar en L1 cache durante las operaciones
     * de asignacion y recoleccion.
     *
     * Se accede via GcHeap::stats() que devuelve una referencia const (sin copia).
     *
     * @note Los contadores son monotonicamente crecientes; nunca se resetean.
     *       Para medir un intervalo, tomar snapshot antes y despues.
     */
    struct GcStats {
        uint64_t alloc_count    = 0; /**< Llamadas a alloc() que tuvieron exito. */
        uint64_t alloc_bytes    = 0; /**< Bytes de payload asignados en total. */
        uint64_t freed_count    = 0; /**< Objetos liberados por major GC (sweep). */
        uint64_t freed_bytes    = 0; /**< Bytes de payload liberados por sweep. */
        uint64_t promoted_count = 0; /**< Objetos evacuados Nursery -> OldGen. */
        uint64_t promoted_bytes = 0; /**< Bytes de payload evacuados a OldGen. */
        uint64_t minor_gc_count = 0; /**< Ciclos de minor GC ejecutados. */
        uint64_t major_gc_count = 0; /**< Ciclos de major GC ejecutados. */
        uint64_t peak_nursery   = 0; /**< Uso maximo de Nursery en bytes. */
        uint64_t peak_old       = 0; /**< Uso maximo de OldGen en bytes. */
    };

    /**
     * @brief Heap generacional de recoleccion de basura por proceso.
     *
     * Implementa un GC de dos generaciones (Nursery + OldGen) con los siguientes
     * algoritmos:
     *
     *   - minor_gc(): evacuacion estilo Cheney. Copia objetos YOUNG vivos a OldGen
     *     en un solo pase sobre la HandleTable. La Nursery se resetea atomicamente
     *     reseteando el bump-pointer, sin coste por objeto muerto.
     *
     *   - major_gc(): mark-and-sweep tri-color sobre OldGen. Tres fases:
     *     PRE-MARK (reset a WHITE), MARK (handles vivos -> BLACK),
     *     SWEEP (WHITE -> DEAD, actualizar old_used_).
     *
     * Uso tipico en bytecode Vesta:
     * @code
     *   GcHandle h = heap.alloc(sizeof(MyObject));
     *   MyObject *obj = reinterpret_cast<MyObject *>(heap.deref(h));
     *   obj->field = 42;
     *   // ... usar obj solo hasta el siguiente GC ...
     *   heap.drop(h);  // objeto elegible para recoleccion
     * @endcode
     *
     * @warning Los punteros devueltos por deref() son invalidos tras cualquier
     *          llamada a minor_gc() o major_gc(), ya que el GC puede mover los
     *          objetos. Siempre llamar a deref() de nuevo despues de un GC.
     *
     * @warning No es thread-safe. Un solo hilo debe usar el heap a la vez.
     */
    class GcHeap {
    public:
        /**
         * @brief Construye el heap y reserva la Nursery.
         *
         * @param arena_mgr    ArenaManager del proceso propietario. OldGen pide
         *                     bloques a traves de el cuando necesita mas espacio.
         * @param nursery_bytes Tamano de la Nursery en bytes. Un tamano pequeno
         *                     provoca minor GC mas frecuentes pero pausas mas cortas.
         *                     Por defecto 1 MB.
         * @param old_threshold Bytes de OldGen usados que disparan un major GC
         *                      automatico al final de cada minor GC. Por defecto 8 MB.
         *
         * @note La Nursery se asigna como una Arena READ|WRITE en arena_mgr para
         *       que quede registrada en el proceso y sea liberada correctamente
         *       incluso si el proceso muere sin llamar al destructor.
         */
        explicit GcHeap(vm::ArenaManager &arena_mgr,
                        size_t            nursery_bytes = 1 * 1024 * 1024,
                        size_t            old_threshold = 8 * 1024 * 1024);

        /**
         * @brief Destructor. Libera la Nursery y todos los bloques de OldGen.
         *
         * Llama a arena_mgr_.free_arena() por cada bloque. No llama a ninguna
         * funcion de finalizacion de objetos (el lenguaje Vesta no tiene
         * destructores a nivel de VM en esta version).
         */
        ~GcHeap();

        GcHeap(const GcHeap &) = delete;

        GcHeap &operator=(const GcHeap &) = delete;

        /**
         * @brief Asigna un objeto de @p size bytes payload en el heap.
         *
         * Flujo de asignacion:
         * @code
         *   1. Fast-path: bump Nursery si hay espacio.        O(1)
         *   2. Nursery llena: disparar minor_gc() y reintentar.
         *   3. Sigue sin espacio: asignar directamente en OldGen.
         *   4. OOM en OldGen: devolver GC_NULL_HANDLE.
         * @endcode
         *
         * El payload se zero-inicializa siempre. El GcHeader se escribe con
         * color=WHITE, gen=YOUNG (o OLD si va directo a OldGen).
         *
         * @param size Bytes del payload (sin incluir GcHeader).
         * @return Handle valido, o GC_NULL_HANDLE si no hay memoria disponible.
         *
         * @note El handle devuelto permanece valido hasta que se llame a drop().
         *       La direccion fisica puede cambiar tras un minor_gc().
         */
        GcHandle alloc(size_t size);

        /**
         * @brief Devuelve un puntero al payload del objeto referenciado por @p handle.
         *
         * El puntero apunta inmediatamente despues del GcHeader del objeto.
         * Es valido para leer y escribir hasta la proxima llamada a minor_gc()
         * o major_gc(), ya que el GC puede mover el objeto.
         *
         * @param handle Handle valido devuelto por alloc().
         * @return Puntero al payload, o nullptr si el handle es invalido o libre.
         *
         * @warning Nunca almacenar este puntero mas alla del siguiente GC.
         *          Usar el handle para reconvertir si es necesario.
         */
        uint8_t *deref(GcHandle handle);

        /**
         * @brief Registra una referencia old->young en el remembered set.
         *
         * Debe llamarse cada vez que el codigo de usuario (o el compilador del
         * lenguaje Vesta) escribe un GcHandle YOUNG dentro del payload de un
         * objeto OLD. Sin esta llamada, el minor GC podria no encontrar el objeto
         * YOUNG como alcanzable y recolectarlo prematuramente.
         *
         * No anade duplicados al remembered set.
         *
         * @param old_handle Handle del objeto OLD que contiene el campo modificado.
         *
         * @note Esta funcion solo es necesaria para referencias cross-generacion
         *       (OLD -> YOUNG). Referencias YOUNG -> YOUNG o OLD -> OLD no
         *       necesitan write barrier.
         */
        void write_barrier(GcHandle old_handle);

        /**
         * @brief Libera el handle indicando que el objeto ya no es alcanzable.
         *
         * Marca el slot en la HandleTable como libre (live=false, addr=nullptr)
         * y lo recicla para futuras asignaciones. El objeto permanece fisicamente
         * en Nursery u OldGen hasta que el GC lo recolecte.
         *
         * En la Nursery: el objeto es ignorado en la proxima evacuacion y su
         * memoria se reclama cuando el bump-pointer se resetea.
         *
         * En OldGen: el objeto permanece con su GcHeader intacto hasta el proximo
         * major_gc(), donde el PRE-MARK lo pone WHITE y el SWEEP lo marca DEAD.
         *
         * @param h Handle a liberar. Si es invalido o ya esta libre, no hace nada.
         */
        void drop(GcHandle h) {
            release_handle(h);
        }

        /**
         * @brief Crea una referencia debil al objeto indicado por @p target.
         *
         * La referencia debil no impide la recoleccion del objeto.
         * Durante el barrido del major GC, si el objeto es recolectado,
         * la entrada se pone automaticamente a GC_NULL_HANDLE.
         *
         * @param target Handle del objeto a referenciar debilmente.
         * @return Indice uint32_t de la entrada en weak_table_.
         */
        uint32_t alloc_weak(GcHandle target);

        /**
         * @brief Lee el handle apuntado por la referencia debil @p idx.
         *
         * @param idx Indice en weak_table_ obtenido de alloc_weak().
         * @return Handle del objeto si aun esta vivo; GC_NULL_HANDLE si fue recolectado.
         */
        GcHandle deref_weak(uint32_t idx) const;

        /**
         * @brief Libera la entrada de referencia debil @p idx.
         *
         * Marca la entrada como libre para su reutilizacion.
         *
         * @param idx Indice en weak_table_ a liberar.
         */
        void free_weak(uint32_t idx);

        // =====================================================================
        //  Primitivas de monitor (sincronizacion por objeto)
        //
        //  Un monitor es un lock reentrante asociado a un objeto GC.  El estado
        //  del lock (propietario y profundidad) se almacena en ObjectHeader de
        //  cada objeto.  La cola de procesos en espera se almacena en esta clase.
        //
        //  Invariante:
        //    - ObjectHeader::owner_pid == 0  <=>  monitor libre
        //    - ObjectHeader::owner_pid != 0  <=>  monitor adquirido por ese pid
        //    - ObjectHeader::lock_depth >= 1 cuando owner_pid != 0
        //
        //  Los PID codificados en las colas usan el formato:
        //    encoded_pid = (scheduler_id << 32) | local_pid
        // =====================================================================

        /**
         * @brief Intenta adquirir el monitor del objeto referenciado por @p h.
         *
         * Si el monitor esta libre, lo asigna a @p local_pid y devuelve true.
         * Si ya lo posee @p local_pid (lock reentrante), incrementa lock_depth y
         * devuelve true.
         * Si lo posee otro proceso, devuelve false (el llamante debe bloquear).
         *
         * @param h         Handle del objeto cuyo monitor se quiere adquirir.
         * @param local_pid PID local del proceso solicitante.
         * @return true si el monitor fue adquirido o incrementado; false si bloqueado.
         */
        bool monitor_try_acquire(GcHandle h, uint32_t local_pid);

        /**
         * @brief Libera el monitor del objeto referenciado por @p h.
         *
         * Decrementa lock_depth.  Si llega a 0, marca el monitor como libre y
         * extrae un proceso de la cola de espera para despertarlo.
         *
         * @param h         Handle del objeto cuyo monitor se libera.
         * @param local_pid PID local del proceso propietario actual.
         * @return PID codificado del siguiente proceso de la cola de espera
         *         (0 si la cola estaba vacia o el monitor sigue bloqueado).
         */
        uint64_t monitor_release(GcHandle h, uint32_t local_pid);

        /**
         * @brief Anade un PID codificado a la cola de espera del monitor de @p h.
         *
         * Se llama desde exec_instr_monenter cuando el monitor esta ocupado, o
         * desde exec_instr_monwait cuando el proceso libera el monitor para esperar.
         *
         * @param h           Handle del objeto cuyo monitor espera el proceso.
         * @param encoded_pid PID codificado: (scheduler_id << 32) | local_pid.
         */
        void monitor_add_waiter(GcHandle h, uint64_t encoded_pid);

        /**
         * @brief Extrae y devuelve un PID codificado de la cola de espera de @p h.
         *
         * Devuelve 0 si la cola esta vacia.  Se usa en MONNOTI para despertar
         * exactamente un proceso.
         *
         * @param h Handle del objeto cuya cola de espera se consulta.
         * @return PID codificado del proceso despertado, o 0 si la cola estaba vacia.
         */
        uint64_t monitor_pop_waiter(GcHandle h);

        /**
         * @brief Extrae y devuelve todos los PID codificados de la cola de @p h.
         *
         * La cola queda vacia tras la llamada.  Se usa en MONNOTA para despertar
         * a todos los procesos que esperan sobre el mismo objeto.
         *
         * @param h Handle del objeto cuya cola de espera se vacia.
         * @return Vector con todos los PID codificados (puede estar vacio).
         */
        std::vector<uint64_t> monitor_pop_all_waiters(GcHandle h);

        /**
         * @brief Minor GC: evacua la Nursery copiando supervivientes a OldGen.
         *
         * Algoritmo (Cheney-style simplificado):
         * @code
         *   Para cada handle H en la HandleTable:
         *     Si H.live && H.addr en rango Nursery && gen == YOUNG:
         *       alloc_in_old(tamano del objeto)
         *       memcpy al nuevo slot
         *       H.addr = nuevo slot (handle actualizado)
         *       objeto original marcado BLACK (forward pointer en payload)
         *   remembered_set.clear()
         *   nursery_bump = nursery_base  (reset O(1))
         *   Si old_used >= old_threshold: major_gc()
         * @endcode
         *
         * Todos los objetos YOUNG sin handle vivo son abandonados en la Nursery
         * y su memoria se reclama gratis con el reset del bump-pointer. Este es el
         * beneficio central de la generacional: limpiar objetos muertos es O(1).
         *
         * @note Provoca una pausa del proceso propietario. Otros procesos de la
         *       VM no se ven afectados.
         */
        void minor_gc();

        /**
         * @brief Major GC: mark-and-sweep tri-color sobre OldGen.
         *
         * Tres fases:
         *
         *   PRE-MARK: recorre todos los objetos OldGen y los pone WHITE (excepto
         *             los ya DEAD). Necesario porque los objetos llegan a OldGen
         *             con color BLACK tras la evacuacion y no hay otro mecanismo
         *             que los resetee cuando su handle se suelta.
         *
         *   MARK:     recorre la HandleTable. Por cada handle vivo cuyo objeto
         *             esta en OldGen, pone el objeto BLACK (alcanzable).
         *
         *   SWEEP:    recorre los bloques de OldGen. Objetos WHITE (sin raiz) se
         *             marcan DEAD, se decrementa old_used_ y se actualizan stats.
         *             Objetos BLACK sobreviven. Objetos DEAD se ignoran.
         *
         * @note Actualmente el MARK no sigue punteros dentro de los objetos
         *       (no hay grafo de objetos en esta version). Toda la liveness se
         *       determina por la HandleTable. Para soportar grafos de objetos
         *       se debe implementar un MARK transitive que siga campos GcHandle.
         *
         * @note Provoca una pausa del proceso propietario. Puede ser largo si
         *       OldGen es grande. Para pausas acotadas considerar un major GC
         *       incremental en versiones futuras.
         */
        void major_gc();

        /**
         * @brief Configura el umbral de OldGen para major GC automatico.
         *
         * Cuando old_used_ >= old_threshold_ al finalizar un minor_gc(), se
         * dispara un major_gc() automaticamente. Un umbral bajo provoca mas
         * ciclos major GC pero mantiene OldGen mas compacto.
         *
         * @param bytes Umbral en bytes. Un valor de 0 deshabilita el major GC
         *              automatico (solo se puede disparar manualmente).
         */
        void set_old_threshold(size_t bytes) {
            old_threshold_ = bytes;
        }

        /** @brief Bytes actualmente usados en la Nursery (distancia bump-base). */
        size_t nursery_used() const {
            return static_cast<size_t>(nursery_bump_ - nursery_base_);
        }

        /** @brief Tamano total de la Nursery en bytes. */
        size_t nursery_total() const {
            return nursery_size_;
        }

        /** @brief Bytes actualmente contabilizados en OldGen. */
        size_t old_used() const {
            return old_used_;
        }

        /**
         * @brief Devuelve una referencia de solo lectura a las estadisticas acumuladas.
         *
         * La referencia es valida mientras el GcHeap exista. No copia los datos.
         * Los contadores son monotonicamente crecientes; para medir un intervalo
         * tomar un snapshot antes y otro despues de la operacion de interes.
         *
         * @return Referencia const a GcStats.
         */
        const GcStats &stats() const {
            return stats_;
        }

    private:
        vm::ArenaManager &arena_mgr_;

        // --- Nursery ---
        uint8_t *nursery_base_     = nullptr; ///< Inicio del bloque Nursery.
        uint8_t *nursery_bump_     = nullptr; ///< Proximo byte libre (bump pointer).
        uint8_t *nursery_end_      = nullptr; ///< Fin del bloque Nursery.
        size_t   nursery_size_     = 0;       ///< Tamano total de la Nursery.
        uint64_t nursery_arena_id_ = 0;       ///< ID en ArenaManager para liberar al destruir.

        // --- OldGen ---
        struct OldBlock {
            uint8_t *ptr;      ///< Inicio del bloque.
            size_t   size;     ///< Tamano total del bloque en bytes.
            uint64_t arena_id; ///< ID en ArenaManager para liberar al destruir.
        };

        std::vector<OldBlock> old_blocks_;
        size_t                old_used_      = 0; ///< Bytes contabilizados en OldGen.
        size_t                old_threshold_ = 0; ///< Umbral para major GC automatico.

        // --- HandleTable ---
        std::vector<HandleEntry> handles_;
        std::vector<GcHandle>    free_handles_; ///< Freelist LIFO de slots reciclables.

        // --- Tabla de referencias debiles ---
        std::vector<WeakEntry> weak_table_; ///< Referencias debiles indexadas por uint32_t.

        // --- Colas de espera de monitores ---
        // Clave: GcHandle del objeto con el monitor ocupado.
        // Valor: lista FIFO de PID codificados ((scheduler_id<<32)|local_pid) esperando.
        std::unordered_map<GcHandle, std::vector<uint64_t>> monitor_waiters_;

        // --- RememberedSet ---
        std::unordered_set<GcHandle> remembered_set_; ///< Handles OLD con referencias a YOUNG.

        // --- Estadisticas ---
        GcStats stats_;

        /**
         * @brief Crea un nuevo handle apuntando a @p addr.
         * Reutiliza un slot libre si existe; si no, anade uno nuevo al vector.
         */
        GcHandle new_handle(uint8_t *addr);

        /**
         * @brief Marca el handle @p h como libre y lo recicla en free_handles_.
         * No libera la memoria del objeto.
         */
        void release_handle(GcHandle h);

        /**
         * @brief Asigna @p total_bytes en OldGen via ArenaManager.
         *
         * Orden de busqueda:
         *   1. Slots DEAD en bloques existentes cuyo tamano sea suficiente.
         *   2. Espacio libre al final del ultimo bloque (bump).
         *   3. Nuevo bloque pedido al ArenaManager.
         *
         * @param total_bytes Tamano total incluyendo GcHeader y padding.
         * @return Puntero al inicio del slot, o nullptr si OOM.
         */
        uint8_t *alloc_in_old(size_t total_bytes);

        /**
         * @brief Evacua el objeto referenciado por @p h de Nursery a OldGen.
         *
         * Si el objeto ya es BLACK o ya esta en OldGen, no hace nada.
         * Tras la evacuacion: el handle apunta al nuevo slot en OldGen,
         * el objeto original se marca BLACK (forward pointer en payload),
         * y se actualizan las estadisticas de promocion.
         *
         * @param h Handle del objeto a evacuar.
         */
        void do_evacuate(GcHandle h);

        /**
         * @brief Evacua el objeto referenciado por @p h de Nursery a OldGen.
         *
         * Wrapper que comprueba addr != nullptr antes de delegar en do_evacuate().
         * Los objetos ya en OldGen o ya marcados BLACK son ignorados.
         */
        void evacuate_object(GcHandle h);

        /**
         * @brief Escanea el payload del objeto @p h buscando handles YOUNG y los evacua.
         *
         * Cada palabra de 4 bytes del payload se interpreta como potencial GcHandle.
         * Si apunta a un objeto YOUNG no evacuado, llama a do_evacuate() y lo
         * anade al worklist para escaneo transitivo posterior (Cheney-style).
         */
        void scan_young_refs(GcHandle h, std::vector<GcHandle> &worklist);

        /**
         * @brief Marca transitivamente todos los objetos OLD alcanzables desde @p h.
         *
         * Escanea el payload de @p h. Si alguna palabra de 4 bytes es un handle
         * valido con addr != nullptr que apunta a un objeto OLD WHITE, lo marca
         * BLACK y lo anade al worklist para propagacion.
         */
        void mark_reachable(GcHandle h, std::vector<GcHandle> &worklist);
    };
} // namespace gc

#endif // GC_HEAP_H
