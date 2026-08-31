/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/jit/test_phys_liveness.cpp
 * @brief Que registros estan vivos en cada punto del MachineIR ya repartido.
 *
 * Lo que se comprueba no es el algoritmo -- gen/kill y punto fijo son de
 * manual -- sino que la respuesta incluya lo que NO se ve mirando los
 * operandos, que es donde se equivoca quien lo deduce por su cuenta:
 *
 *   - lo que una instruccion lee IMPLICITAMENTE (el RDX:RAX de una division);
 *   - las bases de una direccion, que se leen aunque el operando sea destino;
 *   - los registros de un bloque de `asm volatile`;
 *   - lo que cruza de un bloque a otro.
 *
 * Cada caso construye la funcion a mano, pregunta, y compara contra lo que
 * tiene que salir.  Sin la suite completa detras: este fichero se compila y se
 * ejecuta solo.
 */

#include "analysis/facts/phys_liveness.h"

#include <cstdio>

using namespace jit;
using analysis::facts::compute_phys_liveness;
using analysis::facts::PhysLivenessFacts;

namespace {

int fallos = 0;

/// @brief Comprueba que @p r este (o no) vivo tras la instruccion @p i de @p b.
void espera(const PhysLivenessFacts &F, uint32_t b, uint32_t i, MReg r,
            bool vivo, const char *que) {
    const bool real = F.is_live_after(b, i, static_cast<uint8_t>(r));
    if (real == vivo) {
        std::printf("  ok    %s\n", que);
        return;
    }
    std::printf("  FALLO %s: salio %s, se esperaba %s\n", que,
                real ? "vivo" : "muerto", vivo ? "vivo" : "muerto");
    ++fallos;
}

MOperand reg(MReg r, uint8_t w = 8) { return MOperand::make_reg(r, w); }
MOperand imm(int32_t v) { return MOperand::make_imm32(v); }

/// @brief Una funcion de un solo bloque con las instrucciones dadas.
MFunction una(std::vector<MInstr> is) {
    MFunction f;
    f.blocks.emplace_back();
    f.blocks[0].instrs = std::move(is);
    return f;
}

/* --- 1.  Lo basico: escribir y no leer. --------------------------------- */
void caso_pisado() {
    std::printf("pisar un registro sin leerlo\n");
    // mov rax, 1 ; mov rax, 2 ; mov rcx, rax
    MFunction f = una({
        MInstr::make_unary(MOp::MOV, reg(MReg::RAX), imm(1)),
        MInstr::make_unary(MOp::MOV, reg(MReg::RAX), imm(2)),
        MInstr::make_unary(MOp::MOV, reg(MReg::RCX), reg(MReg::RAX)),
    });
    const PhysLivenessFacts F = compute_phys_liveness(f);
    // Tras la PRIMERA, rax no esta vivo: la segunda lo pisa entero sin leerlo.
    espera(F, 0, 0, MReg::RAX, false, "tras el primer mov, rax muerto");
    // Tras la SEGUNDA si: la tercera lo lee.
    espera(F, 0, 1, MReg::RAX, true, "tras el segundo mov, rax vivo");
}

/* --- 2.  Lecturas IMPLICITAS: la division. ------------------------------ */
void caso_division() {
    std::printf("una division lee RDX:RAX sin nombrarlos\n");
    // mov rdx, 0 ; idiv rsi
    MFunction f = una({
        MInstr::make_unary(MOp::MOV, reg(MReg::RDX), imm(0)),
        MInstr::make_unary(MOp::IDIV, reg(MReg::RSI), reg(MReg::RSI)),
    });
    const PhysLivenessFacts F = compute_phys_liveness(f);
    /* Es EL caso que rompe a quien mira solo los operandos: `idiv rsi` no
     * nombra rdx en ninguna parte, asi que el `mov rdx, 0` de delante parece
     * una escritura muerta.  Borrarlo cambia el resultado de la division. */
    espera(F, 0, 0, MReg::RDX, true, "tras poner rdx a 0, rdx sigue vivo");
}

/* --- 3.  Una direccion LEE su base. ------------------------------------- */
void caso_direccion() {
    std::printf("escribir en [rax] no escribe rax: lo lee\n");
    // mov rax, 8 ; mov [rax], rcx
    MFunction f = una({
        MInstr::make_unary(MOp::MOV, reg(MReg::RAX), imm(8)),
        MInstr::make_unary(MOp::MOV, MOperand::make_mem(MReg::RAX, 0),
                           reg(MReg::RCX)),
    });
    const PhysLivenessFacts F = compute_phys_liveness(f);
    espera(F, 0, 0, MReg::RAX, true, "rax vivo: es la base de la direccion");
}

/* --- 4.  Lo que cruza de un bloque a otro. ------------------------------ */
void caso_entre_bloques() {
    std::printf("un valor que se usa en el bloque siguiente\n");
    MFunction f;
    f.blocks.emplace_back();
    f.blocks.emplace_back();
    f.blocks[0].instrs = {MInstr::make_unary(MOp::MOV, reg(MReg::RBX), imm(7))};
    f.blocks[0].succ_a = 1;
    f.blocks[1].instrs = {
        MInstr::make_unary(MOp::MOV, reg(MReg::RCX), reg(MReg::RBX))};
    const PhysLivenessFacts F = compute_phys_liveness(f);
    espera(F, 0, 0, MReg::RBX, true, "rbx vivo al salir del primer bloque");
    espera(F, 1, 0, MReg::RBX, false, "y muerto tras su ultima lectura");
}

/* --- 5.  Un bloque de `asm volatile` dice lo que lee. -------------------- */
void caso_asm() {
    std::printf("un asm volatile lee registros que no estan en los operandos\n");
    MFunction f;
    f.blocks.emplace_back();
    AsmBlob b;
    b.bytes = {0x90};                              // nop: da igual lo que sea
    b.in_phys = {static_cast<uint8_t>(MReg::RSI)}; // lee rsi
    b.out_phys = {static_cast<uint8_t>(MReg::RAX)};
    b.clobbers_conocidos = true;
    f.asm_blobs.push_back(std::move(b));
    MInstr a = MInstr::make_unary(MOp::INLINE_ASM_RAW, MOperand(), imm(0));
    f.blocks[0].instrs = {
        MInstr::make_unary(MOp::MOV, reg(MReg::RSI), imm(3)),
        a,
    };
    const PhysLivenessFacts F = compute_phys_liveness(f);
    /* Sin mirar el bloque, ese `mov rsi, 3` parece muerto: el asm no nombra
     * rsi en ningun operando.  Es el fallo que se vio de verdad. */
    espera(F, 0, 0, MReg::RSI, true, "rsi vivo: lo lee el asm");
}

} // namespace

int main() {
    std::printf("== liveness por registro fisico ==\n");
    caso_pisado();
    caso_division();
    caso_direccion();
    caso_entre_bloques();
    caso_asm();
    if (fallos == 0) {
        std::printf("\nTODO OK\n");
        return 0;
    }
    std::printf("\n%d FALLO(S)\n", fallos);
    return 1;
}
