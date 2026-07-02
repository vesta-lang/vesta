/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file loader/interp_stackmap.h
 * @brief Stackmaps PRECISOS para frames del INTERPRETE (Phase E.1 + E.2).
 *
 * = Motivacion =
 *
 * El GC del interprete escanea la pila VM + los registros de forma
 * CONSERVADORA (@c gc_heap::scan_stack_roots): marca vivo cualquier
 * valor que "parezca" un handle o host_ptr.  Esto retiene objetos mas
 * de la cuenta (falsos positivos por host_ptrs residuales en registros
 * de callees ya retornados) -> colecta no-determinista.
 *
 * Phase E introduce stackmaps PRECISOS: por cada SAFEPOINT del bytecode
 * (PC donde el GC puede correr -- sitios de alocacion), una lista de las
 * ubicaciones (registros / slots de pila) que contienen raices GC vivas
 * en ese punto.  El GC del interprete consulta el stackmap del PC de
 * cada frame y marca SOLO esas ubicaciones (preciso, sin falsos
 * positivos por registros muertos).
 *
 * = Modelo espejo del JIT =
 *
 * El JIT ya es preciso (Phase D.2): @c jit::StackmapSlot / @c Stackmap /
 * @c scan_jit_frames.  El interprete replica el mismo diseno, con dos
 * diferencias:
 *   - Los stackmaps del interprete son DATOS (PC -> ubicaciones); el
 *     scan es C++ portable (NADA de codegen nativo) -> arch-independiente.
 *   - Las ubicaciones son registros VM (R0..R15) o slots de spill
 *     (@c rbp - (slot+1)*8), no offsets desde RBP nativo.
 *
 * = Ubicacion de un valor GC en el interprete =
 *
 * Segun como lo asigna el @c ir_emitter:
 *   - Valor @c is_gc_object en un registro VM R0..R15: el registro
 *     contiene un HOST_PTR al payload (kind HOSTPTR).
 *   - Valor @c is_gc_object derramado (spill) a un slot: el slot en
 *     @c rbp-(slot+1)*8 contiene el GcHandle (kind HANDLE) -- el
 *     emisor convierte host_ptr -> handle al derramar (estable a
 *     evacuacion del GC).
 *
 * = Formato binario de la seccion @c VSMP (en el @c .velb) =
 *
 *   +0  [4]  magic "VSMP" (0x504D5356 en LE)
 *   +4  [2]  version u16 (= 1)
 *   +6  [2]  reserved u16
 *   +8  [4]  entry_count u32
 *   +12 [..] entradas:
 *              u32 pc_offset      (offset absoluto de bytecode en el .velb)
 *              u16 slot_count
 *              slot_count * { u8 location, u8 gc_kind }
 *
 * @c location codifica reg vs slot:
 *   - 0x00..0x0F: registro VM R<location>.
 *   - 0x40 + n  : slot de spill n (valor en @c rbp - (n+1)*8).
 * @c gc_kind reusa la enum @c jit::StackmapGcKind (HANDLE=0/HOSTPTR=1/
 * STRING=2).
 *
 * Backward-compat: si el @c .velb no lleva seccion @c VSMP, la tabla
 * queda vacia y el scan preciso es no-op (el conservador cubre todo).
 */

#ifndef VESTA_LOADER_INTERP_STACKMAP_H
#define VESTA_LOADER_INTERP_STACKMAP_H

#include <algorithm>
#include <cstdint>
#include <vector>

#include "jit/machine_ir.h" // jit::StackmapGcKind (reusamos la enum)

namespace loader {

/// Magic de la seccion @c VSMP en el @c .velb ("VSMP" en little-endian).
static constexpr uint32_t INTERP_STACKMAP_MAGIC = 0x504D5356u;

/// Version del formato de la seccion @c VSMP.
static constexpr uint16_t INTERP_STACKMAP_VERSION = 1u;

/// Base para codificar un slot de spill en el byte @c location.
/// @c location < 0x40 es un registro VM; @c location en [0x40, 0x80) es un
/// slot de spill con indice @c location - INTERP_SM_SLOT_BASE.
static constexpr uint8_t INTERP_SM_SLOT_BASE = 0x40u;

/// Base para codificar un handle GC empujado a traves de un CALL, localizado
/// RELATIVO AL RBP DEL CALLEE (no del caller).  @c location >= 0x80 es un
/// handle en @c callee_rbp + 16 + 8*(location - INTERP_SM_PUSH_BASE).
///
/// Robusto ante ALLOCA en el caller: los empujes pre-call quedan por ENCIMA
/// del @c saved_rbp + @c return_pc que el @c enter del callee dejo, asi que su
/// offset desde @c callee_rbp es FIJO independientemente de cuanto haya bajado
/// rsp el caller por sus ALLOCA.  El scan usa el rbp del frame ACTUAL (el
/// callee) para materializarlos, no el caller_rbp.
static constexpr uint8_t INTERP_SM_PUSH_BASE = 0x80u;

/// Offset (bytes) del PRIMER handle empujado desde el rbp del callee:
/// @c [callee_rbp] = saved_rbp, @c [callee_rbp+8] = return_pc, y el empuje
/// mas alto (topmost, ultimo empujado antes del call) queda en @c callee_rbp+16.
static constexpr uint32_t INTERP_SM_PUSH_FIRST_OFF = 16u;

/**
 * @struct InterpStackmapSlot
 * @brief Una ubicacion (registro o slot de spill) que contiene una raiz
 *        GC viva en un safepoint del interprete.  2 bytes empaquetados.
 */
struct InterpStackmapSlot {
    uint8_t location = 0; ///< <0x40 = reg R<location>; >=0x40 = slot (location-0x40)
    jit::StackmapGcKind gc_kind =
        jit::StackmapGcKind::HANDLE; ///< categoria del valor

    /// True si esta ubicacion es un registro VM (R0..R15).
    bool is_reg() const noexcept { return location < INTERP_SM_SLOT_BASE; }
    /// True si es un slot de spill (caller-rbp relativo).
    bool is_spill() const noexcept {
        return location >= INTERP_SM_SLOT_BASE && location < INTERP_SM_PUSH_BASE;
    }
    /// True si es un handle empujado a traves de un call (callee-rbp relativo).
    bool is_pushed() const noexcept { return location >= INTERP_SM_PUSH_BASE; }
    /// Indice de registro (valido solo si @c is_reg()).
    uint8_t reg_index() const noexcept { return location; }
    /// Indice de slot de spill (valido solo si @c is_spill()).
    uint8_t slot_index() const noexcept {
        return static_cast<uint8_t>(location - INTERP_SM_SLOT_BASE);
    }
    /// Posicion desde el tope (0=topmost) de un handle empujado (si @c
    /// is_pushed()).  Offset = @c callee_rbp + INTERP_SM_PUSH_FIRST_OFF + 8*k.
    uint8_t push_index() const noexcept {
        return static_cast<uint8_t>(location - INTERP_SM_PUSH_BASE);
    }
};

/**
 * @struct InterpStackmap
 * @brief Snapshot de raices GC vivas en un safepoint (PC de bytecode).
 */
struct InterpStackmap {
    uint32_t pc_offset = 0; ///< offset absoluto de bytecode en el .velb
    std::vector<InterpStackmapSlot> slots;
};

/**
 * @class InterpStackmapTable
 * @brief Tabla PC -> stackmap de un ejecutable, ordenada por @c pc_offset
 *        para lookup por busqueda binaria O(log N).
 *
 * Vive en @c Executable (poblada por el loader al parsear la seccion
 * @c VSMP).  El GC la consulta con el PC de cada frame del interprete.
 */
class InterpStackmapTable {
  public:
    /// True si no hay stackmaps (ejecutable sin seccion @c VSMP).
    bool empty() const noexcept { return maps_.empty(); }
    size_t size() const noexcept { return maps_.size(); }

    /// Anade un stackmap.  Debe llamarse en orden ascendente de @c pc_offset
    /// (el loader emite las entradas ya ordenadas por el linker).
    void add(InterpStackmap sm) { maps_.push_back(std::move(sm)); }

    /// Ordena por @c pc_offset (idempotente; el loader lo llama tras cargar).
    /// Inline (header-only) para no requerir una TU dedicada en cada target
    /// que enlaza el loader.
    void finalize() {
        std::sort(maps_.begin(), maps_.end(),
                  [](const InterpStackmap &a, const InterpStackmap &b) {
                      return a.pc_offset < b.pc_offset;
                  });
    }

    /**
     * @brief Busca el stackmap cuyo @c pc_offset coincide EXACTAMENTE con
     *        @p pc.  Devuelve nullptr si no hay match.
     *
     * El PC de un frame del interprete apunta AL inicio de la instruccion
     * de safepoint (el emisor registra el offset del inicio del safepoint),
     * por lo que exigimos coincidencia exacta -- a diferencia del JIT, cuyo
     * RIP capturado suele apuntar DESPUES del poll.  Busqueda binaria
     * O(log N) sobre @c maps_ (ordenado por @c finalize).
     */
    const InterpStackmap *lookup_exact(uint32_t pc) const noexcept {
        if (maps_.empty()) return nullptr;
        size_t lo = 0, hi = maps_.size();
        while (lo < hi) {
            const size_t mid = lo + (hi - lo) / 2;
            const uint32_t k = maps_[mid].pc_offset;
            if (k == pc) return &maps_[mid];
            if (k < pc)
                lo = mid + 1;
            else
                hi = mid;
        }
        return nullptr;
    }

  private:
    std::vector<InterpStackmap> maps_; ///< ordenado por pc_offset
};

} // namespace loader

#endif // VESTA_LOADER_INTERP_STACKMAP_H
