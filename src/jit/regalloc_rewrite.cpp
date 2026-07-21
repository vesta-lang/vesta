/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file jit/regalloc_rewrite.cpp
 * @brief Implementacion del rewrite vreg -> fisico ( D.7, commit 4a).
 *        Ver regalloc_rewrite.h y doc/REGALLOC.md.
 */

#include "jit/regalloc_rewrite.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility> // std::pair (marshal stack-args; UCRT64 no transitivo)
#include <vector>

#include "vesta_rt/abi.h" // VESTA_PROC_OSR_BUFFER_OFFSET / VESTA_OSR_BUFFER_N
#include "vx/asm/asm_backend.h"  // ensamblar el asm DIFERIDO post-RA
#include "vx/asm/asm_phys_reg.h" // nombre del reg fisico ($N -> reg)
#include <cctype>

namespace jit {

namespace {

/** @brief Diagnostico/A-B: VESTA_JIT_NO_FRAMELESS=1 fuerza el frame
 *  completo (push rbp / mov rbp,rsp / pop rbp) incluso en hojas que
 *  calificarian para frameless.  Sirve para medir el efecto del
 *  frameless o aislar un posible bug del codegen del prologo/epilogo
 *  (el codegen mas safety-critical: un fallo aqui corrompe el heap via
 *  el precise stack-scan de la GC). */
bool jit_no_frameless() noexcept {
    static const bool off = [] {
        const char *v = std::getenv("VESTA_JIT_NO_FRAMELESS");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return off;
}

/** @brief OSR 1a (diagnostico, VESTA_OSR_COUNT=1): instrumenta cada
 *  back-edge con un contador de iteraciones.  Emitido POST-regalloc
 *  (aqui, no en vreg_select) para no pasar por la legalizacion 2-address
 *  de @c lower() -- que mangla @c ADD [mem],imm al ser ADD un bin_alu.
 *  El encoder procesa la secuencia verbatim (igual que el contador
 *  on-entry del Selector).  Prerrequisito del tier-up por back-edge (1b)
 *  y del OSR real (2). */
bool jit_osr_count() noexcept {
    static const bool on = [] {
        const char *v = std::getenv("VESTA_OSR_COUNT");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return on;
}
/* Contador unico global de iteraciones de back-edge (diagnostico 1a). */
uint64_t g_osr_be_total = 0;
uint32_t g_osr_be_sites = 0;

/** @brief Umbral de iteraciones para disparar el tier-up por back-edge
 *  (1b).  Default 100k; override VESTA_OSR_THRESHOLD. */
uint32_t jit_osr_threshold() noexcept {
    static const uint32_t t = [] {
        const char *v = std::getenv("VESTA_OSR_THRESHOLD");
        return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 10))
                 : 100000u;
    }();
    return t;
}
/* Cuantas veces se disparo el trigger (diagnostico). */
uint64_t g_osr_trigger_hits = 0;

/* ---- OSR paso 2a: descriptor del estado capturado por loop ---- */

/** @brief Una celda del state-transfer: el IR VID y si es raiz GC.
 *  El valor vive en @c osr_buffer[vid] tras la captura del trigger. */
struct OsrCaptureSlot {
    uint32_t vid;  ///< IR value id (== indice en osr_buffer)
    uint8_t is_gc; ///< 1 si es host_ptr/handle a objeto GC
};
/** @brief Descriptor de un loop OSR-instrumentado, indexado por loop_id.
 *  Construido en compile-time (rewrite_to_physical); lo consume el
 *  handler para loguear (2a) y, en 2b/2c, dirigir el salto C1->C2. */
struct OsrLoopDesc {
    std::string fn_name;                  ///< nombre de la funcion
    uint32_t header_block;                ///< MBlock id del loop header
    std::vector<OsrCaptureSlot> captures; ///< live-in del header
    bool aborted;                         ///< true si no se capturo estado
};
/* Indexado por loop_id (== el contador g_osr_be_sites del sitio). */
std::vector<OsrLoopDesc> g_osr_loops;

/** @brief Handler de OSR instalado por auto_jit (2c).  Dado un loop_id,
 *  devuelve la direccion del OSR-entry del C2 precompilado (o 0 si no
 *  hay variante).  nullptr por defecto -> el trigger solo loguea y el
 *  C1 NO hace swap (comportamiento 2a/2b). */
uint64_t (*g_osr_handler)(uint64_t) = nullptr;

/** @brief Diagnostico opt-in del trigger (VESTA_OSR_LOG=1): loguea el
 *  estado capturado en cada disparo.  Off por defecto para no spamear
 *  cuando el OSR real (swap) ya esta activo. */
bool jit_osr_log() noexcept {
    static const bool on = [] {
        const char *v = std::getenv("VESTA_OSR_LOG");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return on;
}

/** @brief Stub del trigger OSR.  Se invoca UNA vez por back-edge cuando
 *  el contador iguala el umbral.  Recibe el buffer con el estado del loop
 *  capturado por el C1 (puede loguearlo con VESTA_OSR_LOG=1).  Devuelve
 *  la direccion del OSR-entry del C2 (via @c g_osr_handler) para que el
 *  C1 haga el frame-swap; 0 = no swap (el C1 continua su loop).
 *  Convencion C: (proc, loop_id, buffer) -> uint64_t en RAX. */
uint64_t osr_trigger_stub(void * /*proc*/, uint64_t loop_id, uint64_t *buffer) {
    ++g_osr_trigger_hits;
    if (jit_osr_log()) {
        if (loop_id < g_osr_loops.size()) {
            const OsrLoopDesc &d = g_osr_loops[static_cast<size_t>(loop_id)];
            std::fprintf(stderr,
                         "[osr] TRIGGER loop_id=%llu fn=%s header_bb=%u "
                         "capturas=%zu%s\n",
                         static_cast<unsigned long long>(loop_id),
                         d.fn_name.c_str(), d.header_block, d.captures.size(),
                         d.aborted ? " (ABORTADO: estado no capturable)" : "");
            if (buffer && !d.aborted) {
                for (const OsrCaptureSlot &c : d.captures) {
                    const uint64_t val = buffer[c.vid];
                    std::fprintf(stderr, "[osr]   v%u%s = %lld (0x%llx)\n",
                                 c.vid, c.is_gc ? " gc" : "",
                                 static_cast<long long>(val),
                                 static_cast<unsigned long long>(val));
                }
            }
        } else {
            std::fprintf(stderr, "[osr] TRIGGER loop_id=%llu (umbral %u)\n",
                         static_cast<unsigned long long>(loop_id),
                         jit_osr_threshold());
        }
    }
    /* 2c: delegar en el handler para obtener el OSR-entry del C2. */
    return g_osr_handler ? g_osr_handler(loop_id) : 0;
}
/** @brief Registra (una vez) el dump atexit del contador de back-edges. */
void osr_install_dump_once() {
    static bool installed = false;
    if (installed) return;
    installed = true;
    std::atexit([] {
        std::fprintf(
            stderr,
            "[osr] back-edges=%u  iteraciones_totales=%llu  triggers=%llu\n",
            g_osr_be_sites, static_cast<unsigned long long>(g_osr_be_total),
            static_cast<unsigned long long>(g_osr_trigger_hits));
    });
}

/** @brief True si la ALU binaria @p op es conmutativa. */
bool is_commutative(MOp op) noexcept {
    switch (op) {
    case MOp::ADD:
    case MOp::AND:
    case MOp::OR:
    case MOp::XOR:
    case MOp::IMUL: return true;
    default: return false;
    }
}

/** @brief True si @p op es una ALU binaria que reescribimos (4a). */
bool is_bin_alu(MOp op) noexcept {
    switch (op) {
    case MOp::ADD:
    case MOp::SUB:
    case MOp::AND:
    case MOp::OR:
    case MOp::XOR:
    case MOp::IMUL: return true;
    default: return false;
    }
}

/**
 * @struct Lowerer
 * @brief Estado del rewrite de una funcion.
 */
struct Lowerer {
    const RegAlloc &ra;
    const TargetRegInfo &tri;
    bool vm_abi = false;      ///< VM_ABI (salva RBX=ProcessVM*) vs host leaf
    bool no_frame = false;    ///< hoja frameless: sin push/mov rbp ni sub rsp
    bool naked = false;       ///<  NR @Naked: sin prologo/epilogo/ret
    uint32_t k = 0;           ///< numero de callee-saved asignados
    uint32_t total_saved = 0; ///< callee-saved + (vm_abi ? 1 (rbx) : 0)
    int32_t spill_bytes = 0;  ///< tamano del area de spills (alineado)
    /// Args ilimitados (JIT/AOT): nº de slots GP que se pasan por PILA
    /// (overflow de los arg_regs).  Se reservan en el FONDO del frame
    /// (encima del shadow Win64) y se direccionan RSP-relativo en el call.
    /// El interprete tiene limite 12 (bytecode), el JIT/AOT no.
    uint32_t out_stack_args = 0;
    /// Offset RSP del primer stack-arg: 32 (Win64 shadow) o 0 (SysV).
    int32_t stack_arg_base = 0;
    /// Commit 8: offset (desde RBP) del inicio del area de allocas
    /// (justo debajo de los spill slots) + cursor de asignacion.
    uint32_t alloca_base = 0;
    uint32_t alloca_cursor = 0;
    /// Fase 2 (ALLOCA_VM): la fn reserva en el VM stack del proceso.
    /// Hay que salvar el VM-RSP (proc->stack_pointer) al entry y
    /// restaurarlo en CADA RET, o el VM stack hace leak/overflow entre
    /// llamadas.  @c vm_rsp_save_off es el offset (negativo desde RBP)
    /// del slot que guarda el VM-RSP original (un qword debajo del area
    /// de allocas host, por encima del shadow space Win64).
    bool has_vm_alloca = false;
    int32_t vm_rsp_save_off = 0;
    /// Callback-ABI save-set (jubilacion de slots): si @c cb_save_regs, el
    /// frame reserva 128B (16 qwords) donde @c CB_SAVE_REGS salva
    /// proc->registers[0..15] del caller VM.  @c cb_save_base_off es el offset
    /// (negativo desde RBP) del slot del reg 0; el reg r vive en
    /// @c cb_save_base_off - r*8.
    bool cb_save_regs = false;
    int32_t cb_save_base_off = 0;
    MReg scr0 = MReg::R10;
    MReg scr1 = MReg::R11;
    /// FP-regalloc ( AOT C1 float): scratch XMM del rewrite
    /// (materializar spills FP + two-address legalization de ADDSD/etc),
    /// analogo a scr0/scr1 GP.  Por defecto XMM14/XMM15 (ver
    /// @c target_x86_64_target); se sobreescriben desde @c tri.scratch[FP].
    MReg fscr0 = MReg::XMM14;
    MReg fscr1 = MReg::XMM15;
    /// Tamano de un slot de pila / push (= pointer_size del target):
    /// 8 en x86-64, 4 en x86-32.  El frame (callee-saved, spill slots,
    /// epilogue lea) se mide en estos slots; usar 8 fijo en x86-32
    /// desalineaba el `lea esp,[ebp-N]` -> el ret leia basura.
    uint32_t SZ = 8;
    /// MFunction destino: necesario para crear labels intra-expansion
    /// (LOAD_VM/STORE_VM page-cache) via @c pf->new_label().  Se asigna
    /// en @c rewrite_to_physical tras construir pf.
    MFunction *pf = nullptr;

    Lowerer(const RegAlloc &r, const TargetRegInfo &t, AbiKind abi,
            bool has_calls, uint32_t alloca_total, bool has_vm_alloca_in,
            uint32_t out_stack_args_in = 0, bool has_stack_params_in = false,
            bool cb_save_regs_in = false)
        : ra(r), tri(t), vm_abi(abi == AbiKind::VM),
          has_vm_alloca(has_vm_alloca_in), out_stack_args(out_stack_args_in),
          cb_save_regs(cb_save_regs_in) {
        SZ = t.pointer_size ? t.pointer_size : 8u;
        k = static_cast<uint32_t>(ra.callee_saved_used.size());
        total_saved = k + (vm_abi ? 1u : 0u); // +1 por el push rbx
        /* Hoja frameless: una funcion sin CALLs que no spillea ni
         * reserva allocas no necesita frame pointer.  RBP solo
         * direcciona spill slots y el area de allocas; sin ellos se
         * omite todo el frame (push rbp / mov rbp,rsp / sub rsp / lea /
         * pop rbp).  Es SEGURO respecto a la GC: el precise stack-scan
         * camina la cadena RBP, pero solo en un SAFEPOINT, y una hoja
         * no tiene safepoint (no llama a nada) -> ningun frame de hoja
         * esta vivo cuando corre la GC.  Aun en VM_ABI se conserva el
         * push/pop de RBX (callee-saved del host que trae ProcessVM*) y
         * los callee-saved usados; lo unico que desaparece es RBP. */
        no_frame = !has_calls && ra.num_spill_slots == 0u &&
                   alloca_total == 0u && !has_vm_alloca &&
                   !has_stack_params_in && /* params en pila -> [rbp+off]
                  necesita el frame pointer estable (push rbp; mov rbp,rsp). */
                   !cb_save_regs_in && /* save-set del callback: la work-area de
                  128B vive en el frame ([rbp-...]) -> necesita frame estable +
                  su reserva en spill_bytes (sub rsp). */
                   !jit_no_frameless() &&
                   !jit_osr_count(); /* el trigger (1b) añade un
                  call -> necesita frame con rsp 16-alineado. */
        /* Las allocas viven debajo de los spill slots. */
        alloca_base = SZ * total_saved + SZ * ra.num_spill_slots;
        spill_bytes =
            static_cast<int32_t>(SZ * ra.num_spill_slots + alloca_total);
        /*   reservar un qword para el VM-RSP salvado, debajo del
         * area de allocas host y por encima del shadow space.  El
         * offset es fijo desde RBP (independiente del shadow/align que
         * se añade despues, que solo crece el frame hacia abajo). */
        if (has_vm_alloca) {
            vm_rsp_save_off = -static_cast<int32_t>(
                8u * total_saved + 8u * ra.num_spill_slots + alloca_total + 8u);
            spill_bytes += 8;
        }
        /* Callback save-set: reservar 128B (16 qwords) debajo de todo lo
         * anterior (spills + allocas + vm_rsp) para salvar regs[0..15] del
         * caller VM.  cb_save_base_off = offset del slot del reg 0 (los demas
         * hacia abajo, -r*8).  no_frame es false (cuerpo no-hoja -> has_calls). */
        if (cb_save_regs) {
            cb_save_base_off =
                -(static_cast<int32_t>(8u * total_saved) + spill_bytes + 8);
            spill_bytes += 128;
        }
        if (!no_frame) {
            /* stack_arg_base = offset RSP del primer arg por pila.  Win64
             * reserva 32 de shadow ANTES de los stack args (el callee lee
             * arg5 en [rsp+32]); SysV no tiene shadow (arg7 en [rsp+0]).
             * Detectado por el nº de GP arg_regs del TARGET (4=Win64,
             * 6=SysV) -> correcto tambien en AOT cross-target. */
            const size_t gareg_n =
                tri.arg_regs[static_cast<size_t>(RegClass::GP)].size();
            stack_arg_base = (gareg_n == 4) ? 32 : 0;
#if defined(_WIN32)
            /* Win64: si hay CALLs, reservar 32 bytes de shadow/home
             * space en el FONDO del frame (debajo de los spill slots)
             * para que el callee no pise nuestros datos. */
            if (has_calls) spill_bytes += 32;
#else
            (void)has_calls;
#endif
            /* Args ilimitados: reservar el outgoing area (slots GP por
             * pila) encima del shadow.  Stack-arg j vive en
             * [rsp + stack_arg_base + j*8] en cada call. */
            if (out_stack_args > 0)
                spill_bytes += static_cast<int32_t>(out_stack_args * 8u);
            /* Alinear (SZ*total_saved + spill_bytes) a 16 para mantener
             * el stack 16-aligned en CALLs internos.  Con slots de 4
             * (x86-32) el desalineo puede ser 4/8/12 -> padding exacto. */
            {
                const uint32_t cur =
                    SZ * total_saved + static_cast<uint32_t>(spill_bytes);
                spill_bytes += static_cast<int32_t>((16u - (cur % 16u)) % 16u);
            }
        }
        /* Frameless: spill_bytes queda en 0 (no hay spills ni allocas) y
         * no se alinea a 16 ni se reserva shadow space porque no hay
         * CALLs internos. */
        const auto &sc = tri.scratch[static_cast<size_t>(RegClass::GP)];
        if (sc.size() >= 1) scr0 = static_cast<MReg>(sc[0]);
        if (sc.size() >= 2) scr1 = static_cast<MReg>(sc[1]);
        const auto &fsc = tri.scratch[static_cast<size_t>(RegClass::FP)];
        if (fsc.size() >= 1) fscr0 = static_cast<MReg>(fsc[0]);
        if (fsc.size() >= 2) fscr1 = static_cast<MReg>(fsc[1]);
    }

    /// Argumento pendiente de la proxima CALL: indice DENTRO DE SU CLASE,
    /// ubicacion fisica resuelta, y si es de clase FP (-> XMM arg_reg, MOVSD).
    struct PendingArg {
        uint8_t idx;
        MOperand loc;
        bool is_fp;
    };
    std::vector<PendingArg> pending_args;

    ///  D.7 commit 6: intervalos (para stackmaps en CALLs) +
    /// posicion lineal del CALL actual + stackmaps acumulados.
    const IntervalResult *ivs = nullptr;
    uint32_t cur_call_pos = 0;
    std::vector<Stackmap> stackmaps;

    /**
     * @brief Emite un PARALLEL MOVE: el conjunto de copias
     *        @c dst_reg <- src se realiza simultaneamente.  Emite en
     *        un orden seguro y rompe ciclos con @p scratch.
     */
    void emit_parallel_moves(std::vector<std::pair<MReg, MOperand>> moves,
                             MReg scratch, std::vector<MInstr> &out) const {
        const size_t n = moves.size();
        std::vector<bool> done(n, false);
        size_t remaining = n;
        auto reads_dst = [&](size_t i) -> bool {
            for (size_t j = 0; j < n; ++j) {
                if (done[j] || j == i) continue;
                if (moves[j].second.kind == MOperandKind::REG &&
                    moves[j].second.reg == reg_id(moves[i].first))
                    return true;
            }
            return false;
        };
        while (remaining > 0) {
            bool progress = false;
            for (size_t i = 0; i < n; ++i) {
                if (done[i] || reads_dst(i)) continue;
                out.push_back(MInstr::make_unary(MOp::MOV, reg(moves[i].first),
                                                 moves[i].second));
                done[i] = true;
                --remaining;
                progress = true;
            }
            if (progress) continue;
            /* Ciclo: salvar un dst a scratch y romperlo. */
            for (size_t i = 0; i < n; ++i) {
                if (done[i]) continue;
                const MReg d = moves[i].first;
                out.push_back(
                    MInstr::make_unary(MOp::MOV, reg(scratch), reg(d)));
                for (size_t j = 0; j < n; ++j)
                    if (!done[j] && moves[j].second.kind == MOperandKind::REG &&
                        moves[j].second.reg == reg_id(d))
                        moves[j].second = reg(scratch);
                out.push_back(
                    MInstr::make_unary(MOp::MOV, reg(d), moves[i].second));
                done[i] = true;
                --remaining;
                break;
            }
        }
    }

    /** @brief Registro host que trae el @c ProcessVM* (primer arg). */
    static MReg proc_arg_reg() noexcept {
#if defined(_WIN32)
        return MReg::RCX;
#else
        return MReg::RDI;
#endif
    }

    /** @brief Offset desde RBP del spill slot @p slot. */
    int32_t slot_off(uint32_t slot) const noexcept {
        return -static_cast<int32_t>(SZ * total_saved + SZ * (slot + 1u));
    }

    /**
     * @brief Tamano del frame en un safepoint call: RBP - RSP.
     *
     * El prologo hace @c push rbp; mov rbp,rsp; (push callee-saved)*total_saved;
     * sub rsp,spill_bytes -> en cualquier CALL interno RSP = RBP - (SZ*total_saved
     * + spill_bytes).  Lo consume el WALK POR TAMANO DE FRAME del GC de AOT para
     * reconstruir RBP desde el RSP del llamador sin leer la cadena RBP.  Los
     * slots GC (slot_off) son RBP-relativos, asi que rbp = sp_llamador + este
     * valor + 16 en la iteracion; ver @c scan_aot_frames.
     */
    uint32_t frame_size_for_scan() const noexcept {
        return SZ * total_saved +
               (spill_bytes > 0 ? static_cast<uint32_t>(spill_bytes) : 0u);
    }

    /** @brief Operando de memoria del spill slot @p slot: [rbp+off]. */
    MOperand slot_mem(uint32_t slot) const noexcept {
        return MOperand::make_mem(MReg::RBP, slot_off(slot));
    }

    /** @brief Resuelve un operando vreg a su ubicacion fisica. */
    MOperand resolve(const MOperand &o) const noexcept {
        if (!o.is_vreg()) return o; // imm/label/mem/reg fisico: passthrough
        const uint32_t vid = o.vreg_id();
        if (ra.in_reg(vid))
            return MOperand::make_reg(static_cast<MReg>(ra.reg_of(vid)),
                                      o.width);
        return slot_mem(ra.slot_of(vid)); // spilled
    }

    /** @brief True si @p o es un valor de coma flotante (vreg de clase FP o
     *  un registro fisico XMM).  Lo usa @c lower() para enrutar los MOV de
     *  un valor float al MOVSD/MOVSS (no al mov entero). */
    static bool is_fp_operand(const MOperand &o) noexcept {
        if (o.is_vreg()) return o.vreg_class() == RegClass::FP;
        if (o.kind == MOperandKind::REG)
            return is_xmm(static_cast<MReg>(o.reg));
        return false;
    }
    /** @brief MOp de movimiento FP segun el ancho (8 -> MOVSD, 4 -> MOVSS). */
    static MOp fp_mov_for_width(uint8_t w) noexcept {
        return (w == 4) ? MOp::MOVSS : MOp::MOVSD;
    }
    static MOperand xmm(MReg r) { return MOperand::make_reg(r, 8); }

    static MOperand reg(MReg r) { return MOperand::make_reg(r, 8); }
    static MInstr push(MReg r) {
        MInstr i;
        i.op = MOp::PUSH;
        i.src1 = reg(r);
        return i;
    }
    static MInstr pop(MReg r) {
        MInstr i;
        i.op = MOp::POP;
        i.dst = reg(r);
        return i;
    }

    void emit_prologue(std::vector<MInstr> &out) const {
        /*  NR @Naked: cero prologo.  El cuerpo (asm) controla todo. */
        if (naked) return;
        if (!no_frame) {
            out.push_back(push(MReg::RBP));
            out.push_back(
                MInstr::make_unary(MOp::MOV, reg(MReg::RBP), reg(MReg::RSP)));
        }
        if (vm_abi) {
            /* Salvar RBX (callee-saved del host) y cargarlo con el
             * ProcessVM* que llega en el primer arg. */
            out.push_back(push(MReg::RBX));
            out.push_back(MInstr::make_unary(MOp::MOV, reg(MReg::RBX),
                                             reg(proc_arg_reg())));
        }
        for (uint8_t r : ra.callee_saved_used)
            out.push_back(push(static_cast<MReg>(r)));
        if (spill_bytes > 0)
            out.push_back(MInstr::make_unary(
                MOp::SUB, reg(MReg::RSP), MOperand::make_imm32(spill_bytes)));
        /*   salvar el VM-RSP original al slot del frame.  Los
         * ALLOCA_VM mas adelante decrementan proc->stack_pointer; el
         * epilogue lo restaura desde aqui (si no, el VM stack hace
         * leak/overflow entre llamadas).  scr0 (R10) es caller-saved y
         * esta libre aqui; RBX ya trae el ProcessVM* (vm_abi). */
        if (has_vm_alloca) {
            out.push_back(MInstr::make_unary(
                MOp::MOV, reg(scr0),
                MOperand::make_mem(MReg::RBX,
                                   VESTA_PROC_STACK_POINTER_OFFSET)));
            out.push_back(MInstr::make_unary(
                MOp::MOV, MOperand::make_mem(MReg::RBP, vm_rsp_save_off),
                reg(scr0)));
        }
    }

    void emit_epilogue(std::vector<MInstr> &out) const {
        /*  NR @Naked: cero epilogo (el cuerpo provee ret/iretq). */
        if (naked) return;
        if (no_frame) {
            /* Frameless: rsp ya apunta justo encima de los registros
             * salvados (no hubo push rbp ni sub rsp).  Solo se deshacen
             * los push de callee-saved y RBX en orden inverso; el ret
             * encuentra la return address exactamente. */
            for (size_t i = ra.callee_saved_used.size(); i-- > 0;)
                out.push_back(pop(static_cast<MReg>(ra.callee_saved_used[i])));
            if (vm_abi) out.push_back(pop(MReg::RBX));
            return;
        }
        /*   restaurar el VM-RSP original ANTES de desmontar el
         * frame (RBP/RBX aun validos).  Sin esto los ALLOCA_VM dejarian
         * proc->stack_pointer decrementado tras el RET -> leak/overflow
         * del VM stack global.  scr0 (R10) es caller-saved (libre). */
        if (has_vm_alloca) {
            out.push_back(MInstr::make_unary(
                MOp::MOV, reg(scr0),
                MOperand::make_mem(MReg::RBP, vm_rsp_save_off)));
            out.push_back(MInstr::make_unary(
                MOp::MOV,
                MOperand::make_mem(MReg::RBX, VESTA_PROC_STACK_POINTER_OFFSET),
                reg(scr0)));
        }
        /* lea rsp, [rbp - SZ*total_saved] -> deshace el sub del frame y
         * apunta rsp al ultimo registro salvado (SZ = 4 en x86-32). */
        out.push_back(MInstr::make_unary(
            MOp::LEA, reg(MReg::RSP),
            MOperand::make_mem(MReg::RBP,
                               -static_cast<int32_t>(SZ * total_saved))));
        /* pop callee en orden inverso. */
        for (size_t i = ra.callee_saved_used.size(); i-- > 0;)
            out.push_back(pop(static_cast<MReg>(ra.callee_saved_used[i])));
        if (vm_abi) out.push_back(pop(MReg::RBX));
        out.push_back(pop(MReg::RBP));
    }

    /**
     * @brief HOST_LEAF: copia los params entrantes (en los arg_regs del
     *        ABI host) a su ubicacion fisica asignada por el regalloc.
     *
     * El selector emite, al inicio del bloque 0, un @c MOV @c vreg <-
     * @c arg_reg fisico por param (que ancla su intervalo).  Aqui los
     * consumimos: los params SPILLED se escriben a su slot directamente
     * (lee el arg_reg pristino, escribe memoria -- no pisa ningun
     * arg_reg), y los params en REGISTRO se mueven con un PARALLEL-MOVE
     * (que rompe ciclos arg_reg<->home con @c scr1).  Hacer los spills
     * ANTES garantiza que leen el arg_reg antes de que un move de
     * registro lo sobrescriba.  El prologo nunca toca los arg_regs (son
     * caller-saved en ambos ABIs), por lo que estan intactos aqui.
     *
     * @param instrs Instrucciones del bloque 0 (en forma vreg).
     * @param params @c MFunction::param_vregs.
     * @param out    Destino de las instrucciones fisicas.
     * @return Numero de instrucciones lideres consumidas (el caller las
     *         salta en el lowering normal).
     */
    size_t emit_host_param_loads(const std::vector<MInstr> &instrs,
                                 const std::vector<uint32_t> &params,
                                 std::vector<MInstr> &out) const {
        if (vm_abi || params.empty()) return 0;
        std::vector<std::pair<MReg, MOperand>> reg_moves;    // GP
        std::vector<std::pair<MReg, MOperand>> freg_moves;   // FP (XMM)
        size_t n = 0;
        /* Los MOV param-init son exactamente los lideres del bloque 0 con
         * dst vreg y src registro fisico (arg_reg).  Para los params FLOAT,
         * el src es un XMM arg-reg (el selector contó el indice float aparte
         * del entero) -> se enrutan a un parallel-move FP via MOVSD. */
        while (n < instrs.size() && n < params.size() &&
               instrs[n].op == MOp::MOV && instrs[n].dst.is_vreg() &&
               instrs[n].src1.is_reg()) {
            const bool fp = is_fp_operand(instrs[n].dst) ||
                            is_fp_operand(instrs[n].src1);
            const MOperand dst = resolve(instrs[n].dst);
            if (dst.is_reg()) {
                if (fp)
                    freg_moves.emplace_back(static_cast<MReg>(dst.reg),
                                            instrs[n].src1);
                else
                    reg_moves.emplace_back(static_cast<MReg>(dst.reg),
                                           instrs[n].src1);
            } else {
                /* param spilled: escribir el arg_reg al slot ya mismo (lee
                 * el arg_reg pristino antes de cualquier move de registro). */
                out.push_back(MInstr::make_unary(
                    fp ? MOp::MOVSD : MOp::MOV, dst, instrs[n].src1));
            }
            ++n;
        }
        if (!reg_moves.empty())
            emit_parallel_moves(std::move(reg_moves), scr1, out);
        if (!freg_moves.empty())
            emit_parallel_moves_fp(std::move(freg_moves), fscr1, out);
        return n;
    }

    /** @brief PARALLEL MOVE para registros XMM (param-load FP): identico a
     *  @c emit_parallel_moves pero emitiendo MOVSD y rompiendo ciclos con un
     *  scratch XMM.  Las fuentes son siempre XMM arg-regs (no memoria). */
    void emit_parallel_moves_fp(std::vector<std::pair<MReg, MOperand>> moves,
                                MReg scratch, std::vector<MInstr> &out) const {
        const size_t n = moves.size();
        std::vector<bool> done(n, false);
        size_t remaining = n;
        auto reads_dst = [&](size_t i) -> bool {
            for (size_t j = 0; j < n; ++j) {
                if (done[j] || j == i) continue;
                if (moves[j].second.kind == MOperandKind::REG &&
                    moves[j].second.reg == reg_id(moves[i].first))
                    return true;
            }
            return false;
        };
        while (remaining > 0) {
            bool progress = false;
            for (size_t i = 0; i < n; ++i) {
                if (done[i] || reads_dst(i)) continue;
                out.push_back(MInstr::make_unary(MOp::MOVSD, xmm(moves[i].first),
                                                 moves[i].second));
                done[i] = true;
                --remaining;
                progress = true;
            }
            if (progress) continue;
            for (size_t i = 0; i < n; ++i) {
                if (done[i]) continue;
                const MReg d = moves[i].first;
                out.push_back(
                    MInstr::make_unary(MOp::MOVSD, xmm(scratch), xmm(d)));
                for (size_t j = 0; j < n; ++j)
                    if (!done[j] && moves[j].second.kind == MOperandKind::REG &&
                        moves[j].second.reg == reg_id(d))
                        moves[j].second = xmm(scratch);
                out.push_back(MInstr::make_unary(MOp::MOVSD, xmm(d),
                                                 moves[i].second));
                done[i] = true;
                --remaining;
                break;
            }
        }
    }

    /** @brief Resuelve un batch de copias de PHI de una MISMA arista como un
     *  PARALLEL MOVE.  Todas las copias phi_dst_i <- arg_i se leen "a la vez"
     *  en la arista; emitidas SECUENCIALMENTE corrompen si el regalloc asigno
     *  phi_dst_i y arg_j al MISMO fisico (ciclo de permutacion) -- ocurre tras
     *  ssa_coalesce, que aprieta la asignacion.  GP via emit_parallel_moves
     *  (scr1 rompe ciclos con un scratch reservado), FP via _fp (fscr1).  Los
     *  dst spilled se escriben primero (leen su src pristino antes de que un
     *  reg-move lo pise). */
    void lower_phi_parallel(const std::vector<const MInstr *> &phis,
                            std::vector<MInstr> &out) {
        std::vector<std::pair<MReg, MOperand>> reg_moves, freg_moves;
        for (const MInstr *p : phis) {
            const bool fp = is_fp_operand(p->dst) || is_fp_operand(p->src1);
            const MOperand dst = resolve(p->dst);
            const MOperand src = resolve(p->src1);
            /* Self-copy (dst==src, mismo fisico): NO-OP en el parallel move.
             * Incluirlo mete un self-loop en el grafo de dependencias que
             * confunde la deteccion de ciclos de emit_parallel_moves.  Aparece
             * con la coalescencia de phi auto-referenciales (`v = phi[.., v]`
             * -> copia `mov v, v`).  Se salta. */
            if (dst.is_reg() && src.kind == MOperandKind::REG &&
                dst.reg == src.reg)
                continue;
            if (dst.is_reg()) {
                if (fp)
                    freg_moves.emplace_back(static_cast<MReg>(dst.reg), src);
                else
                    reg_moves.emplace_back(static_cast<MReg>(dst.reg), src);
            } else {
                /* dst spilled: store ahora (src pristino).  mem->mem via
                 * scratch reservado. */
                const MReg sc = fp ? fscr1 : scr1;
                const MOp mop = fp ? MOp::MOVSD : MOp::MOV;
                if (src.kind == MOperandKind::MEM) {
                    out.push_back(MInstr::make_unary(
                        mop, fp ? xmm(sc) : reg(sc), src));
                    out.push_back(MInstr::make_unary(
                        mop, dst, fp ? xmm(sc) : reg(sc)));
                } else {
                    out.push_back(MInstr::make_unary(mop, dst, src));
                }
            }
        }
        if (!reg_moves.empty())
            emit_parallel_moves(std::move(reg_moves), scr1, out);
        if (!freg_moves.empty())
            emit_parallel_moves_fp(std::move(freg_moves), fscr1, out);
    }

    /** @brief Marshala @c pending_args a los arg_regs del ABI host: los
     *  enteros (GP) con un parallel-move via MOV (scr1 rompe ciclos), los
     *  floats (FP) con un parallel-move via MOVSD a los XMM arg_regs (fscr1).
     *  Cada arg lleva su indice DENTRO DE SU CLASE.  Limpia @c pending_args. */
    void marshal_args(std::vector<MInstr> &out) {
        const auto &gareg = tri.arg_regs[static_cast<size_t>(RegClass::GP)];
        const auto &fareg = tri.arg_regs[static_cast<size_t>(RegClass::FP)];
        std::vector<std::pair<MReg, MOperand>> gmoves, fmoves;
        /* Args ilimitados: GP que no caben en arg_regs van por PILA en
         * [rsp + stack_arg_base + (idx-gareg)*8].  (idx, loc). */
        /* (offset_rsp, loc, is_fp): is_fp -> store por MOVSD. */
        std::vector<std::tuple<int32_t, MOperand, bool>> smoves;
        /* G = nº de GP-stack-args (overflow GP).  Los FP-stack van DESPUES en
         * la convencion Vesta-interna -> [base+(G+(fi-fmax))*8].  Espejo de la
         * carga del callee en vreg_select. */
        uint32_t G = 0;
        for (const auto &pa : pending_args)
            if (!pa.is_fp && pa.idx >= gareg.size()) ++G;
        for (const auto &pa : pending_args) {
            if (pa.is_fp) {
                if (pa.idx < fareg.size())
                    fmoves.emplace_back(static_cast<MReg>(fareg[pa.idx]),
                                        pa.loc);
                else {
                    const int32_t off =
                        stack_arg_base +
                        static_cast<int32_t>(
                            (G + (pa.idx - fareg.size())) * 8u);
                    smoves.emplace_back(off, pa.loc, true);
                }
            } else {
                if (pa.idx < gareg.size())
                    gmoves.emplace_back(static_cast<MReg>(gareg[pa.idx]),
                                        pa.loc);
                else {
                    const int32_t off =
                        stack_arg_base +
                        static_cast<int32_t>((pa.idx - gareg.size()) * 8u);
                    smoves.emplace_back(off, pa.loc, false);
                }
            }
        }
        /* Stores de stack-args PRIMERO (leen su loc antes del shuffle de los
         * arg_regs).  mem->mem via scr1 (R11, caller-saved, libre aqui) -- NO
         * scr0: un CALL INDIRECTO captura su func_ptr en scr0 antes de este
         * marshal, y debe sobrevivir (ver el caso MOp::CALL).  scr1 se reusa
         * luego para romper ciclos del parallel-move (secuencial, sin solape).
         * FP via MOVSD (scratch XMM si mem->mem). */
        for (const auto &sm : smoves) {
            const int32_t soff = std::get<0>(sm);
            const MOperand &sloc = std::get<1>(sm);
            const bool sfp = std::get<2>(sm);
            const MOperand dst = MOperand::make_mem(MReg::RSP, soff);
            if (sfp) {
                if (sloc.kind == MOperandKind::MEM) {
                    out.push_back(
                        MInstr::make_unary(MOp::MOVSD, xmm(fscr1), sloc));
                    out.push_back(
                        MInstr::make_unary(MOp::MOVSD, dst, xmm(fscr1)));
                } else {
                    out.push_back(MInstr::make_unary(MOp::MOVSD, dst, sloc));
                }
            } else if (sloc.kind == MOperandKind::MEM) {
                out.push_back(MInstr::make_unary(MOp::MOV, reg(scr1), sloc));
                out.push_back(MInstr::make_unary(MOp::MOV, dst, reg(scr1)));
            } else {
                out.push_back(MInstr::make_unary(MOp::MOV, dst, sloc));
            }
        }
        if (!gmoves.empty()) emit_parallel_moves(std::move(gmoves), scr1, out);
        if (!fmoves.empty())
            emit_parallel_moves_fp(std::move(fmoves), fscr1, out);
        pending_args.clear();
    }

    /** @brief Reescribe una instr vreg a 0+ instrs fisicas. */
    void lower(const MInstr &in, std::vector<MInstr> &out) {
        const MOp op = in.op;

        if (op == MOp::ARG) {
            /* Acumular: (indice-de-clase, ubicacion fisica, es_fp).  El
             * selector ya numera el indice por clase; la clase se toma del
             * operando vreg (in.src1.vreg_class) o, si ya es un reg fisico,
             * de si es XMM. */
            pending_args.push_back(
                {in.variant, resolve(in.src1), is_fp_operand(in.src1)});
            return;
        }

        /* ===== Callback save-set (jubilacion de slots) ===== */
        if (op == MOp::CB_SAVE_REGS || op == MOp::CB_RESTORE_REGS) {
            /* Expandir con R11 (scratch): copiar los 16 qwords entre
             * proc->registers[r] ([rbx + REGISTERS_OFFSET + r*8]) y la
             * work-area del frame ([rbp + cb_save_base_off - r*8]).  RBX =
             * ProcessVM* (VM_ABI), cargado por LOAD_PROC antes del SAVE. */
            const bool save = (op == MOp::CB_SAVE_REGS);
            for (uint32_t r = 0; r < 16u; ++r) {
                const MOperand vmreg = MOperand::make_mem(
                    MReg::RBX, VESTA_PROC_REGISTERS_OFFSET +
                                   static_cast<int32_t>(r) * VESTA_REGISTER_SIZE);
                const MOperand frame = MOperand::make_mem(
                    MReg::RBP, cb_save_base_off - static_cast<int32_t>(r) * 8);
                if (save) {
                    out.push_back(MInstr::make_unary(MOp::MOV, reg(scr1), vmreg));
                    out.push_back(MInstr::make_unary(MOp::MOV, frame, reg(scr1)));
                } else {
                    out.push_back(MInstr::make_unary(MOp::MOV, reg(scr1), frame));
                    out.push_back(MInstr::make_unary(MOp::MOV, vmreg, reg(scr1)));
                }
            }
            return;
        }

        /* ===== FP-regalloc ( AOT C1 float) ===== */

        /* FP MOV (dst/src de clase float): movimiento de un escalar f64/f32.
         * El selector emite @c MOp::MOV para copias FP, el param-init
         * (@c MOV vr_fp, XMM_arg) y el RET (@c MOV XMM0, vr_fp); aqui se
         * enruta al MOVSD/MOVSS.  Ambos pueden estar en XMM o en un slot de
         * pila (spilled).  x86 no tiene mov mem,mem -> si AMBOS son MEM se
         * pasa por el scratch XMM. */
        MOperand d_res = (op == MOp::MOV) ? resolve(in.dst) : MOperand::none();
        MOperand s_res = (op == MOp::MOV) ? resolve(in.src1) : MOperand::none();
        // La clase se decide por (a) la clase declarada del vreg operando, o
        // (b) el REGISTRO FiSICO ASIGNADO por el linear-scan.  Ambos deben
        // coincidir, pero si por cualquier razon divergen (un vreg cuya clase
        // declarada quedo GP pero recibio un XMM), el fisico manda: un MOV a/de
        // un XMM ES un movimiento FP y DEBE emitirse como MOVSD/MOVSS.  Sin
        // esto el encoder enmascararia el XMM al GP homonimo (`mov rax`), lo que
        // (1) corromperia un f64 vivo y (2) haria que el modelo de efectos del
        // scheduler viera W[xmm] cuando el hardware escribe W[rax] -> perderia
        // el hazard WAW y reordenaria a valores incorrectos.
        const bool fp_phys =
            (d_res.is_reg() && is_xmm(static_cast<MReg>(d_res.reg))) ||
            (s_res.is_reg() && is_xmm(static_cast<MReg>(s_res.reg)));
        if (op == MOp::MOV &&
            (is_fp_operand(in.dst) || is_fp_operand(in.src1) || fp_phys)) {
            const MOp mv =
                fp_mov_for_width(in.dst.width ? in.dst.width : in.src1.width);
            MOperand d = d_res;
            MOperand s = s_res;
            if (d.kind == MOperandKind::MEM && s.kind == MOperandKind::MEM) {
                out.push_back(MInstr::make_unary(mv, xmm(fscr0), s));
                out.push_back(MInstr::make_unary(mv, d, xmm(fscr0)));
            } else {
                out.push_back(MInstr::make_unary(mv, d, s));
            }
            return;
        }

        /* AVX escalar 3-OPERANDOS (VADDSD/VSUBSD/VMULSD/VDIVSD + SS): VX nativo
         * dst = src1 OP src2 con dst != src1 permitido -> NO se legaliza a
         * 2-address (sin el `mov dst,src1`).  Esta es la ventaja que motivo
         * MOps separadas: el regalloc/scheduler las explota como 3-op.  vvvv
         * (src1) DEBE ser reg (materializar si spilled); src2 puede ser MEM (VX
         * reg-reg-mem = load-and-op); dst reg (fscr0 + store si spilled). */
        if (op == MOp::VADDSD || op == MOp::VSUBSD || op == MOp::VMULSD ||
            op == MOp::VDIVSD || op == MOp::VADDSS || op == MOp::VSUBSS ||
            op == MOp::VMULSS || op == MOp::VDIVSS || op == MOp::VXORPS ||
            op == MOp::VANDPS) {
            // VXORPS/VANDPS (fneg/fabs): MOVSD (8B) cubre f64 y f32 (la mascara
            // de signo tiene los bits altos a 0 -> XOR/AND no los altera).
            const bool is_ss = (op == MOp::VADDSS || op == MOp::VSUBSS ||
                                op == MOp::VMULSS || op == MOp::VDIVSS);
            const MOp mv = is_ss ? MOp::MOVSS : MOp::MOVSD;
            const bool dst_spilled =
                in.dst.is_vreg() && ra.spilled(in.dst.vreg_id());
            const MOperand dreg = dst_spilled ? xmm(fscr0) : resolve(in.dst);
            MOperand s1 = resolve(in.src1);
            if (s1.kind == MOperandKind::MEM) { // vvvv debe ser reg
                out.push_back(MInstr::make_unary(mv, xmm(fscr1), s1));
                s1 = xmm(fscr1);
            }
            const MOperand s2 = resolve(in.src2); // reg o MEM (VX lo admite)
            out.push_back(MInstr::make_binary(op, dreg, s1, s2));
            if (dst_spilled)
                out.push_back(MInstr::make_unary(mv, resolve(in.dst), dreg));
            return;
        }

        /* FP arith binaria (3-op pre-legalization): ADDSD/SUBSD/MULSD/DIVSD
         * + variantes SS + XORPS.  Legalizacion 2-address: el dst debe ser un
         * XMM y contener src1 antes de la op (dst = src1 OP src2).  Casos:
         *   - dst spilled -> usar fscr0 como acumulador, store al final.
         *   - src1/src2 spilled -> materializar a XMM (fscr0/fscr1) antes.
         *   - anti-dependencia (dst==src2 reg): para no-conmutativas (SUB/DIV)
         *     mover src2 a fscr1 primero. */
        if (op == MOp::ADDSD || op == MOp::SUBSD || op == MOp::MULSD ||
            op == MOp::DIVSD || op == MOp::ADDSS || op == MOp::SUBSS ||
            op == MOp::MULSS || op == MOp::DIVSS || op == MOp::XORPS ||
            op == MOp::ANDPS || op == MOp::MINSD || op == MOp::MAXSD ||
            op == MOp::MINSS || op == MOp::MAXSS) {
            const bool is_ss = (op == MOp::ADDSS || op == MOp::SUBSS ||
                                op == MOp::MULSS || op == MOp::DIVSS ||
                                op == MOp::MINSS || op == MOp::MAXSS);
            const MOp mv = is_ss ? MOp::MOVSS : MOp::MOVSD;
            const bool commutative =
                (op == MOp::ADDSD || op == MOp::MULSD || op == MOp::ADDSS ||
                 op == MOp::MULSS || op == MOp::XORPS || op == MOp::ANDPS);
            const bool dst_spilled =
                in.dst.is_vreg() && ra.spilled(in.dst.vreg_id());
            /* acumulador: el dst fisico si esta en XMM, o fscr0 si spilled. */
            const MOperand acc = dst_spilled ? xmm(fscr0) : resolve(in.dst);
            MOperand s1 = resolve(in.src1);
            MOperand s2 = resolve(in.src2);
            auto same_reg = [](const MOperand &a, const MOperand &b) {
                return a.kind == MOperandKind::REG &&
                       b.kind == MOperandKind::REG && a.reg == b.reg;
            };
            /* La op SSE 2-address es `OP dst, src` (dst = dst OP src), el
             * encoder lee dst + src1.  Hay que dejar src1 (s1) en acc y
             * aplicar OP acc, s2.  Cuidado con las anti-dependencias:
             *   - s2 == acc (s2 ya esta en el dst): si es CONMUTATIVA basta
             *     `OP acc, s1` (acc tiene s2; acc = s2 OP s1 = s1 OP s2).  Si
             *     NO es conmutativa (SUB/DIV) hay que salvar s2 a fscr1 antes
             *     de pisar acc con s1.
             *   - en otro caso: `mov acc, s1` (si difieren) y luego materializar
             *     s2 a XMM si esta spilled. */
            if (same_reg(s2, acc)) {
                if (commutative) {
                    /* acc ya = s2; aplicar OP acc, s1 (s1 a XMM si MEM). */
                    if (s1.kind == MOperandKind::MEM) {
                        out.push_back(MInstr::make_unary(mv, xmm(fscr1), s1));
                        s1 = xmm(fscr1);
                    }
                    out.push_back(MInstr::make_unary(op, acc, s1));
                } else {
                    /* salvar s2 (en acc) a fscr1, poner s1 en acc, OP acc,fscr1. */
                    out.push_back(MInstr::make_unary(mv, xmm(fscr1), s2));
                    if (!same_reg(acc, s1))
                        out.push_back(MInstr::make_unary(mv, acc, s1));
                    out.push_back(MInstr::make_unary(op, acc, xmm(fscr1)));
                }
            } else {
                /* acc = s1 (si difieren). */
                if (!same_reg(acc, s1))
                    out.push_back(MInstr::make_unary(mv, acc, s1));
                /* s2 debe estar en XMM (reg-reg only). */
                if (s2.kind == MOperandKind::MEM) {
                    out.push_back(MInstr::make_unary(mv, xmm(fscr1), s2));
                    s2 = xmm(fscr1);
                }
                out.push_back(MInstr::make_unary(op, acc, s2));
            }
            if (dst_spilled)
                out.push_back(MInstr::make_unary(
                    mv, slot_mem(ra.slot_of(in.dst.vreg_id())), acc));
            return;
        }

        /* FP unaria con dst XMM (SQRTSD/SQRTSS + ROUNDSD): dst = f(src).  src
         * puede estar spilled -> materializar a fscr1; dst spilled -> fscr0.
         * ROUNDSD lleva el modo de redondeo en @c variant (floor/ceil/round/
         * trunc) -> se propaga al MInstr emitido. */
        if (op == MOp::SQRTSD || op == MOp::SQRTSS || op == MOp::ROUNDSD ||
            op == MOp::ROUNDSS) {
            const MOp mv = (op == MOp::SQRTSS || op == MOp::ROUNDSS)
                               ? MOp::MOVSS
                               : MOp::MOVSD;
            const bool dst_spilled =
                in.dst.is_vreg() && ra.spilled(in.dst.vreg_id());
            const MOperand pdst = dst_spilled ? xmm(fscr0) : resolve(in.dst);
            MOperand s = resolve(in.src1);
            if (s.kind == MOperandKind::MEM) {
                out.push_back(MInstr::make_unary(mv, xmm(fscr1), s));
                s = xmm(fscr1);
            }
            out.push_back(MInstr::make_unary(op, pdst, s));
            out.back().flags = in.flags;     // propagar MI_FLAG_VX_SCALAR
            out.back().variant = in.variant; // ROUNDSD: modo de redondeo
            if (dst_spilled)
                out.push_back(MInstr::make_unary(
                    mv, slot_mem(ra.slot_of(in.dst.vreg_id())), pdst));
            return;
        }

        /* UCOMISD/UCOMISS: comparacion FP (setea flags).  Ambos operandos
         * deben estar en XMM (reg-reg only).  Spilled -> fscr0/fscr1. */
        if (op == MOp::UCOMISD || op == MOp::UCOMISS) {
            const MOp mv = (op == MOp::UCOMISS) ? MOp::MOVSS : MOp::MOVSD;
            MOperand a = resolve(in.dst); // operando A (UCOMISD a, b)
            MOperand b = resolve(in.src1);
            if (a.kind == MOperandKind::MEM) {
                out.push_back(MInstr::make_unary(mv, xmm(fscr0), a));
                a = xmm(fscr0);
            }
            if (b.kind == MOperandKind::MEM) {
                out.push_back(MInstr::make_unary(mv, xmm(fscr1), b));
                b = xmm(fscr1);
            }
            out.push_back(MInstr::make_unary(op, a, b));
            out.back().flags = in.flags; // propagar MI_FLAG_VX_SCALAR
            return;
        }

        /* Conversiones int<->float.  CVTSI2SD/CVTSI2SS: dst XMM <- src GP.
         * CVTTSD2SI/CVTTSS2SI: dst GP <- src XMM.  CVTSS2SD/CVTSD2SS: XMM<-XMM.
         * El operando GP/XMM fuente puede estar spilled (MEM) -> a scratch. */
        if (op == MOp::CVTSI2SD || op == MOp::CVTSI2SS) {
            const MOp mv = (op == MOp::CVTSI2SS) ? MOp::MOVSS : MOp::MOVSD;
            const bool dst_spilled =
                in.dst.is_vreg() && ra.spilled(in.dst.vreg_id());
            const MOperand pdst = dst_spilled ? xmm(fscr0) : resolve(in.dst);
            MOperand s = resolve(in.src1); // GP source
            if (s.kind == MOperandKind::MEM) {
                out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0), s));
                s = reg(scr0);
            }
            out.push_back(MInstr::make_unary(op, pdst, s));
            out.back().flags = in.flags; // propagar MI_FLAG_VX_SCALAR
            if (dst_spilled)
                out.push_back(MInstr::make_unary(
                    mv, slot_mem(ra.slot_of(in.dst.vreg_id())), pdst));
            return;
        }
        if (op == MOp::CVTTSD2SI || op == MOp::CVTTSS2SI) {
            const MOp mv = (op == MOp::CVTTSS2SI) ? MOp::MOVSS : MOp::MOVSD;
            const bool dst_spilled =
                in.dst.is_vreg() && ra.spilled(in.dst.vreg_id());
            const MOperand pdst = dst_spilled ? reg(scr0) : resolve(in.dst);
            MOperand s = resolve(in.src1); // XMM source
            if (s.kind == MOperandKind::MEM) {
                out.push_back(MInstr::make_unary(mv, xmm(fscr1), s));
                s = xmm(fscr1);
            }
            out.push_back(MInstr::make_unary(op, pdst, s));
            out.back().flags = in.flags; // propagar MI_FLAG_VX_SCALAR
            if (dst_spilled)
                out.push_back(MInstr::make_unary(
                    MOp::MOV, slot_mem(ra.slot_of(in.dst.vreg_id())), pdst));
            return;
        }
        if (op == MOp::CVTSS2SD || op == MOp::CVTSD2SS) {
            const bool dst_spilled =
                in.dst.is_vreg() && ra.spilled(in.dst.vreg_id());
            const MOperand pdst = dst_spilled ? xmm(fscr0) : resolve(in.dst);
            MOperand s = resolve(in.src1); // XMM source
            if (s.kind == MOperandKind::MEM) {
                out.push_back(MInstr::make_unary(MOp::MOVSD, xmm(fscr1), s));
                s = xmm(fscr1);
            }
            out.push_back(MInstr::make_unary(op, pdst, s));
            out.back().flags = in.flags; // propagar MI_FLAG_VX_SCALAR
            if (dst_spilled)
                out.push_back(MInstr::make_unary(
                    MOp::MOVSD, slot_mem(ra.slot_of(in.dst.vreg_id())), pdst));
            return;
        }

        /* MOVQ_GP_XMM (dst XMM <- src GP) / MOVQ_XMM_GP (dst GP <- src XMM):
         * bitcast de bits IEEE entre bancos (float CONST, BITCAST).  El
         * operando fuente puede estar spilled.  El dst spilled tambien. */
        if (op == MOp::MOVQ_GP_XMM) {
            const bool dst_spilled =
                in.dst.is_vreg() && ra.spilled(in.dst.vreg_id());
            const MOperand pdst = dst_spilled ? xmm(fscr0) : resolve(in.dst);
            MOperand s = resolve(in.src1); // GP source
            if (s.kind == MOperandKind::MEM) {
                out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0), s));
                s = reg(scr0);
            }
            out.push_back(MInstr::make_unary(MOp::MOVQ_GP_XMM, pdst, s));
            if (dst_spilled)
                out.push_back(MInstr::make_unary(
                    MOp::MOVSD, slot_mem(ra.slot_of(in.dst.vreg_id())), pdst));
            return;
        }
        if (op == MOp::MOVQ_XMM_GP) {
            const bool dst_spilled =
                in.dst.is_vreg() && ra.spilled(in.dst.vreg_id());
            const MOperand pdst = dst_spilled ? reg(scr0) : resolve(in.dst);
            MOperand s = resolve(in.src1); // XMM source
            if (s.kind == MOperandKind::MEM) {
                out.push_back(MInstr::make_unary(MOp::MOVSD, xmm(fscr1), s));
                s = xmm(fscr1);
            }
            out.push_back(MInstr::make_unary(MOp::MOVQ_XMM_GP, pdst, s));
            if (dst_spilled)
                out.push_back(MInstr::make_unary(
                    MOp::MOV, slot_mem(ra.slot_of(in.dst.vreg_id())), pdst));
            return;
        }

        if (op == MOp::CALL_ABS) {
            /* Parallel-move de los args (GP + FP) a sus arg_regs, luego
             * mov scratch,addr + call scratch. */
            marshal_args(out);
            /* Cargar la direccion (imm64 del pool) en scratch y llamar. */
            out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0), in.src1));
            MInstr call;
            call.op = MOp::CALL;
            call.src1 = reg(scr0);
            /* Stackmap (commit 6): describir los GC roots vivos a
             * traves de este call.  El linear_scan los forzo a slots,
             * asi que estan en el stack -> el GC los lee via stackmap.
             * Se asocia al CALL por @c flags (idx); el encoder rellena
             * el pc_offset. */
            if (ivs != nullptr) {
                Stackmap sm;
                sm.frame_size = frame_size_for_scan();
                const uint32_t NVI =
                    static_cast<uint32_t>(ivs->intervals.size());
                for (uint32_t v = 0; v < NVI; ++v) {
                    const LiveInterval &lv = ivs->intervals[v];
                    if (!lv.is_gc() || !lv.covers(cur_call_pos)) continue;
                    if (!ra.spilled(v))
                        continue; // invariante: GC+cross-call -> slot
                    StackmapSlot s;
                    s.rbp_offset =
                        static_cast<int16_t>(slot_off(ra.slot_of(v)));
                    s.gc_kind = static_cast<StackmapGcKind>(
                        static_cast<uint8_t>(lv.gc_kind - 1u));
                    sm.slots.push_back(s);
                }
                call.flags = static_cast<uint16_t>(stackmaps.size());
                stackmaps.push_back(std::move(sm));
            }
            out.push_back(call);
            return;
        }

        if (op == MOp::CALL_SYM) {
            /* AOT: CALL rel32 a una funcion del modulo por nombre.  Igual
             * marshalling que CALL_ABS (parallel-move de los args a los
             * arg_regs del ABI host), pero la llamada es un CALL DIRECTO
             * rel32 (no via scratch): el encoder deja un placeholder +
             * MReloc{CALL_REL32} con el sym_idx que viaja en src1. */
            marshal_args(out);
            MInstr call;
            call.op = MOp::CALL_SYM;
            call.src1 = in.src1;
            /*  AOT-GC (Inc 1): describir los GC roots vivos a traves de
             * este call (gc<T>).  El linear_scan los forzo a slots; el GC los
             * lee via el stackmap.  Antes se omitia ("BARE sin GC"); ahora el
             * gc<T> opt-in los necesita.  Vacio (0 slots) si no hay valores GC
             * vivos -> cero coste para el codigo sin GC. */
            if (ivs != nullptr) {
                Stackmap sm;
                sm.frame_size = frame_size_for_scan();
                const uint32_t NVI =
                    static_cast<uint32_t>(ivs->intervals.size());
                for (uint32_t v = 0; v < NVI; ++v) {
                    const LiveInterval &lv = ivs->intervals[v];
                    if (!lv.is_gc() || !lv.covers(cur_call_pos)) continue;
                    if (!ra.spilled(v)) continue;
                    StackmapSlot s;
                    s.rbp_offset =
                        static_cast<int16_t>(slot_off(ra.slot_of(v)));
                    s.gc_kind = static_cast<StackmapGcKind>(
                        static_cast<uint8_t>(lv.gc_kind - 1u));
                    sm.slots.push_back(s);
                }
                call.flags = static_cast<uint16_t>(stackmaps.size());
                stackmaps.push_back(std::move(sm));
            }
            out.push_back(call);
            return;
        }

        if (op == MOp::MOV_SYM || op == MOp::LEA_RIP_SYM ||
            op == MOp::LEA_LABEL || op == MOp::TLS_LE_ADDR ||
            op == MOp::TLS_PE_ADDR) {
            /* AOT: dst = &simbolo (.rodata) o &thread_local (TLS).  MOV_SYM =
             * abs (mov imm64, --no-pie); LEA_RIP_SYM = RIP-rel (lea, default
             * PIC); LEA_LABEL = direccion nativa de un label local (in-JIT
             * catch); TLS_LE_ADDR = direccion por-hilo (mov %fs:0 + lea@tpoff).
             * Resolver el dst vreg a fisico y emitir la instr fisica; el
             * encoder deja el placeholder + MReloc/MFixup.  dst spilled ->
             * scratch + store. */
            MOperand d = resolve(in.dst);
            if (d.is_reg()) {
                MInstr m;
                m.op = op;
                m.dst = d;
                m.src1 = in.src1;
                m.src2 = in.src2; /* TLS_PE_ADDR: 2o simbolo (_tls_index) */
                out.push_back(m);
            } else {
                MInstr m;
                m.op = op;
                m.dst = reg(scr0);
                m.src1 = in.src1;
                m.src2 = in.src2;
                out.push_back(m);
                out.push_back(MInstr::make_unary(MOp::MOV, d, reg(scr0)));
            }
            return;
        }

        if (op == MOp::CALL) {
            /* CALL INDIRECTO (call reg/mem): el target es un vreg (el
             * jit_code del inline-dispatch VM_ABI, o el func_ptr de la
             * vtable en HOST_LEAF CALLIND -- AOT.2.c).  Emite el stackmap
             * de los GC roots vivos a traves del call. */
            MOperand tgt = resolve(in.src1);
            /* HOST_LEAF CALLIND: hay ARGs pendientes que marshalar a los
             * arg_regs del ABI host.  CRiTICO: capturar el func_ptr a scr0
             * (R10) ANTES del marshal (su loc actual puede caer en un arg_reg
             * que el parallel-move pisaria); luego call scr0.  marshal_args usa
             * scr1 (R11) -- NO scr0 -- como scratch de los stack-args mem->mem,
             * asi el target en scr0 SOBREVIVE al marshal (RAX no sirve: el
             * allocator puede colocar un arg en RAX -> `mov rax,tgt` lo
             * pisaria; visto con 12 args -> resultado erroneo). */
            if (!pending_args.empty()) {
                out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0), tgt));
                tgt = reg(scr0);
                marshal_args(out);
            } else if (tgt.kind == MOperandKind::MEM) {
                /* target spilled -> cargar a scratch antes del call. */
                out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0), tgt));
                tgt = reg(scr0);
            }
            MInstr call;
            call.op = MOp::CALL;
            call.src1 = tgt;
            if (ivs != nullptr) {
                Stackmap sm;
                sm.frame_size = frame_size_for_scan();
                const uint32_t NVI =
                    static_cast<uint32_t>(ivs->intervals.size());
                for (uint32_t v = 0; v < NVI; ++v) {
                    const LiveInterval &lv = ivs->intervals[v];
                    if (!lv.is_gc() || !lv.covers(cur_call_pos)) continue;
                    if (!ra.spilled(v)) continue;
                    StackmapSlot s;
                    s.rbp_offset =
                        static_cast<int16_t>(slot_off(ra.slot_of(v)));
                    s.gc_kind = static_cast<StackmapGcKind>(
                        static_cast<uint8_t>(lv.gc_kind - 1u));
                    sm.slots.push_back(s);
                }
                call.flags = static_cast<uint16_t>(stackmaps.size());
                stackmaps.push_back(std::move(sm));
            }
            out.push_back(call);
            return;
        }

        if (op == MOp::TAILCALL) {
            /* AOT HOST_LEAF tail-call (TCO genuina, src2 = IMM32 sym_idx):
             * parallel-move de los args (ARG) a los arg_regs del ABI host
             * ANTES del teardown (lee las ubicaciones validas con el frame
             * aun montado; los arg_regs son caller-saved -> sobreviven el
             * epilogue) + emit_epilogue + JMP_SYM al callee.  El callee
             * monta su propio frame y su RET retorna al caller original ->
             * pila O(1).  Sin GC en BARE -> sin stackmap. */
            if (!vm_abi && in.src2.kind == MOperandKind::IMM32) {
                marshal_args(out);
                emit_epilogue(out);
                MInstr j;
                j.op = MOp::JMP_SYM;
                j.src1 = in.src2;
                out.push_back(j);
                return;
            }
            /* TCO con reuso de frame (mismo patron que el frame-swap del
             * OSR 2c): proc -> A0 (sobrevive el teardown por ser
             * caller-saved; el prologue del callee hace mov rbx,A0) +
             * emit_epilogue (desmonta el frame; rsp queda en la return
             * address del caller) + jmp al target.  El RET del callee
             * retorna al caller original -> pila O(1).  No hay GC entre
             * el teardown y la reentrada (sin safepoint/call) -> los args
             * en proc->registers (ya escritos) siguen como roots. */
            const auto &areg = tri.arg_regs[static_cast<size_t>(RegClass::GP)];
            if (vm_abi && !areg.empty())
                out.push_back(MInstr::make_unary(
                    MOp::MOV, reg(static_cast<MReg>(areg[0])), reg(MReg::RBX)));
            emit_epilogue(out);
            if (in.src1.kind == MOperandKind::LABEL) {
                /* self-tail-call: jmp rel32 a code+0 (label bloque 0). */
                MInstr j;
                j.op = MOp::JMP;
                j.src1 = in.src1;
                out.push_back(j);
            } else {
                /* cross-fn: mov scr0, addr(imm64) + jmp scr0. */
                out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0), in.src1));
                MInstr j;
                j.op = MOp::JMP;
                j.src1 = reg(scr0);
                out.push_back(j);
            }
            return;
        }

        if (op == MOp::RET) {
            /*  NR @Naked: NO emitir el ret implicito; el cuerpo (asm)
             * provee la salida real (ret/iretq).  Un ret aqui seria, en el
             * mejor caso, codigo muerto tras un iretq; en el peor, pisaria
             * la convencion de interrupcion. */
            if (naked) return;
            emit_epilogue(out);
            out.push_back(MInstr::make_ret());
            return;
        }

        if (op == MOp::DIVMOD_V) {
            /* dst = src1 / src2 (variant 0) o src1 % src2 (variant 1),
             * signed.  Secuencia x86 con RAX:RDX fijos.  El divisor va a
             * R11 (scr1, reservado -> nunca es un operando, sin aliasing)
             * ANTES de tocar RAX/RDX, asi cualquier asignacion fisica de
             * los operandos (incluida RAX/RDX) es segura:
             *   mov  r11, divisor      ; divisor leido antes de clobbers
             *   mov  rax, dividendo    ; (no-op si ya en RAX)
             *   cqo                    ; sign-extend RAX -> RDX:RAX
             *   idiv r11               ; RAX=cociente, RDX=resto
             *   mov  dst, rax|rdx
             * DIVMOD_V es call-position -> ningun vreg vivo a traves esta
             * en RAX/RDX (van a callee-saved), asi el clobber es seguro. */
            const MOperand a = resolve(in.src1); // dividendo
            const MOperand b = resolve(in.src2); // divisor
            const bool uns = (in.variant & 2u) != 0u; // bit1 = unsigned
            out.push_back(MInstr::make_unary(MOp::MOV, reg(scr1), b));
            out.push_back(MInstr::make_unary(MOp::MOV, reg(MReg::RAX), a));
            if (uns) {
                /* unsigned: RDX = 0 (xor rdx,rdx) + DIV (F7 /6).  Sin cqo:
                 * el dividendo es u64, RDX:RAX = 0:dividendo. */
                out.push_back(MInstr::make_binary(MOp::XOR, reg(MReg::RDX),
                                                  reg(MReg::RDX),
                                                  reg(MReg::RDX)));
                out.push_back(
                    MInstr::make_unary(MOp::DIV_U, MOperand{}, reg(scr1)));
            } else {
                /* signed: CQO (sign-extend RAX -> RDX:RAX) + IDIV (F7 /7). */
                MInstr c;
                c.op = MOp::CQO;
                out.push_back(c);
                out.push_back(
                    MInstr::make_unary(MOp::IDIV, MOperand{}, reg(scr1)));
            }
            const MReg res = (in.variant & 1u) ? MReg::RDX : MReg::RAX;
            out.push_back(
                MInstr::make_unary(MOp::MOV, resolve(in.dst), reg(res)));
            return;
        }

        if (op == MOp::SHIFT_V) {
            /* dst = src1 <shift> src2 (cuenta variable).  x86 exige la cuenta en
             * CL: el selector la pineo a RCX (tmp de vida corta).  Usamos R11
             * (scr1, reservado) como reg de trabajo -> nunca clobbea un vivo:
             *   mov  r11, value    ; value != RCX (interfiere con la cuenta)
             *   shift r11, cl      ; CL = RCX (la cuenta)
             *   mov  dst, r11
             * variant: 0=SHL, 1=SHR, 2=SAR. */
            const MOperand v = resolve(in.src1); // value (!= RCX)
            const MOperand c = resolve(in.src2); // cuenta (pineada a RCX)
            /* Defensivo: garantizar la cuenta en RCX (no-op si el pin la dejo
             * ya ahi; cubre un eventual spill del tmp). */
            if (!(c.kind == MOperandKind::REG &&
                  c.reg == static_cast<uint8_t>(MReg::RCX)))
                out.push_back(MInstr::make_unary(MOp::MOV, reg(MReg::RCX), c));
            out.push_back(MInstr::make_unary(MOp::MOV, reg(scr1), v));
            const MOp mop = (in.variant == 0u)   ? MOp::SHL
                            : (in.variant == 1u) ? MOp::SHR
                            : (in.variant == 2u) ? MOp::SAR
                            : (in.variant == 3u) ? MOp::ROL
                                                 : MOp::ROR;
            MInstr sh{};
            sh.op = mop;
            sh.dst = reg(scr1);     // r11 = valor de trabajo
            sh.src1 = reg(MReg::RCX); // CL -> el encoder emite la forma 0xD3
            out.push_back(sh);
            out.push_back(MInstr::make_unary(MOp::MOV, resolve(in.dst),
                                             reg(scr1)));
            return;
        }

        if (op == MOp::ATOMICADD_V) {
            /* lock xadd [addr], val.  dst (in/out) trae delta y sale con el
             * valor viejo; src1 = addr.  SIN registro fijo: dst y addr los
             * asigno el allocator.  Spills -> scr1 (r11) para addr y un
             * caller-saved libre (r10/r9) para el valor.  call-position ->
             * los caller-saved no tienen vregs vivos aparte de estos operandos. */
            const MOperand d = resolve(in.dst);  // delta in / old out
            const MOperand a = resolve(in.src1); // addr
            MReg base;
            if (a.kind == MOperandKind::MEM) {
                out.push_back(MInstr::make_unary(MOp::MOV, reg(scr1), a));
                base = scr1;
            } else {
                base = static_cast<MReg>(a.reg);
            }
            MReg valreg;
            const bool val_spilled = (d.kind == MOperandKind::MEM);
            if (val_spilled) {
                valreg = (base != MReg::R10) ? MReg::R10 : MReg::R9;
                out.push_back(MInstr::make_unary(MOp::MOV, reg(valreg), d));
            } else {
                valreg = static_cast<MReg>(d.reg);
            }
            MInstr xa{};
            xa.op = MOp::LOCK_XADD;
            xa.dst = MOperand::make_mem(base, 0); // [addr]
            xa.src1 = reg(valreg);                // valor (in/out)
            out.push_back(xa);
            if (val_spilled) {
                out.push_back(MInstr::make_unary(MOp::MOV, d, reg(valreg)));
            }
            return;
        }

        if (op == MOp::ATOMICCAS_V) {
            /* lock cmpxchg [addr], desired.  dst (in/out) esta PRECOLOREADO a
             * RAX (obligado por la ISA de cmpxchg): entra expected, sale old.
             * addr y desired son LIBRES (el precoloreo garantiza que no caen en
             * RAX).  Spills -> scr1 (r11) para addr, caller-saved libre para
             * desired.  call-position: los caller-saved estan libres. */
            const MOperand a = resolve(in.src1);   // addr (!= RAX)
            const MOperand des = resolve(in.src2); // desired (!= RAX)
            MReg base;
            if (a.kind == MOperandKind::MEM) {
                out.push_back(MInstr::make_unary(MOp::MOV, reg(scr1), a));
                base = scr1;
            } else {
                base = static_cast<MReg>(a.reg);
            }
            MReg srcreg;
            if (des.kind == MOperandKind::MEM) {
                srcreg = (base != MReg::R10) ? MReg::R10 : MReg::R9;
                out.push_back(MInstr::make_unary(MOp::MOV, reg(srcreg), des));
            } else {
                srcreg = static_cast<MReg>(des.reg);
            }
            MInstr cx{};
            cx.op = MOp::LOCK_CMPXCHG;
            cx.dst = MOperand::make_mem(base, 0); // [addr]
            cx.src1 = reg(srcreg);                // desired
            out.push_back(cx);
            /* El viejo queda en RAX = resolve(in.dst) (precoloreado); el selector
             * ya emitio el MOV final dst_ir <- rax_v.  Nada mas que hacer. */
            return;
        }

        /* LOAD float HOST: dst es un vreg FP -> MOVSD/MOVSS xmm, [addr]. */
        if (op == MOp::LOAD && is_fp_operand(in.dst)) {
            const uint8_t width = static_cast<uint8_t>(in.flags >> 1);
            MOperand a = resolve(in.src1);
            MReg addr_reg;
            if (a.kind == MOperandKind::MEM) {
                out.push_back(MInstr::make_unary(MOp::MOV, reg(scr1), a));
                addr_reg = scr1;
            } else {
                addr_reg = static_cast<MReg>(a.reg);
            }
            /* P2 SIB: si src2 es IMM32, es el disp fusionado (`[base+disp]`),
             * igual que en la ruta GP de abajo.  Ignorarlo (disp 0) hacia que
             * TODOS los campos float de un struct se leyeran del offset 0: un
             * `struct Rect { Punto min; Punto max; }` con f64 devolvia
             * min.x para max.x, etc. -- silencioso y con valores plausibles. */
            const int32_t fld_disp =
                (in.src2.kind == MOperandKind::IMM32) ? in.src2.value : 0;
            const MOperand mem = MOperand::make_mem(addr_reg, fld_disp);
            const MOp mv = fp_mov_for_width(width ? width : 8);
            const bool dst_spilled =
                in.dst.is_vreg() && ra.spilled(in.dst.vreg_id());
            const MOperand pdst = dst_spilled ? xmm(fscr0) : resolve(in.dst);
            out.push_back(MInstr::make_unary(mv, pdst, mem));
            if (dst_spilled)
                out.push_back(MInstr::make_unary(
                    mv, slot_mem(ra.slot_of(in.dst.vreg_id())), xmm(fscr0)));
            return;
        }

        /* STORE float HOST: el valor (src2) es un vreg FP -> MOVSD/MOVSS. */
        if (op == MOp::STORE && is_fp_operand(in.src2)) {
            const uint8_t width = static_cast<uint8_t>(in.flags);
            MOperand a = resolve(in.src1);
            MReg addr_reg;
            if (a.kind == MOperandKind::MEM) {
                out.push_back(MInstr::make_unary(MOp::MOV, reg(scr1), a));
                addr_reg = scr1;
            } else {
                addr_reg = static_cast<MReg>(a.reg);
            }
            const MOp mv = fp_mov_for_width(width ? width : 8);
            MOperand v = resolve(in.src2);
            if (v.kind == MOperandKind::MEM) {
                out.push_back(MInstr::make_unary(mv, xmm(fscr0), v));
                v = xmm(fscr0);
            }
            const MOperand mem = MOperand::make_mem(addr_reg, 0);
            out.push_back(MInstr::make_unary(mv, mem, v));
            return;
        }

        if (op == MOp::LOAD) {
            /* dst = [addr].  addr y dst pueden estar spilled. */
            const uint8_t width = static_cast<uint8_t>(in.flags >> 1);
            const bool sgn = (in.flags & 1u) != 0u;
            MOperand a = resolve(in.src1);
            MReg addr_reg;
            if (a.kind == MOperandKind::MEM) {
                out.push_back(MInstr::make_unary(MOp::MOV, reg(scr1), a));
                addr_reg = scr1;
            } else {
                addr_reg = static_cast<MReg>(a.reg);
            }
            /* NO tocar mem.width: empaqueta scale|index del MEM.  El
             * ancho de un MOV [mem] lo da el reg; el de MOVSX/MOVZX va
             * en mem.flags (mem_size override).
             * P2 SIB: si src2 es IMM32, es el disp fusionado (`[base+disp]`). */
            const int32_t ld_disp =
                (in.src2.kind == MOperandKind::IMM32) ? in.src2.value : 0;
            MOperand mem = MOperand::make_mem(addr_reg, ld_disp);
            const bool dst_spilled =
                in.dst.is_vreg() && ra.spilled(in.dst.vreg_id());
            MOperand pdst = dst_spilled ? reg(scr0) : resolve(in.dst);
            if (width == 8) {
                out.push_back(MInstr::make_unary(MOp::MOV, pdst, mem));
            } else if (width == 4 && !sgn) {
                /* u32: `mov r32, [mem]` zero-extiende a r64 por hardware
                 * (no hay MOVZX de 32->64).  pdst a 32 bits -> sin REX.W. */
                MOperand d32 = pdst;
                if (d32.kind == MOperandKind::REG) d32.width = 4;
                out.push_back(MInstr::make_unary(MOp::MOV, d32, mem));
            } else if (sgn) {
                mem.flags = width; // ancho del src para MOVSX
                out.push_back(MInstr::make_unary(MOp::MOVSX, pdst, mem));
            } else { // u8/u16
                mem.flags = width;
                out.push_back(MInstr::make_unary(MOp::MOVZX, pdst, mem));
            }
            if (dst_spilled)
                out.push_back(MInstr::make_unary(
                    MOp::MOV, slot_mem(ra.slot_of(in.dst.vreg_id())), pdst));
            return;
        }

        if (op == MOp::STORE) {
            /* [addr] = val.  addr -> scr1 si spilled; val -> scr0 si spilled.
             */
            const uint8_t width = static_cast<uint8_t>(in.flags);
            MOperand a = resolve(in.src1);
            MReg addr_reg;
            if (a.kind == MOperandKind::MEM) {
                out.push_back(MInstr::make_unary(MOp::MOV, reg(scr1), a));
                addr_reg = scr1;
            } else {
                addr_reg = static_cast<MReg>(a.reg);
            }
            MOperand v = resolve(in.src2);
            if (v.kind == MOperandKind::MEM) {
                out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0), v));
                v = reg(scr0);
            }
            v.width = width; // ancho del store lo da el reg src
            /* NO tocar mem.width (index packing). */
            MOperand mem = MOperand::make_mem(addr_reg, 0);
            out.push_back(MInstr::make_unary(MOp::MOV, mem, v));
            return;
        }

        if (op == MOp::LOAD_VM || op == MOp::STORE_VM) {
            /* Acceso a memoria del VM (vaddr, NO host_ptr).  La memoria
             * del VM no es contigua (TLB 3-niveles) -> page-cache INLINE
             * de 1 entrada (replica de selector.cpp): si la pagina del
             * vaddr coincide con la cacheada en proc->vm_mem y no cruza
             * el limite, traduce inline (~hit ~8 instr SIN call); si no,
             * CALL a vrt_vm_read/write_u<w>.  scr0/scr1 (R10/R11, reserva-
             * dos) son los temporales del hit; areg/val se re-leen de su
             * ubicacion canonica (resolve) en el miss -> intactos.  Es
             * call-position (el miss clobbea caller-saved) -> los vregs
             * vivos a traves ya estan en callee-saved/spill.
             * NOTA: vrt_vm_read/write NO disparan GC (solo traducen) ->
             * el CALL del miss no es safepoint -> sin stackmap. */
            const bool is_store = (op == MOp::STORE_VM);
            const uint8_t width = is_store
                                      ? static_cast<uint8_t>(in.flags)
                                      : static_cast<uint8_t>(in.flags >> 1);
            const bool sgn = !is_store && ((in.flags & 1u) != 0u);
            /* imm64_idx con la direccion de vrt_vm_read/write_u<w>:
             * src2 en LOAD_VM, dst en STORE_VM (operandos libres). */
            const MOperand fn_imm = is_store ? in.dst : in.src2;
            const MOperand areg = resolve(in.src1); // vaddr (canonica)
            const MOperand vval = is_store ? resolve(in.src2) : MOperand{};
            /* dst del LOAD: reg fisico o scr0 si spilled. */
            const bool dst_spilled =
                !is_store && in.dst.is_vreg() && ra.spilled(in.dst.vreg_id());
            MReg pr = MReg::RAX;
            if (!is_store)
                pr =
                    dst_spilled ? scr0 : static_cast<MReg>(resolve(in.dst).reg);

            const bool inline_ok = vesta_rt::kProcVmMemOffset != 0 &&
                                   vesta_rt::kVmMemCachedPageVaddrOffset >= 0 &&
                                   vesta_rt::kVmMemCachedPageHostOffset >= 0;
            const int32_t page_v = vesta_rt::kProcVmMemOffset +
                                   vesta_rt::kVmMemCachedPageVaddrOffset;
            const int32_t page_h = vesta_rt::kProcVmMemOffset +
                                   vesta_rt::kVmMemCachedPageHostOffset;
#if defined(_WIN32)
            const MReg A0 = MReg::RCX, A1 = MReg::RDX, A2 = MReg::R8;
#else
            const MReg A0 = MReg::RDI, A1 = MReg::RSI, A2 = MReg::RDX;
#endif
            const MLabelId Lmiss = inline_ok ? pf->new_label() : MLABEL_INVALID;
            const MLabelId Ldone = inline_ok ? pf->new_label() : MLABEL_INVALID;

            if (inline_ok) {
                /* --- HIT path --- */
                out.push_back(MInstr::make_unary(MOp::MOV, reg(scr1), areg));
                out.push_back(
                    MInstr::make_unary(MOp::MOV, reg(scr0), reg(scr1)));
                out.push_back(MInstr::make_unary(MOp::AND, reg(scr0),
                                                 MOperand::make_imm32(-4096)));
                out.push_back(
                    MInstr::make_unary(MOp::CMP, reg(scr0),
                                       MOperand::make_mem(MReg::RBX, page_v)));
                out.push_back(MInstr::make_jcc(MCond::NE, Lmiss));
                out.push_back(MInstr::make_unary(
                    MOp::AND, reg(scr1),
                    MOperand::make_imm32(4095))); // scr1 = offset
                if (width > 1) {                  // cross-page check
                    out.push_back(MInstr::make_unary(
                        MOp::CMP, reg(scr1),
                        MOperand::make_imm32(4096 -
                                             static_cast<int32_t>(width))));
                    out.push_back(MInstr::make_jcc(MCond::A, Lmiss));
                }
                out.push_back(MInstr::make_unary(
                    MOp::MOV, reg(scr0),
                    MOperand::make_mem(MReg::RBX, page_h))); // cached_host
                out.push_back(MInstr::make_unary(MOp::ADD, reg(scr0),
                                                 reg(scr1))); // scr0 = host_ptr
                if (is_store) {
                    MOperand v = vval;
                    if (v.kind == MOperandKind::MEM) {
                        out.push_back(
                            MInstr::make_unary(MOp::MOV, reg(scr1), v));
                        v = reg(scr1);
                    }
                    v.width = width; // ancho lo da el reg
                    out.push_back(MInstr::make_unary(
                        MOp::MOV, MOperand::make_mem(scr0, 0), v));
                } else {
                    MOperand mem = MOperand::make_mem(scr0, 0);
                    if (width >= 8) {
                        out.push_back(MInstr::make_unary(
                            MOp::MOV, MOperand::make_reg(pr, 8), mem));
                    } else if (sgn) {
                        mem.flags = width; // mem_size override
                        out.push_back(MInstr::make_unary(
                            MOp::MOVSX, MOperand::make_reg(pr, 8), mem));
                    } else if (width == 4) {
                        out.push_back(MInstr::make_unary(
                            MOp::MOV, MOperand::make_reg(pr, 4),
                            mem)); // zero-ext
                    } else {
                        mem.flags = width;
                        out.push_back(MInstr::make_unary(
                            MOp::MOVZX, MOperand::make_reg(pr, 8), mem));
                    }
                }
                out.push_back(MInstr::make_jmp(Ldone));
                out.push_back(MInstr::make_label_def(Lmiss));
            }

            /* --- FALLBACK CALL (page-miss) ---
             * Cargo vaddr (y val) a scratch ANTES de los arg-movs para
             * no depender del orden de los arg_regs (robusto ante
             * cualquier ubicacion fisica de areg/vval). */
            out.push_back(MInstr::make_unary(MOp::MOV, reg(scr1), areg));
            if (is_store) {
                /* val a scr0 (reg o mem, mov reg,* lo cubre). */
                out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0), vval));
                out.push_back(
                    MInstr::make_unary(MOp::MOV, reg(A0), reg(MReg::RBX)));
                out.push_back(MInstr::make_unary(MOp::MOV, reg(A1), reg(scr1)));
                out.push_back(MInstr::make_unary(MOp::MOV, reg(A2), reg(scr0)));
                out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0), fn_imm));
                MInstr call;
                call.op = MOp::CALL;
                call.src1 = reg(scr0);
                out.push_back(call);
            } else {
                out.push_back(
                    MInstr::make_unary(MOp::MOV, reg(A0), reg(MReg::RBX)));
                out.push_back(MInstr::make_unary(MOp::MOV, reg(A1), reg(scr1)));
                out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0), fn_imm));
                MInstr call;
                call.op = MOp::CALL;
                call.src1 = reg(scr0);
                out.push_back(call);
                /* resultado en RAX -> pr (igual que el selector). */
                if (sgn && width < 8) {
                    MOperand src = MOperand::make_reg(MReg::RAX, width);
                    out.push_back(MInstr::make_unary(
                        MOp::MOVSX, MOperand::make_reg(pr, 8), src));
                } else {
                    out.push_back(MInstr::make_unary(
                        MOp::MOV, MOperand::make_reg(pr, 8), reg(MReg::RAX)));
                }
            }

            if (inline_ok) out.push_back(MInstr::make_label_def(Ldone));
            if (dst_spilled)
                out.push_back(MInstr::make_unary(
                    MOp::MOV, slot_mem(ra.slot_of(in.dst.vreg_id())),
                    reg(scr0)));
            return;
        }

        if (op == MOp::ALLOCA) {
            /* dst = host_ptr a `size` bytes reservados en el frame
             * (debajo de los spills).  LEA dst, [rbp - off]. */
            const uint32_t size = static_cast<uint32_t>(in.src1.value);
            const uint32_t aligned = (size + 7u) & ~7u;
            const int32_t off =
                static_cast<int32_t>(alloca_base + alloca_cursor + aligned);
            alloca_cursor += aligned;
            const MOperand mem = MOperand::make_mem(MReg::RBP, -off);
            const bool dst_spilled =
                in.dst.is_vreg() && ra.spilled(in.dst.vreg_id());
            const MOperand pdst = dst_spilled ? reg(scr0) : resolve(in.dst);
            out.push_back(MInstr::make_unary(MOp::LEA, pdst, mem));
            if (dst_spilled)
                out.push_back(MInstr::make_unary(
                    MOp::MOV, slot_mem(ra.slot_of(in.dst.vreg_id())), pdst));
            return;
        }

        if (op == MOp::ALLOCA_VM) {
            /* dst = vaddr a `size` bytes del VM stack del proceso:
             *   mov scr0, [rbx+SP]; sub scr0, aligned; mov [rbx+SP],scr0
             *   dst = scr0.
             * scr0 (R10) es scratch reservado; RBX trae el ProcessVM*.
             * El prologue ya salvo el VM-RSP original y el epilogue lo
             * restaura, asi que la resta es local al frame. */
            const uint32_t size = static_cast<uint32_t>(in.src1.value);
            const uint32_t aligned = (size + 15u) & ~15u; // 16-align
            const MOperand sp_mem =
                MOperand::make_mem(MReg::RBX, VESTA_PROC_STACK_POINTER_OFFSET);
            out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0), sp_mem));
            if (aligned > 0)
                out.push_back(MInstr::make_unary(
                    MOp::SUB, reg(scr0),
                    MOperand::make_imm32(static_cast<int32_t>(aligned))));
            out.push_back(MInstr::make_unary(MOp::MOV, sp_mem, reg(scr0)));
            const bool dst_spilled =
                in.dst.is_vreg() && ra.spilled(in.dst.vreg_id());
            if (dst_spilled)
                out.push_back(MInstr::make_unary(
                    MOp::MOV, slot_mem(ra.slot_of(in.dst.vreg_id())),
                    reg(scr0)));
            else
                out.push_back(
                    MInstr::make_unary(MOp::MOV, resolve(in.dst), reg(scr0)));
            return;
        }

        if (op == MOp::MOV) {
            const MOperand rs = resolve(in.src1);
            if (in.dst.is_vreg() && ra.spilled(in.dst.vreg_id())) {
                const uint32_t slot = ra.slot_of(in.dst.vreg_id());
                if (rs.kind == MOperandKind::REG) {
                    out.push_back(
                        MInstr::make_unary(MOp::MOV, slot_mem(slot), rs));
                } else {
                    /* mem<-mem o mem<-imm: pasar por scratch. */
                    out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0), rs));
                    out.push_back(MInstr::make_unary(MOp::MOV, slot_mem(slot),
                                                     reg(scr0)));
                }
            } else {
                /* dst no-spilled (reg) o dst MEM fisico (p.ej. el
                 * return VM a [rbx+off]).  Si AMBOS son MEM (dst MEM
                 * fisico + src spilled), pasar por scratch (no hay
                 * mov mem,mem en x86). */
                const MOperand d = resolve(in.dst);
                if (d.kind == MOperandKind::MEM &&
                    rs.kind == MOperandKind::MEM) {
                    out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0), rs));
                    out.push_back(MInstr::make_unary(MOp::MOV, d, reg(scr0)));
                } else {
                    out.push_back(MInstr::make_unary(MOp::MOV, d, rs));
                }
            }
            return;
        }

        if (op == MOp::MOVZX || op == MOp::MOVSX) {
            /* Extension: dst64 <- src<width>.  El encoder exige
             * dst=REG; si el src esta spilled, cargarlo a scratch1
             * preservando su width; si el dst esta spilled, extender
             * a scratch0 y almacenar. */
            MOperand rs = resolve(in.src1);
            if (rs.kind == MOperandKind::MEM) {
                out.push_back(MInstr::make_unary(MOp::MOV, reg(scr1), rs));
                rs = MOperand::make_reg(scr1, in.src1.width); // width del src
            }
            const bool dst_spilled =
                in.dst.is_vreg() && ra.spilled(in.dst.vreg_id());
            const MOperand pdst = dst_spilled ? reg(scr0) : resolve(in.dst);
            MInstr ext;
            ext.op = op;
            ext.dst = pdst;
            ext.src1 = rs;
            out.push_back(ext);
            if (dst_spilled)
                out.push_back(MInstr::make_unary(
                    MOp::MOV, slot_mem(ra.slot_of(in.dst.vreg_id())), pdst));
            return;
        }

        if (op == MOp::SHL || op == MOp::SHR || op == MOp::SAR ||
            op == MOp::ROL || op == MOp::ROR) {
            /* dst = src1 (sh/rot) src2(imm).  MOV dst, src1; SHL dst, imm.
             * Solo cantidad INMEDIATA (el selector emite ROL/ROR aqui solo
             * con count constante; variable -> fallback, requiere CL). */
            const bool dst_spilled =
                in.dst.is_vreg() && ra.spilled(in.dst.vreg_id());
            const MOperand pdst = dst_spilled ? reg(scr0) : resolve(in.dst);
            const MOperand rs1 = resolve(in.src1);
            if (!(pdst.kind == MOperandKind::REG &&
                  rs1.kind == MOperandKind::REG && pdst.reg == rs1.reg))
                out.push_back(MInstr::make_unary(MOp::MOV, pdst, rs1));
            MInstr sh;
            sh.op = op;
            sh.dst = pdst;
            sh.src1 = in.src2; // imm
            out.push_back(sh);
            if (dst_spilled)
                out.push_back(MInstr::make_unary(
                    MOp::MOV, slot_mem(ra.slot_of(in.dst.vreg_id())), pdst));
            return;
        }

        if (op == MOp::LZCNT || op == MOp::TZCNT || op == MOp::POPCNT) {
            /* dst = op(src).  dst debe ser REG (si spilled -> scratch
             * + store).  src puede ser MEM (op r64, r/m64). */
            const MOperand rs = resolve(in.src1);
            const bool dst_spilled =
                in.dst.is_vreg() && ra.spilled(in.dst.vreg_id());
            const MOperand pdst = dst_spilled ? reg(scr0) : resolve(in.dst);
            out.push_back(MInstr::make_unary(op, pdst, rs));
            if (dst_spilled)
                out.push_back(MInstr::make_unary(
                    MOp::MOV, slot_mem(ra.slot_of(in.dst.vreg_id())), pdst));
            return;
        }

        if (op == MOp::BSWAP) {
            /* bswap r64: in-place, dst debe ser REG.  Si dst spilled,
             * cargar a scratch, bswap, store.  src1 == dst (mismo vreg). */
            const bool dst_spilled =
                in.dst.is_vreg() && ra.spilled(in.dst.vreg_id());
            if (dst_spilled) {
                const MOperand sl = slot_mem(ra.slot_of(in.dst.vreg_id()));
                out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0), sl));
                out.push_back(
                    MInstr::make_unary(MOp::BSWAP, reg(scr0), reg(scr0)));
                out.push_back(MInstr::make_unary(MOp::MOV, sl, reg(scr0)));
            } else {
                const MOperand pdst = resolve(in.dst);
                out.push_back(MInstr::make_unary(MOp::BSWAP, pdst, pdst));
            }
            return;
        }

        if (op == MOp::CMOVCC) {
            /* dst = (cc) ? src : dst  (read-modify).  dst debe ser REG.
             * Preserva variant (codigo de condicion).  src spilled -> scr1;
             * dst spilled -> carga a scr0, cmov, store. */
            MOperand rsrc = resolve(in.src1);
            if (rsrc.kind == MOperandKind::MEM) {
                out.push_back(MInstr::make_unary(MOp::MOV, reg(scr1), rsrc));
                rsrc = reg(scr1);
            }
            const bool dst_spilled =
                in.dst.is_vreg() && ra.spilled(in.dst.vreg_id());
            MInstr c;
            c.op = MOp::CMOVCC;
            c.variant = in.variant;
            if (dst_spilled) {
                const MOperand sl = slot_mem(ra.slot_of(in.dst.vreg_id()));
                out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0), sl));
                c.dst = reg(scr0);
                c.src1 = rsrc;
                out.push_back(c);
                out.push_back(MInstr::make_unary(MOp::MOV, sl, reg(scr0)));
            } else {
                c.dst = resolve(in.dst);
                c.src1 = rsrc;
                out.push_back(c);
            }
            return;
        }

        if (is_bin_alu(op)) {
            const bool dst_spilled =
                in.dst.is_vreg() && ra.spilled(in.dst.vreg_id());
            const MOperand pdst = dst_spilled ? reg(scr0) : resolve(in.dst);
            const MOperand rs1 = resolve(in.src1);
            const MOperand rs2 = resolve(in.src2);

            /* P3 imm-forms: IMUL con src2 inmediato -> forma 3-op NO
             * destructiva `imul pdst, src1, imm` (0x69/0x6B).  Ahorra el
             * `mov pdst, src1` del 2-address.  src1 debe ser reg (si spilled,
             * a scratch); el encoder no acepta un imul con fuente en memoria. */
            if (op == MOp::IMUL && in.src2.kind == MOperandKind::IMM32) {
                MOperand s1 = rs1;
                if (s1.kind == MOperandKind::MEM) {
                    out.push_back(MInstr::make_unary(MOp::MOV, reg(scr1), s1));
                    s1 = reg(scr1);
                }
                out.push_back(MInstr::make_binary(MOp::IMUL, pdst, s1, in.src2));
                if (dst_spilled)
                    out.push_back(MInstr::make_unary(
                        MOp::MOV, slot_mem(ra.slot_of(in.dst.vreg_id())), pdst));
                return;
            }

            const bool anti =
                (rs2.kind == MOperandKind::REG &&
                 pdst.kind == MOperandKind::REG && rs2.reg == pdst.reg);
            /* IMUL (emit_imul) SOLO soporta reg,reg y reg,reg,imm -- NO
             * acepta un operando fuente en memoria (a diferencia de
             * ADD/SUB/AND/OR/XOR, cuyo emit_alu si maneja r/m).  Si el
             * operando fuente del imul de 2 operandos esta spilled
             * (MEM), cargarlo a scr1 primero.  Sin esto, un imul con un
             * src spilled (p.ej. una constante loop-invariante que el
             * regalloc spilleo bajo presion) caia al fallback 0xCC del
             * encoder -> SIGTRAP en runtime. */
            auto imul_fix = [&](MOperand src) -> MOperand {
                if (op == MOp::IMUL && src.kind == MOperandKind::MEM) {
                    out.push_back(MInstr::make_unary(MOp::MOV, reg(scr1), src));
                    return reg(scr1);
                }
                return src;
            };
            /* P3 lea-3op: `dst = src1 + src2` con dst, src1, src2 en
             * REGISTROS DISTINTOS (3-address puro) -> `lea dst, [src1+src2]`
             * en UNA instr (vs `mov dst,src1; add dst,src2`) y SIN tocar
             * flags (util justo antes de un consumidor de flags).  Solo ADD
             * (lea suma, no resta).  Se descarta si:
             *   - !anti garantiza pdst != rs2 (el otro sumando).
             *   - pdst != rs1 (si coalescieron, ya es `add dst,src2` 1-instr).
             *   - alguno de los sumandos es RSP/RBP: RSP no puede ser index en
             *     SIB y RBP-base fuerza disp32; ademas esos NUNCA son valores
             *     computados (frame reservado) -> descartar sin perder nada. */
            if (op == MOp::ADD && !anti && pdst.kind == MOperandKind::REG &&
                rs1.kind == MOperandKind::REG &&
                rs2.kind == MOperandKind::REG && pdst.reg != rs1.reg &&
                rs1.reg != static_cast<uint8_t>(MReg::RSP) &&
                rs1.reg != static_cast<uint8_t>(MReg::RBP) &&
                rs2.reg != static_cast<uint8_t>(MReg::RSP) &&
                rs2.reg != static_cast<uint8_t>(MReg::RBP)) {
                out.push_back(MInstr::make_unary(
                    MOp::LEA, pdst,
                    MOperand::make_mem(static_cast<MReg>(rs1.reg), 0,
                                       static_cast<MReg>(rs2.reg), 1)));
                if (dst_spilled)
                    out.push_back(MInstr::make_unary(
                        MOp::MOV, slot_mem(ra.slot_of(in.dst.vreg_id())), pdst));
                return;
            }
            if (anti) {
                if (is_commutative(op)) {
                    /* pdst ya contiene src2 -> OP pdst, src1 (conmutativo). */
                    out.push_back(MInstr::make_unary(op, pdst, imul_fix(rs1)));
                } else {
                    /* SUB y pdst==src2reg: usar scratch1 para src2. */
                    out.push_back(MInstr::make_unary(MOp::MOV, reg(scr1), rs2));
                    out.push_back(MInstr::make_unary(MOp::MOV, pdst, rs1));
                    out.push_back(MInstr::make_unary(op, pdst, reg(scr1)));
                }
            } else {
                /* Elide el `mov pdst, src1` identidad cuando el regalloc ya
                 * coalescio dst y src1 al mismo fisico (mismo criterio que el
                 * path de shifts).  Quita un mov redundante por cada ALU
                 * 2-address coalescida. */
                if (!(pdst.kind == MOperandKind::REG &&
                      rs1.kind == MOperandKind::REG && pdst.reg == rs1.reg))
                    out.push_back(
                        MInstr::make_unary(MOp::MOV, pdst, rs1)); // pdst = src1
                out.push_back(
                    MInstr::make_unary(op, pdst,
                                       imul_fix(rs2))); // pdst OP= src2
            }
            if (dst_spilled) {
                out.push_back(MInstr::make_unary(
                    MOp::MOV, slot_mem(ra.slot_of(in.dst.vreg_id())), pdst));
            }
            return;
        }

        if (op == MOp::CMP || op == MOp::TEST) {
            MOperand a = resolve(in.src1);
            MOperand b = resolve(in.src2);
            if (a.kind == MOperandKind::MEM && b.kind == MOperandKind::MEM) {
                out.push_back(MInstr::make_unary(MOp::MOV, reg(scr0), a));
                a = reg(scr0);
            }
            out.push_back(MInstr::make_unary(op, a, b)); // dst=a, src1=b
            return;
        }

        /* Resto (JMP/JCC/LABEL_DEF/NOP/...): sustituir vregs en sitio. */
        MInstr m = in;
        m.dst = resolve(in.dst);
        m.src1 = resolve(in.src1);
        m.src2 = resolve(in.src2);
        out.push_back(m);
    }
};

} // namespace

MFunction rewrite_to_physical(const MFunction &vf, const RegAlloc &ra,
                              const TargetRegInfo &tri, AbiKind abi,
                              const IntervalResult *ivs, OsrEmit *osr) {
    /* Detectar si la funcion tiene CALLs (para reservar shadow space). */
    bool has_calls = false;
    uint32_t alloca_total = 0;  // commit 8: bytes de allocas en el frame
    bool has_vm_alloca = false; //   reserva en el VM stack del proceso
    for (const auto &b : vf.blocks) {
        for (const auto &in : b.instrs) {
            if (in.op == MOp::CALL || in.op == MOp::CALL_ABS) has_calls = true;
            /* CALL_SYM (AOT): CALL rel32 a una funcion del modulo -> frame
             * con shadow space (Win64) + rsp 16-alineado, igual que CALL. */
            if (in.op == MOp::CALL_SYM) has_calls = true;
            /* LOAD_VM/STORE_VM: el page-miss emite un CALL a vrt_vm_*; aunque
             * el hit no llama, debe reservarse el frame + shadow space Win64
             * (no frameless). */
            if (in.op == MOp::LOAD_VM || in.op == MOp::STORE_VM)
                has_calls = true;
            /* TAILCALL: su expansion hace emit_epilogue (necesita el frame
             * montado: lea rsp,[rbp-...] + pops) -> forzar frame (no
             * frameless), igual que un call normal. */
            if (in.op == MOp::TAILCALL) has_calls = true;
            if (in.op == MOp::ALLOCA) {
                const uint32_t sz = static_cast<uint32_t>(in.src1.value);
                alloca_total += (sz + 7u) & ~7u;
            }
            if (in.op == MOp::ALLOCA_VM) has_vm_alloca = true;
        }
    }
    /* Args ilimitados (JIT/AOT): max nº de GP args por PILA en cualquier
     * call (overflow de arg_regs).  Reset por call group; toma el maximo
     * para dimensionar el outgoing area del frame una sola vez. */
    uint32_t max_stack_args = 0;
    {
        const size_t gareg_n =
            tri.arg_regs[static_cast<size_t>(RegClass::GP)].size();
        const size_t fareg_n =
            tri.arg_regs[static_cast<size_t>(RegClass::FP)].size();
        uint32_t gp_in_call = 0, fp_in_call = 0;
        for (const auto &b : vf.blocks) {
            for (const auto &in : b.instrs) {
                if (in.op == MOp::ARG) {
                    if (Lowerer::is_fp_operand(in.src1))
                        ++fp_in_call;
                    else
                        ++gp_in_call;
                } else if (in.op == MOp::CALL || in.op == MOp::CALL_ABS ||
                           in.op == MOp::CALL_SYM) {
                    /* Stack-args = overflow GP + overflow FP (van DESPUES de
                     * los GP-stack -> el total dimensiona el outgoing area). */
                    uint32_t s = 0;
                    if (gp_in_call > gareg_n)
                        s += gp_in_call - static_cast<uint32_t>(gareg_n);
                    if (fp_in_call > fareg_n)
                        s += fp_in_call - static_cast<uint32_t>(fareg_n);
                    if (s > max_stack_args) max_stack_args = s;
                    gp_in_call = 0;
                    fp_in_call = 0;
                }
            }
        }
    }
    /* Incoming stack-params (callee): si hay mas params que arg_regs GP, los de
     * overflow llegan en la pila ([rbp+off]) -> hay que FORZAR el frame pointer
     * (no_frame=false) para que rbp sea estable.  Conservador (cuenta total de
     * params): un falso positivo solo reserva un frame de mas, nunca corrompe. */
    const bool has_stack_params =
        vf.param_vregs.size() >
        tri.arg_regs[static_cast<size_t>(RegClass::GP)].size();
    Lowerer lw(ra, tri, abi, has_calls, alloca_total, has_vm_alloca,
               max_stack_args, has_stack_params, vf.cb_save_regs);
    lw.naked = vf.naked; //  NR @Naked: sin prologo/epilogo/ret
    lw.ivs = ivs; // commit 6: para construir stackmaps en CALLs
    MFunction pf;
    lw.pf = &pf; // labels intra-expansion (LOAD_VM/STORE_VM page-cache)
    pf.name = vf.name;
    /* Solo-LSP (vista "Godbolt"): propagar el opt-in de la tabla
     * byte_offset -> source_line al MFunction fisico (que es el que ve el
     * encoder).  OFF por defecto -> sin efecto en produccion. */
    pf.emit_line_map = vf.emit_line_map;
    pf.next_label_id = vf.next_label_id;
    pf.label_offsets = vf.label_offsets;
    pf.imm64_pool = vf.imm64_pool;
    /* AOT: la tabla de simbolos de las relocations viaja del MFunction
     * vreg al fisico; el encoder (que corre sobre @c pf) appendea las
     * @c MReloc referenciando estos indices. */
    pf.reloc_symbols = vf.reloc_symbols;
    /*  AS inc.5: los bytes del inline-asm los consume el ENCODER, que
     * corre sobre @c pf (la funcion reescrita) -> hay que arrastrarlos.
     * (@c vreg_fixed NO se copia: lo consume @c build_intervals, que corre
     * sobre @c vf ANTES del rewrite.) */
    pf.asm_blobs = vf.asm_blobs;
    /* ENSAMBLADO DIFERIDO.  Los blobs de un `asm ( reg x )` con
     * operandos AUTO llegan sin bytes: su plantilla lleva $N y el fisico de
     * cada operando lo acaba de elegir el asignador.  Sustituimos $N por el
     * registro real (@c ra.reg_of del vreg, o el pin @c fixed_phys) y llamamos
     * al ensamblador.  Asi el operando `reg` se integra con el regalloc de la
     * funcion (registro OPTIMO) en vez del pick greedy compile-time. */
    if (vx::g_asm_backend != nullptr) {
        for (AsmBlob &b : pf.asm_blobs) {
            if (!b.deferred) continue;
            std::string nasm;
            bool ok = true;
            const std::string &t = b.deferred_tmpl;
            for (size_t i = 0; i < t.size();) {
                if (t[i] != '$') { nasm += t[i++]; continue; }
                size_t j = i + 1;
                uint32_t idx = 0;
                bool any = false;
                while (j < t.size() &&
                       std::isdigit(static_cast<unsigned char>(t[j]))) {
                    idx = idx * 10 + static_cast<uint32_t>(t[j] - '0');
                    ++j;
                    any = true;
                }
                if (!any || idx >= b.deferred_ops.size()) {
                    nasm += t[i++]; // '$' literal / fuera de rango -> verbatim
                    continue;
                }
                const AsmBlob::DeferredOp &d = b.deferred_ops[idx];
                int phys = d.fixed_phys >= 0
                               ? d.fixed_phys
                               : static_cast<int>(ra.reg_of(d.vreg));
                std::string nm = vx::asm_phys_reg_name(b.deferred_isa,
                                                       d.regclass, phys, d.width);
                if (nm.empty()) { ok = false; break; }
                nasm += nm;
                i = j;
            }
            if (ok) {
                vx::AsmAssembleResult ar =
                    vx::g_asm_backend->assemble(nasm, vx::AsmArch::X86_64);
                if (ar.ok && !ar.bytes.empty()) b.bytes = std::move(ar.bytes);
            }
            b.deferred = false; // ensamblado (o fallo -> bytes vacio, no emite)
        }
    }
    pf.blocks.resize(vf.blocks.size());

    /* OSR 1a: deteccion PRECISA de loop back-edges via DFS (clasico:
     * arista u->v es back-edge sii v esta "gris" = en la pila del DFS).
     * Distingue ciclos reales de saltos a un indice menor que NO son loops
     * (e.g. el merge de un if/else anidado).  @c be_block[u]=1 si el bloque
     * u termina en un BR INCONDICIONAL (succ_b invalido) que es back-edge.
     * Iterativo para no desbordar la pila en funciones con muchos bloques. */
    const size_t NB = vf.blocks.size();
    std::vector<uint8_t> be_block; // vacio salvo con el gate (cero overhead)
    if (jit_osr_count() && NB > 0) {
        be_block.assign(NB, 0);
        std::vector<uint8_t> color(NB, 0);         // 0=white 1=gray 2=black
        std::vector<std::pair<MBlockId, int>> stk; // (bloque, prox succ)
        stk.push_back({0, 0});
        color[0] = 1;
        while (!stk.empty()) {
            MBlockId u = stk.back().first;
            int &si = stk.back().second;
            const MBlockId succ[2] = {vf.blocks[u].succ_a, vf.blocks[u].succ_b};
            if (si < 2) {
                const MBlockId v = succ[si];
                ++si;
                if (v == MBLOCK_INVALID || v >= NB) continue;
                if (color[v] == 0) {
                    color[v] = 1;
                    stk.push_back({v, 0});
                } else if (color[v] == 1) {
                    /* back-edge u->v; instrumentable solo si u es BR
                     * incondicional (sin succ_b) y v es ese succ_a. */
                    if (vf.blocks[u].succ_b == MBLOCK_INVALID &&
                        vf.blocks[u].succ_a == v)
                        be_block[u] = 1;
                }
            } else {
                color[u] = 2;
                stk.pop_back();
            }
        }
    }

    /* OSR 2a: indice global de la primera instr de cada bloque (MISMA
     * numeracion que build_intervals).  @c block_start[h] = 2*first_gi[h]
     * es la posicion del header; @c covers(block_start[h]) == el live-in
     * del header (los valores que C2 debe reanudar).  Solo bajo el gate. */
    std::vector<uint32_t> first_gi;
    if ((jit_osr_count() || osr != nullptr) && NB > 0) {
        first_gi.assign(NB, 0);
        uint32_t g = 0;
        for (size_t bb = 0; bb < NB; ++bb) {
            first_gi[bb] = g;
            g += static_cast<uint32_t>(vf.blocks[bb].instrs.size());
        }
    }

    /* gi = indice lineal global de la instr vreg (MISMO orden que
     * build_intervals).  cur_call_pos = 2*gi se pasa a lower() para que
     * el CALL_ABS sepa que GC roots estan vivos en ese punto. */
    uint32_t gi = 0;
    for (size_t b = 0; b < vf.blocks.size(); ++b) {
        pf.blocks[b].label_id = vf.blocks[b].label_id;
        pf.blocks[b].succ_a = vf.blocks[b].succ_a;
        pf.blocks[b].succ_b = vf.blocks[b].succ_b;
        pf.blocks[b].extra_succs = vf.blocks[b].extra_succs;
        std::vector<MInstr> outv;
        outv.reserve(vf.blocks[b].instrs.size() * 2 + 8);
        if (b == 0) lw.emit_prologue(outv);
        /* HOST_LEAF: cargar los params desde los arg_regs (parallel-move).
         * Consume las MOV param-init lideres del bloque 0; el lowering
         * normal las salta (pero gi avanza para no desincronizar las
         * posiciones de los stackmaps). */
        size_t param_skip = (b == 0)
                                ? lw.emit_host_param_loads(vf.blocks[0].instrs,
                                                           vf.param_vregs, outv)
                                : 0;
        size_t ii = 0;
        /* Batch de copias de PHI (variant==0xFD): una MISMA arista es un
         * PARALLEL MOVE.  Se acumulan y se resuelven juntas con
         * lower_phi_parallel (rompe ciclos con scratch); si se bajaran una a
         * una via lw.lower(), la emision secuencial corromperia un ciclo de
         * permutacion creado por el regalloc (post ssa_coalesce). */
        std::vector<const MInstr *> pending_phi;
        auto flush_phi = [&]() {
            if (pending_phi.empty()) return;
            lw.lower_phi_parallel(pending_phi, outv);
            pending_phi.clear();
        };
        for (const MInstr &in : vf.blocks[b].instrs) {
            if (ii < param_skip) {
                ++ii;
                ++gi;
                continue;
            }
            ++ii;
            if (in.op == MOp::MOV && in.variant == 0xFD) {
                pending_phi.push_back(&in); // acumular; resolver en batch
                ++gi;
                continue;
            }
            flush_phi(); // resolver las copias de PHI antes del terminador
            lw.cur_call_pos = 2u * gi;
            /* Solo-LSP: las MInstr fisicas que emite lower() se construyen
             * frescas (make_unary/...), perdiendo el source_pc de @c in.
             * Lo re-estampamos en las instrs anadidas por esta op para que
             * el encoder produzca la tabla linea<->asm.  OFF en produccion. */
            const size_t lm_before = pf.emit_line_map ? outv.size() : 0;
            lw.lower(in, outv);
            if (pf.emit_line_map) {
                for (size_t k = lm_before; k < outv.size(); ++k) {
                    if (in.source_pc != 0 && outv[k].source_pc == 0)
                        outv[k].source_pc = in.source_pc;
                    /* Tambien la identidad de la op IR (correlacion exacta). */
                    if (outv[k].ir_id == 0xFFFFFFFFu) outv[k].ir_id = in.ir_id;
                }
            }
            ++gi;
        }
        flush_phi(); // por si el bloque termina en copias de PHI
        /* OSR 1a: instrumentar el back-edge (BR incondicional a un bloque
         * anterior).  El counter va ANTES del JMP terminal; el `add` toca
         * flags pero el JMP no las lee.  push/pop rax preserva el estado.
         * Emitido aqui (post-lower) para no pasar por la legalizacion
         * 2-address que mangla ADD [mem],imm. */
        if (osr == nullptr && jit_osr_count() && be_block[b] && !outv.empty() &&
            outv.back().op == MOp::JMP) {
            osr_install_dump_once();
            const uint64_t loop_id = g_osr_be_sites;
            ++g_osr_be_sites;

            /* --- OSR 2a: descriptor del estado vivo del loop header ---
             * Capturamos el live-in del header = covers(2*first_gi[header])
             * = phi_dst (loop-carried) + invariantes usados en el loop
             * (excluye phi_args y temporales del header).  Cada valor se
             * escribira en osr_buffer[vid].  Restringido a IR values reales
             * (vid < ir_value_count): esos ids son ESTABLES entre C1 y C2
             * (el clon C2 los preserva), garantizando que C1 escribe y C2
             * lee la MISMA celda.  Si algun valor no es capturable (temp del
             * selector, fuera del buffer, o sin ubicacion fisica), ABORTA la
             * captura de este loop (el trigger sigue logueando "ABORTADO"). */
            const MBlockId header = vf.blocks[b].succ_a;
            OsrLoopDesc desc;
            desc.fn_name = vf.name;
            desc.header_block = header;
            desc.aborted = false;
            if (ivs != nullptr && header != MBLOCK_INVALID &&
                static_cast<size_t>(header) < NB && !first_gi.empty()) {
                const uint32_t header_pos = 2u * first_gi[header];
                const uint32_t NVI =
                    static_cast<uint32_t>(ivs->intervals.size());
                const uint32_t ir_n = vf.ir_value_count;
                for (uint32_t v = 0; v < NVI; ++v) {
                    const LiveInterval &lv = ivs->intervals[v];
                    if (!lv.covers(header_pos)) continue;
                    if (v >= ir_n || v >= VESTA_OSR_BUFFER_N ||
                        (!ra.in_reg(v) && !ra.spilled(v))) {
                        desc.aborted = true;
                        desc.captures.clear();
                        break;
                    }
                    desc.captures.push_back(
                        {v, static_cast<uint8_t>(lv.is_gc() ? 1u : 0u)});
                }
            } else {
                desc.aborted = true;
            }
            const bool do_capture = !desc.aborted;
            /* Copia para el codegen (el descriptor se mueve al registro). */
            const std::vector<OsrCaptureSlot> caps = desc.captures;
            if (g_osr_loops.size() <= static_cast<size_t>(loop_id))
                g_osr_loops.resize(static_cast<size_t>(loop_id) + 1u);
            g_osr_loops[static_cast<size_t>(loop_id)] = std::move(desc);

            /* Indices del pool: contador, loop_id, handler. */
            const uint32_t idx_cnt =
                static_cast<uint32_t>(pf.imm64_pool.size());
            pf.imm64_pool.push_back(
                reinterpret_cast<uint64_t>(&g_osr_be_total));
            const uint32_t idx_lid =
                static_cast<uint32_t>(pf.imm64_pool.size());
            pf.imm64_pool.push_back(loop_id);
            const uint32_t idx_h = static_cast<uint32_t>(pf.imm64_pool.size());
            pf.imm64_pool.push_back(
                reinterpret_cast<uint64_t>(&osr_trigger_stub));
            const MLabelId lskip = pf.new_label();
            auto R = [](MReg r) { return MOperand::make_reg(r, 8); };
            std::vector<MInstr> seq;
            /* --- contador + check (cada iteracion; jne bien predicho) --- */
            seq.push_back(MInstr::make_unary(MOp::PUSH, {}, R(MReg::RAX)));
            seq.push_back(MInstr::make_unary(
                MOp::MOV, R(MReg::RAX), MOperand::make_imm64_idx(idx_cnt)));
            seq.push_back(MInstr::make_unary(MOp::ADD,
                                             MOperand::make_mem(MReg::RAX, 0),
                                             MOperand::make_imm32(1)));
            seq.push_back(
                MInstr::make_unary(MOp::CMP, MOperand::make_mem(MReg::RAX, 0),
                                   MOperand::make_imm32(static_cast<int32_t>(
                                       jit_osr_threshold()))));
            seq.push_back(MInstr::make_unary(MOp::POP, R(MReg::RAX), {}));
            seq.push_back(MInstr::make_jcc(
                MCond::NE, lskip)); // cmp no toca rsp; pop no toca flags
            /* --- trigger one-shot (cnt == umbral): preservar TODO el estado
             * vivo (caller-saved), llamar al handler, restaurar.  RBX
             * (=ProcessVM*) es callee-saved -> sobrevive el call. --- */
            const auto &cs =
                tri.caller_saved[static_cast<size_t>(RegClass::GP)];
            for (uint8_t r : cs)
                seq.push_back(
                    MInstr::make_unary(MOp::PUSH, {}, R(static_cast<MReg>(r))));
#if defined(_WIN32)
            const MReg A0 = MReg::RCX, A1 = MReg::RDX, A2 = MReg::R8;
            const uint32_t SHADOW = 32;
#else
            const MReg A0 = MReg::RDI, A1 = MReg::RSI, A2 = MReg::RDX;
            const uint32_t SHADOW = 0;
#endif
            /* --- OSR 2a captura: escribir cada vid vivo en osr_buffer[vid].
             * RAX = base del buffer (proc->osr_buffer leido via RBX); RCX =
             * temp.  Ambos son caller-saved (ya salvados en la pila arriba)
             * y se restauran en el pop final.  Las fuentes caller-saved se
             * leen de su COPIA en la pila (offset = 8*(n-1-idx), nunca del
             * reg vivo, que podemos haber clobreado); las callee-saved del
             * reg directo (intactas); las derramadas del slot [rbp+off]. */
            if (do_capture) {
                const size_t n = cs.size();
                seq.push_back(MInstr::make_unary(
                    MOp::MOV, R(MReg::RAX),
                    MOperand::make_mem(MReg::RBX,
                                       VESTA_PROC_OSR_BUFFER_OFFSET)));
                for (const OsrCaptureSlot &c : caps) {
                    const MOperand dstmem = MOperand::make_mem(
                        MReg::RAX, static_cast<int32_t>(c.vid * 8u));
                    if (ra.in_reg(c.vid)) {
                        const uint8_t rid = ra.reg_of(c.vid);
                        int idx = -1;
                        for (size_t i = 0; i < n; ++i)
                            if (cs[i] == rid) {
                                idx = static_cast<int>(i);
                                break;
                            }
                        if (idx >= 0) {
                            /* caller-saved: leer de la copia en la pila. */
                            const int32_t off = static_cast<int32_t>(
                                8u * (n - 1u - static_cast<size_t>(idx)));
                            seq.push_back(MInstr::make_unary(
                                MOp::MOV, R(MReg::RCX),
                                MOperand::make_mem(MReg::RSP, off)));
                            seq.push_back(MInstr::make_unary(MOp::MOV, dstmem,
                                                             R(MReg::RCX)));
                        } else {
                            /* callee-saved: valor vivo en el reg directo. */
                            seq.push_back(MInstr::make_unary(
                                MOp::MOV, dstmem, R(static_cast<MReg>(rid))));
                        }
                    } else { /* spilled: leer del slot rbp-relativo. */
                        seq.push_back(MInstr::make_unary(
                            MOp::MOV, R(MReg::RCX),
                            MOperand::make_mem(
                                MReg::RBP, lw.slot_off(ra.slot_of(c.vid)))));
                        seq.push_back(
                            MInstr::make_unary(MOp::MOV, dstmem, R(MReg::RCX)));
                    }
                }
                /* arg2 = base del buffer (sigue en RAX tras la captura). */
                seq.push_back(
                    MInstr::make_unary(MOp::MOV, R(A2), R(MReg::RAX)));
            } else {
                /* sin captura: arg2 = 0 (el handler no lee el buffer). */
                seq.push_back(MInstr::make_unary(MOp::MOV, R(A2),
                                                 MOperand::make_imm32(0)));
            }
            seq.push_back(
                MInstr::make_unary(MOp::MOV, R(A0), R(MReg::RBX))); // arg0=proc
            seq.push_back(MInstr::make_unary(
                MOp::MOV, R(A1),
                MOperand::make_imm64_idx(idx_lid))); // arg1=loop_id
            /* Alinear rsp a 16 + shadow space (Win64).  Tras el frame rsp
             * esta 16-alineado; push de C caller-saved lo desalinea 8 si C
             * es impar. */
            const uint32_t adjust = ((cs.size() & 1u) ? 8u : 0u) + SHADOW;
            if (adjust)
                seq.push_back(MInstr::make_unary(
                    MOp::SUB, R(MReg::RSP),
                    MOperand::make_imm32(static_cast<int32_t>(adjust))));
            seq.push_back(MInstr::make_unary(MOp::MOV, R(MReg::RAX),
                                             MOperand::make_imm64_idx(idx_h)));
            {
                MInstr c;
                c.op = MOp::CALL;
                c.src1 = R(MReg::RAX);
                seq.push_back(c);
            }
            /* --- 2c FRAME-SWAP: el handler devolvio en RAX la direccion del
             * OSR-entry del C2 (o 0).  Si != 0, ABANDONAMOS el frame C1 y
             * saltamos al C2: emit_epilogue restaura el frame con
             * `lea rsp,[rbp-8*total_saved]` (ABSOLUTO desde rbp -> no hay que
             * deshacer los push del trigger) + pops, dejando rsp en la
             * direccion de retorno del CALLER de C1.  RAX (osr_entry) NO se
             * popea en el epilogue -> sobrevive.  `jmp rax` entra al C2 con
             * rsp en ret_addr: el RET del C2 retorna al caller de C1, como si
             * el C2 hubiera sido la funcion llamada.  El buffer-por-VID ya
             * tiene el estado (capturado arriba, antes del handler) y el
             * handler es lookup-puro (sin GC) -> host_ptr GC siguen frescos. */
            const MLabelId lcont = pf.new_label();
            seq.push_back(
                MInstr::make_unary(MOp::TEST, R(MReg::RAX), R(MReg::RAX)));
            seq.push_back(
                MInstr::make_jcc(MCond::E, lcont)); // RAX==0 -> no swap
            /* El OSR-entry del C2 es VM_ABI: su prologue hace
             * `mov rbx, <arg0>` esperando ProcessVM* en el registro del 1er
             * argumento (A0 = RCX win / RDI sysv).  El epilogue del C1
             * restaura RBX al valor del caller (pop rbx) -> hay que pasar
             * proc por A0 ANTES del epilogue.  A0 es caller-saved y NO esta
             * en callee_saved -> sobrevive el epilogue (igual que RAX). */
            seq.push_back(MInstr::make_unary(MOp::MOV, R(A0), R(MReg::RBX)));
            lw.emit_epilogue(seq); // rsp -> ret_addr; RAX + A0 intactos
            {
                MInstr j;
                j.op = MOp::JMP;
                j.src1 = R(MReg::RAX);
                seq.push_back(j);
            }
            seq.push_back(MInstr::make_label_def(lcont));
            /* --- ruta sin swap (RAX==0): continuar el loop C1 normal. --- */
            if (adjust)
                seq.push_back(MInstr::make_unary(
                    MOp::ADD, R(MReg::RSP),
                    MOperand::make_imm32(static_cast<int32_t>(adjust))));
            for (size_t i = cs.size(); i-- > 0;)
                seq.push_back(MInstr::make_unary(
                    MOp::POP, R(static_cast<MReg>(cs[i])), {}));
            seq.push_back(MInstr::make_label_def(lskip));
            outv.insert(outv.end() - 1, seq.begin(), seq.end());
        }
        pf.blocks[b].instrs = std::move(outv);
    }

    /* --- OSR 2b: APPEND del bloque OSR-entry (C2, el 2o punto de entrada
     * a mitad del loop).  Monta el frame normal, carga el live-in del
     * header desde proc->osr_buffer[vid] a la ubicacion fisica de cada vid,
     * y salta al MBlock del header.  R10/R11 (scratch, no asignables) como
     * base/temp -> nunca colisionan con la ubicacion fisica de un vid. */
    if (osr != nullptr && ivs != nullptr && !first_gi.empty() &&
        osr->header_block != MBLOCK_INVALID &&
        static_cast<size_t>(osr->header_block) < NB) {
        const MBlockId header = osr->header_block;
        const uint32_t header_pos = 2u * first_gi[header];
        const uint32_t NVI = static_cast<uint32_t>(ivs->intervals.size());
        const uint32_t ir_n = vf.ir_value_count;

        /* (0) Recolectar el live-in del header (covers(header_pos)) y, si hay
         * red de seguridad (required_captures), VERIFICAR que todo vid este
         * en el conjunto capturado por el C1.  Si alguno NO esta -> ABORTAR
         * (no emitir el OSR-entry) -> el C2 no se registra -> sin swap -> el
         * C1 continua.  Asi un C2 optimizado que necesite un valor no
         * capturado por el C1 degrada a "sin speedup", nunca a corrupcion. */
        std::vector<uint32_t> live_in;
        bool mismatch = false;
        for (uint32_t v = 0; v < NVI; ++v) {
            const LiveInterval &lv = ivs->intervals[v];
            if (!lv.covers(header_pos)) continue;
            if (v >= ir_n || v >= VESTA_OSR_BUFFER_N) continue;
            if (!ra.in_reg(v) && !ra.spilled(v)) continue; // sin ubicacion
            if (osr->required_captures != nullptr) {
                const auto &req = *osr->required_captures;
                bool found = false;
                for (uint32_t cv : req)
                    if (cv == v) {
                        found = true;
                        break;
                    }
                if (!found) {
                    mismatch = true;
                    break;
                }
            }
            live_in.push_back(v);
        }
        if (mismatch) {
            osr->osr_entry_valid = false; // fallback seguro: no swap
        } else {
            const MLabelId lbl = pf.new_label();
            auto R = [](MReg r) { return MOperand::make_reg(r, 8); };
            const MReg base = lw.scr0; // R10 (no asignable)
            const MReg tmp = lw.scr1;  // R11 (no asignable)
            MBlock entryb;
            entryb.label_id = lbl;
            entryb.succ_a = header; // metadata (el jmp usa el label)
            entryb.succ_b = MBLOCK_INVALID;
            std::vector<MInstr> ob;
            /* (a) Prologue IDENTICO al de la entry 0 (mismo frame). */
            lw.emit_prologue(ob);
            /* (b) base = proc->osr_buffer (via RBX = ProcessVM*). */
            ob.push_back(MInstr::make_unary(
                MOp::MOV, R(base),
                MOperand::make_mem(MReg::RBX, VESTA_PROC_OSR_BUFFER_OFFSET)));
            /* (c) cargar cada live-in vid del buffer a su reg/slot. */
            for (uint32_t v : live_in) {
                const MOperand srcmem =
                    MOperand::make_mem(base, static_cast<int32_t>(v * 8u));
                if (ra.in_reg(v)) {
                    ob.push_back(MInstr::make_unary(
                        MOp::MOV,
                        MOperand::make_reg(static_cast<MReg>(ra.reg_of(v)), 8),
                        srcmem));
                } else { /* spilled (garantizado por el filtro de arriba). */
                    ob.push_back(MInstr::make_unary(MOp::MOV, R(tmp), srcmem));
                    ob.push_back(MInstr::make_unary(
                        MOp::MOV,
                        MOperand::make_mem(MReg::RBP,
                                           lw.slot_off(ra.slot_of(v))),
                        R(tmp)));
                }
            }
            /* (d) saltar al header (reanuda el loop a mitad). */
            ob.push_back(MInstr::make_jmp(pf.blocks[header].label_id));
            entryb.instrs = std::move(ob);
            pf.blocks.push_back(std::move(entryb));
            osr->osr_entry_label = lbl;
            osr->osr_entry_valid = true;
        }
    }

    /* --- Peephole final: eliminar `mov r64, r64` con el MISMO registro
     * (no-ops puros del two-address legalization: `mov pdst, rs1` cuando
     * pdst==rs1).  En un loop apretado (p.ej. un C2 con inline agresivo)
     * estos consumen ancho de banda de DECODE aunque el renamer los elimine.
     * SOLO width 8: un `mov e_x, e_x` de 32 bits zero-extiende los 64 bits
     * (NO es no-op) y se conserva.  Cero riesgo: un self-mov de 64 bits es
     * semanticamente identico a su ausencia. */
    for (auto &blk : pf.blocks) {
        std::vector<MInstr> cleaned;
        cleaned.reserve(blk.instrs.size());
        for (MInstr &mi : blk.instrs) {
            if (mi.op == MOp::MOV && mi.dst.kind == MOperandKind::REG &&
                mi.src1.kind == MOperandKind::REG &&
                mi.dst.reg == mi.src1.reg && mi.dst.width == 8 &&
                mi.src1.width == 8) {
                continue; // self-mov de 64 bits -> descartar
            }
            cleaned.push_back(std::move(mi));
        }
        blk.instrs = std::move(cleaned);
    }

    pf.stackmaps = std::move(lw.stackmaps); // commit 6
    return pf;
}

/* ===================================================================== */
/* OSR runtime glue ( D.8, 2c) -- definiciones publicas              */
/* ===================================================================== */

void set_osr_handler(uint64_t (*handler)(uint64_t)) {
    g_osr_handler = handler;
}

uint32_t osr_loop_count() {
    return g_osr_be_sites;
}

bool osr_loop_info(uint64_t loop_id, std::string &fn_name_out,
                   uint32_t &header_block_out) {
    if (loop_id >= g_osr_loops.size()) return false;
    const OsrLoopDesc &d = g_osr_loops[static_cast<size_t>(loop_id)];
    if (d.aborted) return false;
    fn_name_out = d.fn_name;
    header_block_out = d.header_block;
    return true;
}

bool osr_loop_captures(uint64_t loop_id, std::vector<uint32_t> &out_vids) {
    if (loop_id >= g_osr_loops.size()) return false;
    const OsrLoopDesc &d = g_osr_loops[static_cast<size_t>(loop_id)];
    if (d.aborted) return false;
    out_vids.clear();
    out_vids.reserve(d.captures.size());
    for (const OsrCaptureSlot &c : d.captures)
        out_vids.push_back(c.vid);
    return true;
}

} // namespace jit
