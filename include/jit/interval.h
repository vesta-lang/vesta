/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file jit/interval.h
 * @brief Construccion de live intervals sobre MachineIR para el register
 *        allocator ( D.7, commit 2).  Ver doc/REGALLOC.md.
 *
 * Un *live interval* describe, para cada registro virtual, EN QUE POSICIONES
 * lineales del codigo esta vivo (puede tener "holes": muere y revive).  Es la
 * entrada del linear-scan (commit 4): el allocator recorre los intervalos por
 * orden de inicio y asigna fisicos / spillea segun solapen o no.
 *
 * = Posiciones lineales =
 *
 * Cada instruccion ocupa DOS posiciones: @c use (par) y @c def (impar).  La
 * i-esima instruccion (orden global de layout) tiene @c use_pos = 2*i y
 * @c def_pos = 2*i+1.  Asi un valor USADO y RE-DEFINIDO en la misma instr no
 * solapa consigo mismo (su uso termina en use_pos, el nuevo def empieza en
 * def_pos) -- imprescindible para que dst pueda reusar el reg de un src
 * (two-address).
 *
 * = Liveness =
 *
 * Dataflow iterativo a punto fijo (gen/kill por bloque, live-in/out), que
 * maneja back-edges (loops) de forma robusta sin necesidad de deteccion de
 * loops.  Tras la liveness, los rangos se construyen en una pasada backward
 * por bloque usando @c live_out como conjunto vivo al final del bloque.  La
 * correccion NO depende del orden de bloques (solo la calidad del spilling).
 *
 * = Roles de operandos =
 *
 * El unico punto consciente del opcode: @c operand_roles(MOp) dice si cada
 * uno de @c {dst, src1, src2} es def, use, ambos o nada, asumiendo la forma
 * de TRES operandos pre-legalization (el dst de un ALU es def puro; la
 * two-address legalization es posterior).  En un futuro multi-arch esta
 * tabla se mueve a la capa del selector/target; hoy @c MOp es compartido.
 */

#ifndef VESTA_JIT_INTERVAL_H
#define VESTA_JIT_INTERVAL_H

#include "codegen/linear_pos.h"
#include "jit/machine_ir.h"
#include "jit/target_reginfo.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace jit {

/**
 * @struct LiveRange
 * @brief Rango semi-abierto [from, to) de posiciones lineales donde un
 *        vreg esta vivo.
 */
struct LiveRange {
    uint32_t from = 0; ///< posicion de inicio (inclusive)
    uint32_t to = 0;   ///< posicion de fin (exclusive)
};

/**
 * @struct LiveInterval
 * @brief Conjunto de rangos vivos de UN registro virtual, mas sus
 *        posiciones de uso (heuristica de spill).
 *
 * @c ranges queda ordenado ascendente y SIN solapes tras la
 * construccion.  Un interval vacio (@c ranges vacio) = vreg sin usos
 * (codigo muerto que el allocator ignora).
 */
struct LiveInterval {
    uint32_t vreg = UINT32_MAX;    ///< id del registro virtual
    RegClass cls = RegClass::GP;   ///< clase (GP/FP)
    std::vector<LiveRange> ranges; ///< rangos vivos, ordenados/disjuntos
    std::vector<uint32_t> uses;    ///< posiciones de uso (ascendente)
    ///  D.7 commit 6: categoria GC.  0 = no GC; 1+ = StackmapGcKind+1
    /// (1=HANDLE, 2=HOSTPTR, 3=STRING).  Si != 0 y el intervalo cruza un
    /// call, el allocator lo FUERZA a un slot (enfoque A) para que el GC
    /// lo describa via stackmap y lo escanee del stack.
    uint8_t gc_kind = 0;
    ///  AS inc.5: registro fisico FORZADO (precoloreo), o -1 si libre.
    /// Lo poblea @c build_intervals desde @c MFunction::vreg_fixed.  El
    /// @c linear_scan asigna este fisico exacto al intervalo de forma
    /// INCONDICIONAL (override del cross-call: el pin puede ser caller-saved
    /// porque el inline-asm lo necesita en ese registro concreto).
    int fixed_reg = -1;

    /// register-required (nivel intermedio).  El linear-scan DEBE darle
    /// un registro (que elige el) y NO lo derrama -- desaloja una victima si
    /// hace falta.  Lo poblea @c build_intervals desde
    /// @c MFunction::vreg_reg_required.  Ignorado si @c fixed_reg >= 0 (el pin
    /// ya garantiza el registro).
    bool reg_required = false;

    bool is_gc() const noexcept { return gc_kind != 0; }
    bool empty() const noexcept { return ranges.empty(); }

    /** @brief Primera posicion viva (o UINT32_MAX si vacio). */
    uint32_t start() const noexcept {
        return ranges.empty() ? UINT32_MAX : ranges.front().from;
    }
    /** @brief Posicion fin (exclusive) del ultimo rango (o 0 si vacio). */
    uint32_t end() const noexcept {
        return ranges.empty() ? 0u : ranges.back().to;
    }

    /** @brief True si @p pos cae dentro de algun rango vivo. */
    bool covers(uint32_t pos) const noexcept {
        /* ranges ordenados: busqueda lineal (pocos rangos por vreg). */
        for (const auto &r : ranges) {
            if (pos < r.from) return false; // ranges ascendentes
            if (pos < r.to) return true;
        }
        return false;
    }

    /**
     * @brief Primera posicion >= @p pos en la que dos intervalos estan
     *        vivos a la vez, o UINT32_MAX si nunca solapan en o despues
     *        de @p pos.  Lo usa el allocator para detectar interferencia.
     */
    uint32_t first_overlap_from(const LiveInterval &o,
                                uint32_t pos) const noexcept;

    /** @brief Primera posicion de uso > @p pos (o UINT32_MAX). */
    uint32_t next_use_after(uint32_t pos) const noexcept {
        for (uint32_t u : uses)
            if (u > pos) return u;
        return UINT32_MAX;
    }

    /**
     * @brief añade un rango [from, to).  Coalesce con rangos
     *        existentes solapados/adyacentes manteniendo orden.
     */
    void add_range(uint32_t from, uint32_t to);

    /**
     * @brief Acorta el inicio del PRIMER rango a @p pos (un def: el vreg
     *        no esta vivo antes de su definicion en este bloque).
     */
    void set_from(uint32_t pos) noexcept {
        if (!ranges.empty()) ranges.front().from = pos;
    }
};

/**
 * @struct IntervalResult
 * @brief Salida de @c build_intervals.
 */
struct IntervalResult {
    /// Indexado por vreg id (denso 0..vreg_count-1).  Intervals vacios
    /// = vregs sin uso.
    std::vector<LiveInterval> intervals;
    /// Posiciones de cada CALL (use_pos).  El allocator (commit 4) las
    /// usa para clobbear caller-saved e insertar save/restore.
    std::vector<uint32_t> call_positions;
    /// Posicion lineal maxima + 1 (tamano del espacio de posiciones).
    uint32_t max_pos = 0;
    ///  AS inc.5e: clobbers de registros fisicos por posicion de un
    /// INLINE_ASM_RAW.  El @c linear_scan excluye estos fisicos para los
    /// vregs NO-precoloreados cuyo intervalo cubre @c pos -- protege los
    /// clobbers de callee-saved (r12-r15) que el call-position (que solo
    /// fuerza a callee-saved a los caller-saved live-across) no cubre.
    struct AsmClobberSite {
        uint32_t pos;              ///< use_pos del INLINE_ASM_RAW
        std::vector<uint8_t> regs; ///< MReg ids clobbered (no bindings)
    };
    std::vector<AsmClobberSite> asm_clobbers;
    /// Vregs que DEBEN ser memory-resident (force-spill).  Indexado por vreg
    /// id (1 = forzar SPILL).  Lo poblea @c build_intervals con los valores
    /// live-in a un sucesor EXTRA/abnormal (handler de excepcion): deben vivir
    /// en un slot para sobrevivir al edge anormal (el throw clobberea regs
    /// pero no la memoria; el catch recarga del slot).  Mecanismo general
    /// reusable por GC/deopt (forzar memoria en un punto).  Vacio = sin
    /// fuerza (caso comun, cero coste).
    std::vector<uint8_t> force_spill;
    /// Coalescing hint para ops 2-address (`dst = src1 OP src2`): indexado por
    /// vreg id del dst -> vreg id de src1 (o -1 sin hint).  El @c linear_scan,
    /// al asignar dst, PREFIERE el fisico de src1 SI esta libre (libre <=>
    /// src1 ya expiro = murio -> coalescing seguro).  Asi dst y src1 comparten
    /// reg y el legalizado 2-address elide el `mov dst, src1`.  Solo es una
    /// PREFERENCIA: si el reg no esta libre, cae al greedy normal (correcto).
    std::vector<int32_t> coalesce_hint;
    /// Posicion lineal de inicio de cada bloque (@c 2*first_gi[b]), ascendente.
    /// Es un HECHO de la ESTRUCTURA del programa, no del allocator: delimita
    /// los tramos de codigo RECTILINEO (sin bifurcaciones ni confluencias).  Lo
    /// necesita cualquier transformacion que inserte codigo en un punto y deba
    /// garantizar que ese punto se ejecuta en el MISMO camino que otro (el
    /// splitting: cargar un valor a registro y devolverlo a memoria deben
    /// ocurrir siempre juntos).  El bloque de una posicion @c pos es el ultimo
    /// @c b con @c block_starts[b] <= pos.
    std::vector<uint32_t> block_starts;
};

/**
 * @enum OperandRole
 * @brief Rol de un operando respecto al dataflow.
 */
enum class OperandRole : uint8_t {
    NONE = 0,  ///< no es un registro relevante (label, imm, vacio)
    USE = 1,   ///< se lee
    DEF = 2,   ///< se escribe
    USEDEF = 3 ///< se lee y se escribe (p.ej. CMOVcc condicional)
};

/**
 * @struct InstrRoles
 * @brief Roles de @c {dst, src1, src2} para una MInstr.
 */
struct InstrRoles {
    OperandRole dst = OperandRole::NONE;
    OperandRole src1 = OperandRole::NONE;
    OperandRole src2 = OperandRole::NONE;
};

/**
 * @brief Clasifica los roles de los operandos de un opcode (forma de
 *        tres operandos pre-legalization).
 */
InstrRoles operand_roles(MOp op) noexcept;

/**
 * @brief Construye los live intervals de los vregs de @p mf.
 *
 * @param mf   Funcion en MachineIR (forma vreg).
 * @param tri  Descriptor del target (para futuras constraints; en commit
 *             2 se usa solo para validacion).
 * @return     Intervals por vreg + posiciones de CALL.
 */
IntervalResult build_intervals(const MFunction &mf, const TargetRegInfo &tri);

/**
 * @struct MachineNextUseFacts
 * @brief Hecho del nivel MACHINEIR: por cada vreg, las posiciones de sus USOS
 * en la numeracion de @c build_intervals (2 por instr, uso en @c 2*gi) -- de
 *        donde sale el NEXT-USE que consume el allocator (@c smart_spill) para
 *        elegir victima estilo Belady.
 *
 * Es el HOMOLOGO MachineIR de @c analysis::UseDefFacts (nivel IR): MISMA
 * interfaz
 * (@c next_use_after / @c distance_to_next_use) pero en el dominio @c
 * codegen::LinearPos. El allocator trabaja en ESTE espacio (su @c now es el @c
 * start de un intervalo, una posicion de @c build_intervals); consultar el
 * UseDefFacts IR con ese @c now mezclaria dominios -- el tipo fuerte @c
 * codegen::LinearPos lo impide en compilacion.
 *
 * FORMA CSR (contigua): @c use_pos son todas las posiciones concatenadas por
 * vreg;
 * @c off[v]..off[v+1] delimita las de @p v.  @c next_use_after es busqueda
 * binaria. Lo produce @c compute_next_use reusando el criterio de operandos de
 * @c build_intervals (roles USE/USEDEF, mismo INLINE_ASM_RAW).
 *
 * DOS BELADY, NO DUPLICACION.  El "next-use" existe en DOS niveles porque el
 * concepto pertenece a un DOMINIO (ver @c analysis/manager/analysis_manager.h):
 *   - Belady IR      = @c analysis::UseDefFacts   (IrValueId + @c
 * ir::LinearPos).
 *   - Belady Machine = este Fact                   (vreg + @c
 * codegen::LinearPos). Mismo concepto, dos dominios de posicion no
 * intercambiables (el tipo fuerte lo impide).  El allocator es MachineIR-level
 * -> consume ESTE, no el IR.
 *
 * FACTORIZACION PENDIENTE (cuando pague; no urgente).  Este Fact y
 * @c analysis::UseDefFacts comparten el MOLDE de ALMACENAR + CONSULTAR (CSR
 * @c off/@c use_pos + @c next_use_after con busqueda binaria + sentinela
 * @c invalid()).  SOLO eso se factorizaria a un @c NextUseTable<ValueId,
 * Position> del que ambos deriven (@c UseDefFacts = NextUseTable<IrValueId,
 * ir::LinearPos>,
 * @c MachineNextUseFacts = NextUseTable<vreg, codegen::LinearPos>).  Los
 * PRODUCTORES NO se factorizan: @c compute_use_def (recorre IR, PHI-args en @c
 * block_end) y
 * @c compute_next_use (recorre MachineIR, roles USE/USEDEF) son especificos del
 * nivel.  La semantica sigue en dos Facts distintos y dos dominios; solo se
 * comparte la estructura de datos.
 */
struct MachineNextUseFacts {
    std::vector<uint32_t> off; ///< off[v]..off[v+1] = rango de v en use_pos.
    std::vector<uint32_t>
        use_pos;          ///< posiciones de uso (2*gi), por vreg, ascendente.
    uint32_t max_pos = 0; ///< 2 * total_instrs (= IntervalResult.max_pos).

    /// Sentinela: el vreg no tiene ningun uso posterior (muerto tras @c pos).
    /// Es el ESTADO invalido de la posicion, no una posicion real.
    static constexpr codegen::LinearPos NO_NEXT_USE =
        codegen::LinearPos::invalid();

    /** @brief Numero de vregs cubiertos (off tiene un extra al final). */
    uint32_t num_vregs() const noexcept {
        return off.empty() ? 0u : static_cast<uint32_t>(off.size() - 1);
    }
    /** @brief ¿El vreg @p v se usa en algun sitio? */
    bool has_uses(uint32_t v) const noexcept {
        return v < num_vregs() && off[v + 1] > off[v];
    }
    /**
     * @brief Posicion MachineIR del PROXIMO uso de @p v estrictamente despues
     * de
     *        @p pos, o @c NO_NEXT_USE si no hay ninguno (vreg muerto -> victima
     *        ideal para Belady).  @c codegen::LinearPos garantiza el dominio
     * MachineIR.
     */
    codegen::LinearPos next_use_after(uint32_t v,
                                      codegen::LinearPos pos) const noexcept {
        if (v >= num_vregs()) return NO_NEXT_USE;
        const uint32_t *b = use_pos.data() + off[v];
        const uint32_t *e = use_pos.data() + off[v + 1];
        const uint32_t *it =
            std::upper_bound(b, e, pos.value); // primer uso > pos
        return it == e ? NO_NEXT_USE : codegen::LinearPos{*it};
    }
    /**
     * @brief Distancia al proximo uso desde @p pos (mayor = mejor victima
     * Belady).
     *        @c 0xFFFFFFFF si no hay proximo uso (distancia infinita).
     */
    uint32_t distance_to_next_use(uint32_t v,
                                  codegen::LinearPos pos) const noexcept {
        const codegen::LinearPos nu = next_use_after(v, pos);
        return nu.is_valid() ? (nu - pos)
                             : UINT32_MAX; // UINT32_MAX = distancia infinita.
    }
};

/**
 * @brief Produce @c MachineNextUseFacts de @p mf: por cada vreg, las posiciones
 * de sus usos en la numeracion de @c build_intervals (uso en @c 2*gi).  Reusa
 *        el criterio de operandos de @c build_intervals (roles USE/USEDEF +
 *        inline-asm).  Funcion PURA; no mira el IR (solo el MachineIR
 * asignado).
 */
MachineNextUseFacts compute_next_use(const MFunction &mf);

} // namespace jit

#endif // VESTA_JIT_INTERVAL_H
