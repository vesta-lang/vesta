/*
 * VestaVM - Maquina Virtual Distribuida
 * Copyright (C) 2026 David Lopez.T (DesmonHak); Licencia: GPLv2 + excepcion de runtime (ver LICENSE)
 */
/**
 * @file include/runtime/profile.h
 * @brief Sprint D.6 (2026-06-03): profile counters runtime para PGO + C2.
 *
 * Sistema de instrumentacion runtime que recolecta tres tipos de
 * informacion del programa en ejecucion, necesaria para que el C2 JIT
 * (Phase D.8) tome decisiones especulativas seguras:
 *
 *   1. Branch frequency (taken / not_taken counts en BR_COND/jcc).
 *      Permite layout de codigo + branch hints en codegen.
 *
 *   2. CALLVIRT type observation (clases vistas + count por call site).
 *      Permite devirtualizacion en C2 cuando un call site es
 *      monomorfico o polimorfico-estable (PIC con guard + deopt).
 *
 *   3. Allocation count (NEWOBJ count per pc).
 *      Permite escape analysis priorizado en C2 (focus en allocs
 *      mas frecuentes).
 *
 * = Coste runtime =
 *
 * Fast path cuando profile inactivo (default):
 *   if (UNLIKELY(g_profile_active)) { ... }
 * = 1 atomic load relaxed + 1 branch predicted-not-taken = ~1 cycle.
 * Programs sin --profile no pagan overhead real.
 *
 * Fast path cuando profile activo:
 *   Branch: 1 atomic fetch_add relaxed (~2-5 cycles).
 *   Callvirt: lookup + 1 atomic fetch_add (mutex solo si grow).
 *   Alloc: 1 atomic fetch_add.
 *
 * = Persistencia =
 *
 * Al exit del proceso (atexit), si --profile path se especifico,
 * dump al `.vprof` binario:
 *
 *   Header: magic 'VPRF' (4B) + version u16 + flags u16 +
 *           n_branches u32 + n_callsites u32 + n_allocs u32 +
 *           reserved[12]
 *   Branches:  [pc u64][taken u64][not_taken u64]  (24B per entry)
 *   CallSites: [pc u64][n_types u8][_pad 7B][megamorphic u64][
 *              (class_name string + count u64) x n_types]
 *   Allocs:    [pc u64][count u64]
 *
 * Phase D.10 PGO persistence carga el `.vprof` al startup para
 * warm-start del C2 (compile preemptive de fns hot del run anterior).
 *
 * = Concurrencia =
 *
 * Counters son @c std::atomic<uint64_t> con @c memory_order_relaxed.
 * En x86 esto es identico a un store/load no atomic (cero overhead).
 * Para multi-thread, el orden de actualizaciones entre threads no
 * importa (solo conteos agregados).  Lookup de @c CallSiteCounter
 * necesita mutex porque la lista de class_ptr observados puede crecer
 * dinamicamente (max 4 entries antes de megamorfico).
 */
#ifndef VESTA_RUNTIME_PROFILE_H
#define VESTA_RUNTIME_PROFILE_H

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <functional>
#include <string>
#include <unordered_map>

namespace loader {
struct ClassInfo;
}

namespace runtime {
namespace profile {

/**
 * @struct BranchCounter
 * @brief Contador per-PC de un branch condicional (BR_COND / jcc).
 *
 * Incrementa @c taken o @c not_taken segun el resultado de la
 * condicion al ejecutar el branch.  Counters son @c atomic con
 * @c memory_order_relaxed.
 */
struct BranchCounter {
    std::atomic<uint64_t> taken{0};
    std::atomic<uint64_t> not_taken{0};

    BranchCounter() = default;
    // No copyable porque @c atomic no es copyable.
    BranchCounter(const BranchCounter &) = delete;
    BranchCounter &operator=(const BranchCounter &) = delete;
    BranchCounter(BranchCounter &&other) noexcept {
        taken.store(other.taken.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
        not_taken.store(other.not_taken.load(std::memory_order_relaxed),
                        std::memory_order_relaxed);
    }
};

/**
 * @struct TypeObservation
 * @brief Una clase vista en un call site con su count de hits.
 *
 * El @c class_ptr es un puntero opaco a @c loader::ClassInfo (se
 * resuelve via @c ClassRegistry).  Para persistencia cross-run,
 * el dump serializa el nombre de la clase (no el puntero).
 */
struct TypeObservation {
    loader::ClassInfo *class_ptr = nullptr;
    uint64_t count = 0;
    /// Nombre cacheado al momento de la PRIMERA observacion.  El
    /// ClassRegistry se destruye antes que el atexit handler, asi
    /// que NO podemos dereferenciar @c class_ptr->name al dump.
    std::string class_name;
};

/**
 * @struct CallSiteCounter
 * @brief Observaciones de tipo en un CALLVIRT/CALLM/CALLCLOSURE.
 *
 * Mantiene hasta 4 clases distintas (suficiente para PIC monomorfico
 * y polimorfico-estable).  Cuando se observa una 5ta clase, marca
 * @c megamorphic_count++ y no agrega nueva entrada.  C2 puede
 * decidir devirtualizar si:
 *   - n_types == 1 (monomorfico) -> direct call + guard
 *   - n_types <= 4 + megamorphic == 0 (estable) -> PIC inline
 *   - megamorphic > 0 -> dispatch normal via vtable
 */
struct CallSiteCounter {
    std::array<TypeObservation, 4> types{};
    uint64_t megamorphic_count = 0;
    uint8_t n_types = 0;
};

/**
 * @struct ProfileCollector
 * @brief Singleton runtime que mantiene todos los counters.
 *
 * Lifetime: instanciado al inicio si @c profile_init() se llamo.
 * Dump al @c .vprof file al exit (atexit handler o llamada
 * explicita a @c profile_dump()).
 */
struct ProfileCollector {
    /// PC -> contador taken/not_taken.  Atomic counters; el map
    /// crece bajo @c collector_mtx.
    std::unordered_map<uint64_t, BranchCounter> branches;

    /// PC -> contador per-class.  Acceso completo bajo @c collector_mtx
    /// porque la lista de tipos puede crecer.
    std::unordered_map<uint64_t, CallSiteCounter> callsites;

    /// PC del NEWOBJ -> count.  Atomic.
    std::unordered_map<uint64_t, std::atomic<uint64_t>> allocs;

    /// Mutex compartido para todos los inserts en los maps.
    /// Lookup por iterator es lock-free (pero el iterator
    /// puede invalidarse si otro thread inserta).  Usamos lock
    /// shared para read si llegamos a multi-thread; por ahora
    /// lock simple en hot path.
    mutable std::mutex collector_mtx;

    /// Flag global; permite fast-path inline con cero overhead
    /// cuando esta desactivado (default).  Set por
    /// @c profile_init().
    std::atomic<bool> active{false};

    /// Path al @c .vprof de salida.  Vacio = no dump.
    std::string output_path;
};

/// Instancia singleton.  Sin TLS: un solo set de counters por
/// proceso.  Inicializado lazy en @c profile_init().
extern ProfileCollector g_profile;

// ===========================================================================
// Profiler LIGERO de branches (lock-free) para el auto-PGO del JIT (default-ON)
// ===========================================================================
//
// El profiler D.6 (arriba) usa mutex + unordered_map por PC: correcto para el
// dump completo del `.vprof`, pero demasiado pesado para dejarlo SIEMPRE activo
// en tier-0 (el interp).  El auto-PGO del JIT necesita datos de branches
// recolectados durante tier-0 SIN penalizar; para eso este profiler ligero:
//
//   - Tabla de tamano FIJO (potencia de 2) indexada por hash del PC.  Cero
//     asignacion dinamica, cero mutex.
//   - Cada entrada: {pc, taken, not_taken} atomicos (relaxed).  Insercion via
//     un CAS del slot vacio; colision con otro PC -> se DESCARTA la muestra
//     (perfil aproximado, aceptable para una heuristica de rentabilidad).
//   - Fast path OFF: 1 load relaxed + branch predicho -> ~1 ciclo.  Se puede
//     dejar activo siempre que el JIT este habilitado sin coste apreciable.
//
// Es un perfil APROXIMADO por diseno (colisiones se pierden); para el PGO de la
// if-conversion basta el orden de magnitud de P(mispredict) por linea.

/// @brief Entrada del profiler ligero (una ranura de la tabla fija).
struct LiteBranchSlot {
    std::atomic<uint64_t> pc{0}; ///< PC del branch; 0 = ranura vacia.
    std::atomic<uint32_t> taken{0};
    std::atomic<uint32_t> not_taken{0};
};

/// Numero de ranuras (potencia de 2).  16384 * 16B = 256 KB estaticos.
constexpr size_t kLiteBranchSlots = 1u << 14;
constexpr uint64_t kLiteBranchMask = kLiteBranchSlots - 1;

/// Tabla estatica del profiler ligero + flag de actividad.
extern LiteBranchSlot g_lite_branches[kLiteBranchSlots];
extern std::atomic<bool> g_lite_active;

/// @brief Activa/desactiva el profiler ligero (lo hace @c main al habilitar el
///        JIT).  Idempotente.
void lite_profile_set_active(bool on);

/// @brief True si el profiler ligero esta recolectando.
inline bool lite_profile_active() {
    return g_lite_active.load(std::memory_order_relaxed);
}

/**
 * @brief Registra un branch en el profiler ligero (lock-free).  Fast path OFF
 *        = 1 load + branch.  ON = hash + 1-2 atomicas relaxed.  Colision con
 *        otro PC -> muestra descartada (perfil aproximado por diseno).
 * @param pc    PC del branch (VM address).
 * @param taken True si la condicion fue verdadera.
 */
inline void lite_profile_branch(uint64_t pc, bool taken) {
    if (!g_lite_active.load(std::memory_order_relaxed)) return; // fast path OFF
    // Hash de Fibonacci (multiplicativo) -> buena dispersion de PCs.
    const uint64_t h = (pc * 0x9E3779B97F4A7C15ull) >> (64 - 14);
    LiteBranchSlot &slot = g_lite_branches[h & kLiteBranchMask];
    uint64_t cur = slot.pc.load(std::memory_order_relaxed);
    if (cur != pc) {
        if (cur != 0) return; // ranura ocupada por otro PC: descartar muestra
        uint64_t expected = 0; // reclamar la ranura vacia
        if (!slot.pc.compare_exchange_strong(expected, pc,
                                              std::memory_order_relaxed) &&
            expected != pc) {
            return; // otro thread la reclamo con un PC distinto: descartar
        }
    }
    if (taken)
        slot.taken.fetch_add(1, std::memory_order_relaxed);
    else
        slot.not_taken.fetch_add(1, std::memory_order_relaxed);
}

/**
 * @brief Inicializa el profiler con un path de salida.
 *
 * Setea @c g_profile.active=true y registra un handler @c atexit
 * para dump automatico al exit.  Llamado por @c main.cpp cuando
 * el flag CLI @c --profile <path> esta presente, o por env var
 * @c VESTA_PROFILE_DUMP=path.
 *
 * Idempotente: si ya esta inicializado con el mismo path, no-op.
 *
 * @param output_path  Ruta del @c .vprof a generar.  Vacio = solo
 *                     instrumentacion, sin dump.
 */
void profile_init(const std::string &output_path);

/**
 * @brief Volcar todos los counters al @c output_path como @c .vprof.
 *
 * Llamado automaticamente por el atexit handler.  Tambien expuesto
 * para tests o dumps intermedios.
 */
void profile_dump();

/**
 * @brief Escribe un perfil de branches indexado por LINEA FUENTE (para PGO de
 *        la if-conversion, consumido por @c ir::load_branch_profile).
 *
 * Recorre @c g_profile.branches (contadores por PC), mapea cada PC a su linea
 * fuente con @p pc_to_line (0 = sin linea; se descarta), agrega taken/not_taken
 * por linea y escribe el formato de texto @c "<line> <taken> <nt>".  Se llama
 * tras la ejecucion, con la VM viva (el @c debug_info sigue accesible).
 *
 * @param path       Ruta del perfil de texto a escribir.
 * @param pc_to_line Callback PC (VM) -> linea fuente (0 si desconocida).
 * @return numero de lineas escritas.
 */
int profile_write_branch_lines(const std::string &path,
                               const std::function<uint32_t(uint64_t)> &pc_to_line);

/**
 * @brief Bridge desacoplado runtime -> IR: vuelca el perfil de branches medido
 *        (contadores por PC) al almacen por-linea que consume la if-conversion
 *        (@c ir::set_branch_profile_entry), SIN pasar por un archivo.
 *
 * Es la pieza que mantiene al JIT VM-INDEPENDIENTE: el JIT (if-conversion +
 * politica de SELECT) solo lee @c ir::g_branch_profile (datos planos, sin tipos
 * de la VM); este bridge, del lado runtime, es el UNICO que toca @c debug_info
 * (via @p pc_to_line) para alimentarlo.  Asi el auto-PGO no acopla el JIT a la
 * VM.  P(mispredict) = min(taken,nt)/(taken+nt) por linea agregada.
 *
 * @param pc_to_line Callback PC (VM) -> linea fuente (0 si desconocida).
 * @return numero de lineas volcadas al almacen de la if-conversion.
 */
int profile_apply_branch_lines(const std::function<uint32_t(uint64_t)> &pc_to_line);

/**
 * @brief Registra el resultado de un branch condicional.
 *
 * Llamado desde @c exec_instr_jmp (cuando cond != 0x0F que es
 * incondicional) y desde los opcodes fusionados @c cmpjmp /
 * @c cmpjmpu / @c decjnz.
 *
 * @param pc     PC del branch (instr.rip antes del jump).
 * @param taken  True si la condicion fue verdadera.
 */
void profile_branch(uint64_t pc, bool taken);

/**
 * @brief Registra un tipo observado en un call site virtual.
 *
 * Llamado desde @c exec_instr_callvirt / @c exec_instr_callm /
 * @c exec_instr_callclosure tras resolver la clase del objeto.
 *
 * Si el call site ya tiene 4 tipos distintos y este es nuevo,
 * incrementa @c megamorphic_count.  Si el tipo ya esta en la lista,
 * incrementa su count.
 *
 * @param pc          PC del CALLVIRT.
 * @param class_ptr   Clase del receptor (obj->class_ptr).
 */
void profile_callvirt(uint64_t pc, loader::ClassInfo *class_ptr);

/**
 * @brief Registra una alocacion (NEWOBJ o GC_ALLOC).
 *
 * Llamado desde @c exec_instr_newobj y @c exec_instr_gcalloc.
 * Util para identificar hot allocs candidatos a stack allocation
 * por escape analysis en C2 (Phase D.8.b).
 *
 * @param pc  PC del NEWOBJ/GC_ALLOC.
 */
void profile_newobj(uint64_t pc);

/**
 * @brief Reset todos los counters (sin desactivar).
 *
 * Util para tests que quieren mediar solo una seccion.
 */
void profile_reset();

} // namespace profile
} // namespace runtime

#endif // VESTA_RUNTIME_PROFILE_H
