/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 */

/**
 * @file jit/sched/machine_sched.cpp
 * @brief List scheduler machine-level (C2.15).  Reordena las MInstr de cada
 *        bloque respetando el DAG de dependencias (efectos COMPLETOS de
 *        @ref machine_effects + desambiguacion de memoria real) y priorizando
 *        por camino critico + latencia (@ref SchedCostModel), para ocultar
 *        latencias y exponer ILP.
 *
 * Memoria: NO se serializa a lo bruto.  Cada acceso se resuelve a una
 * referencia @c MemRef (base + desplazamiento + tamano + objeto de procedencia)
 * y dos accesos solo dependen si de verdad pueden SOLAPARSE:
 *   - objetos de procedencia distintos (ALLOCAs diferentes) -> nunca aliasan.
 *   - misma base con rangos [disp,disp+size) disjuntos -> nunca aliasan.
 *   - solo los punteros genuinamente desconocidos (o accesos de ancho opaco:
 *     rep-movs, atomicos, call) se tratan como que aliasan -> eso es la
 *     semantica correcta sin un points-to global, no un atajo.
 */

#include "jit/sched/machine_sched.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace jit {
namespace sched {

namespace {

// ---------------------------------------------------------------------------
// Desambiguacion de memoria (alias analysis)
// ---------------------------------------------------------------------------

/// Clave de registro uniforme de un operando REG/VREG (o UINT32_MAX si no).
uint32_t op_reg_key(const MOperand &o) {
    if (o.kind == MOperandKind::REG) return o.reg;
    if (o.kind == MOperandKind::VREG)
        return MEffects::VREG_BASE + static_cast<uint32_t>(o.value);
    return UINT32_MAX;
}

/// Objeto de procedencia de una direccion: 0=desconocido, 1=frame (rbp/rsp),
/// >=2 = objetos ALLOCA distintos (regiones disjuntas del frame).
enum : uint32_t { OBJ_UNKNOWN = 0, OBJ_FRAME = 1, OBJ_FIRST_ALLOCA = 2 };

/// Referencia a memoria de una instruccion.
struct MemRef {
    bool touches = false;   ///< la instruccion accede a memoria
    bool reads = false;
    bool writes = false;
    uint32_t base = UINT32_MAX; ///< registro base (reg/vreg) de la direccion
    uint32_t object = OBJ_UNKNOWN; ///< objeto de procedencia del base
    int64_t disp = 0;       ///< desplazamiento constante
    int32_t size = 8;       ///< bytes accedidos
    bool exact = false;     ///< disp/size conocidos y sin index -> rango exacto
};

/// Ancho de acceso de un operando MEM (bytes); 0/desconocido -> 8 (sobre-estima
/// el rango, que es SEGURO para el test de solapamiento).
int32_t mem_size_bytes(const MOperand &m) {
    const int32_t s = m.flags; // mem_size override (bytes) si != 0
    return (s == 1 || s == 2 || s == 4 || s == 8) ? s : 8;
}

/// ¿El operando MEM tiene index? (index != NONE en los bits altos de width).
bool mem_has_index(const MOperand &m) {
    return ((m.width >> 2) & 0x3F) != static_cast<uint8_t>(MReg::NONE);
}

/**
 * @brief Extrae la @c MemRef de @p mi usando el mapa de procedencia @p obj_of.
 *        Cubre las tres formas de acceso del MachineIR:
 *          - operando MEM real (base+index*scale+disp).
 *          - pseudos LOAD/STORE(+_VM): la direccion es un vreg (src1).
 *          - accesos de ancho opaco (rep-movs/atomicos): base desconocida.
 */
MemRef extract_memref(const MInstr &mi, const MEffects &e,
                      const std::unordered_map<uint32_t, uint32_t> &obj_of) {
    MemRef r;
    r.touches = e.reads_mem || e.writes_mem;
    if (!r.touches) return r;
    r.reads = e.reads_mem;
    r.writes = e.writes_mem;

    auto obj = [&](uint32_t base) -> uint32_t {
        auto it = obj_of.find(base);
        return it == obj_of.end() ? OBJ_UNKNOWN : it->second;
    };

    // Pseudos con direccion en src1 (vreg): LOAD/STORE/LOAD_VM/STORE_VM.
    switch (mi.op) {
    case MOp::LOAD:
    case MOp::LOAD_VM:
    case MOp::STORE:
    case MOp::STORE_VM: {
        r.base = op_reg_key(mi.src1);
        r.object = obj(r.base);
        r.disp = 0;
        // LOAD: flags=(width<<1)|signed; STORE: flags=width.
        const int w = (mi.op == MOp::LOAD || mi.op == MOp::LOAD_VM)
                          ? (mi.flags >> 1)
                          : mi.flags;
        r.size = (w == 1 || w == 2 || w == 4 || w == 8) ? w : 8;
        r.exact = (r.base != UINT32_MAX);
        return r;
    }
    default:
        break;
    }

    // Operando MEM real (busca en dst/src1/src2).
    for (const MOperand *m : {&mi.dst, &mi.src1, &mi.src2}) {
        if (m->kind != MOperandKind::MEM) continue;
        r.base = m->reg;
        r.object = obj(r.base);
        r.disp = m->value;
        r.size = mem_size_bytes(*m);
        r.exact = !mem_has_index(*m); // con index el offset no es constante
        return r;
    }

    // Toca memoria pero sin referencia identificable (rep-movs, atomicos con
    // direccion fija en un reg, etc.): base desconocida -> aliasa por seguridad
    // semantica (no hay forma de probar disjuncion).
    return r;
}

/// ¿Pueden SOLAPARSE dos referencias a memoria?
bool may_alias(const MemRef &a, const MemRef &b) {
    // Objetos ALLOCA distintos = regiones disjuntas del frame -> nunca aliasan.
    if (a.object >= OBJ_FIRST_ALLOCA && b.object >= OBJ_FIRST_ALLOCA &&
        a.object != b.object)
        return false;
    // Misma base y ambos rangos exactos -> test de solapamiento [d,d+size).
    if (a.base != UINT32_MAX && a.base == b.base && a.exact && b.exact) {
        const int64_t ae = a.disp + a.size, be = b.disp + b.size;
        return a.disp < be && b.disp < ae; // solapan
    }
    // Sin mas informacion, dos direcciones pueden ser iguales -> aliasan.
    return true;
}

// ---------------------------------------------------------------------------
// DAG de dependencias
// ---------------------------------------------------------------------------

/// ¿Comparten alguna clave de registro dos conjuntos (pequenos)?
bool intersects(const std::vector<uint32_t> &a,
                const std::vector<uint32_t> &b) {
    for (uint32_t x : a)
        for (uint32_t y : b)
            if (x == y) return true;
    return false;
}

/**
 * @brief ¿Hay dependencia de @p a (antes) a @p b (despues)?  Modela TODOS los
 *        hazards: RAW/WAR/WAW en registros, dependencias de FLAGS y memoria con
 *        alias analysis (solo dependen si pueden solaparse).
 */
bool depends(const MEffects &ea, const MEffects &eb, const MemRef &ma,
             const MemRef &mb) {
    if (intersects(ea.writes, eb.reads)) return true;  // RAW
    if (intersects(ea.reads, eb.writes)) return true;  // WAR
    if (intersects(ea.writes, eb.writes)) return true; // WAW
    if (ea.writes_flags && eb.reads_flags) return true;
    if (ea.reads_flags && eb.writes_flags) return true;
    if (ea.writes_flags && eb.writes_flags) return true;
    // Memoria: solo si al menos uno ESCRIBE y las referencias pueden solapar.
    if (ma.touches && mb.touches && (ma.writes || mb.writes) &&
        may_alias(ma, mb))
        return true;
    return false;
}

/**
 * @brief Reordena en sitio las instrucciones [lo,hi) de @p ins (una REGION sin
 *        barreras) por list scheduling.
 * @return numero de instrucciones que cambiaron de posicion.
 */
int schedule_region(std::vector<MInstr> &ins, int lo, int hi,
                    const std::vector<MEffects> &eff,
                    const std::vector<MemRef> &refs, const SchedCostModel &cm) {
    const int n = hi - lo;
    if (n <= 1) return 0;

    // DAG: succ[i] = dependientes de i; indeg[i] = num. de predecesores.
    std::vector<std::vector<int>> succ(n);
    std::vector<int> indeg(n, 0);
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            if (depends(eff[lo + i], eff[lo + j], refs[lo + i], refs[lo + j])) {
                succ[i].push_back(j);
                ++indeg[j];
            }

    // Latencia por nodo (del modelo de coste).
    std::vector<float> lat(n);
    for (int i = 0; i < n; ++i) lat[i] = cm.cost(ins[lo + i]).latency;

    // Altura = camino critico (latencia) hasta el final de la region.
    std::vector<float> height(n, 0.0f);
    for (int i = n - 1; i >= 0; --i) {
        float mx = 0.0f;
        for (int s : succ[i]) mx = std::max(mx, height[s]);
        height[i] = lat[i] + mx;
    }

    // List scheduling dirigido por ciclos.
    std::vector<int> ready_cycle(n, 0);
    std::vector<char> done(n, 0);
    std::vector<int> order;
    order.reserve(n);
    int cur_cycle = 0;

    for (int placed = 0; placed < n; ++placed) {
        int best = -1;
        for (int i = 0; i < n; ++i) {
            if (done[i] || indeg[i] != 0) continue;
            if (best < 0) { best = i; continue; }
            const bool i_av = ready_cycle[i] <= cur_cycle;
            const bool b_av = ready_cycle[best] <= cur_cycle;
            if (i_av != b_av) { if (i_av) best = i; continue; }
            if (height[i] != height[best]) { if (height[i] > height[best]) best = i; continue; }
            if (ready_cycle[i] != ready_cycle[best]) { if (ready_cycle[i] < ready_cycle[best]) best = i; continue; }
            if (i < best) best = i;
        }
        done[best] = 1;
        order.push_back(best);
        const int start = std::max(cur_cycle, ready_cycle[best]);
        const int finish = start + static_cast<int>(lat[best] + 0.5f);
        cur_cycle = start + 1;
        for (int s : succ[best]) {
            ready_cycle[s] = std::max(ready_cycle[s], finish);
            --indeg[s];
        }
    }

    // Reescribe la region con el nuevo orden; cuenta los que se movieron.
    std::vector<MInstr> tmp;
    tmp.reserve(n);
    int moved = 0;
    for (int k = 0; k < n; ++k) {
        if (order[k] != k) ++moved;
        tmp.push_back(ins[lo + order[k]]);
    }
    for (int k = 0; k < n; ++k) ins[lo + k] = std::move(tmp[k]);
    return moved;
}

/**
 * @brief Construye el mapa registro/vreg -> objeto de procedencia de toda la
 *        funcion (una pasada; los vregs se definen una vez pre-regalloc).
 *        ALLOCA -> objeto distinto; rbp/rsp -> frame; MOV/LEA de una fuente
 *        propagan el objeto.
 */
std::unordered_map<uint32_t, uint32_t> build_provenance(const MFunction &mf) {
    std::unordered_map<uint32_t, uint32_t> obj;
    obj[static_cast<uint32_t>(MReg::RBP)] = OBJ_FRAME;
    obj[static_cast<uint32_t>(MReg::RSP)] = OBJ_FRAME;
    uint32_t next_alloca = OBJ_FIRST_ALLOCA;
    for (const MBlock &blk : mf.blocks) {
        for (const MInstr &mi : blk.instrs) {
            const uint32_t d = op_reg_key(mi.dst);
            if (d == UINT32_MAX) continue;
            switch (mi.op) {
            case MOp::ALLOCA:
            case MOp::ALLOCA_VM:
                obj[d] = next_alloca++;
                break;
            case MOp::MOV:
            case MOp::LEA: {
                // dst hereda el objeto de la base de la fuente.
                uint32_t src = op_reg_key(mi.src1);
                if (src == UINT32_MAX && mi.src1.kind == MOperandKind::MEM)
                    src = mi.src1.reg;
                auto it = obj.find(src);
                if (it != obj.end()) obj[d] = it->second;
                break;
            }
            default:
                break;
            }
        }
    }
    return obj;
}

} // namespace

int schedule_function(MFunction &mf, const SchedCostModel &cm, EffIsa isa) {
    const std::unordered_map<uint32_t, uint32_t> prov = build_provenance(mf);
    int total_moved = 0;
    for (MBlock &blk : mf.blocks) {
        std::vector<MInstr> &ins = blk.instrs;
        const int n = static_cast<int>(ins.size());
        if (n <= 1) continue;

        // Efectos + referencia de memoria por instruccion (una sola pasada).
        std::vector<MEffects> eff(n);
        std::vector<MemRef> refs(n);
        for (int i = 0; i < n; ++i) {
            eff[i] = machine_effects(ins[i], isa);
            refs[i] = extract_memref(ins[i], eff[i], prov);
        }

        // Parte el bloque en REGIONES delimitadas por barreras.
        int lo = 0;
        for (int i = 0; i < n; ++i) {
            if (eff[i].is_barrier) {
                total_moved += schedule_region(ins, lo, i, eff, refs, cm);
                lo = i + 1; // la barrera queda en su sitio
            }
        }
        total_moved += schedule_region(ins, lo, n, eff, refs, cm);
    }
    return total_moved;
}

} // namespace sched
} // namespace jit
