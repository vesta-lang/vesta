/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/codegen/test_vm_target.cpp
 * @brief La ISA de la PROPIA VM descrita como un target mas (@c target_vm).
 *
 * Primer paso de unificar los TRES modos (interprete, JIT, AOT) bajo el mismo
 * allocator: los TRES modos usan @c codegen::rbank; el emisor `.vel` entra por
 * @c codegen::rbank -- dos asignadores para el mismo problema.
 *
 * Lo que se comprueba aqui NO es que "compile", sino que el descriptor
 * DESCRIBE LA MISMA MAQUINA que el emisor asume hoy: que el banco construido a
 * partir de el coincide, registro a registro, con las constantes que el emisor
 * lleva codificadas (@c ALLOC_REGS, @c SCRATCH_REG, @c SCRATCH2_REG,
 * @c ARGC_REG).  Si alguien cambia una de las dos partes sin la otra, este test
 * lo dice -- que es justo el fallo que hizo falsa la documentacion de
 * regalloc.h durante meses.
 */

#include "codegen/rbank/physical_bank.h"
#include "codegen/vm_target.h"
#include "ir/regalloc.h"
#include "jit/backend_caps.h"
#include "jit/target_reginfo.h"

#include <algorithm>
#include <cstdio>

using namespace codegen::rbank;

static int g_checks = 0, g_fails = 0;
#define CHECK(c)                                                                 \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(c)) {                                                              \
            ++g_fails;                                                           \
            std::printf("  FALLO L%d: %s\n", __LINE__, #c);                      \
        }                                                                        \
    } while (0)

static bool has(const std::vector<uint8_t> &v, uint8_t x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

int main() {
    std::printf("=== test_vm_target (la VM como target del allocator) ===\n");

    const size_t GP = static_cast<size_t>(jit::RegClass::GP);

    /* --- 1. El descriptor coincide con lo que el emisor asume HOY --------- */
    {
        const jit::TargetRegInfo &t = codegen::target_vm();

        // Asignables = r0..r12, es decir EXACTAMENTE ir::ALLOC_REGS registros.
        CHECK(t.allocatable[GP].size() == static_cast<size_t>(ir::ALLOC_REGS));
        for (uint8_t r = 0; r < ir::ALLOC_REGS; ++r) CHECK(has(t.allocatable[GP], r));

        // Los scratch del emisor NO son asignables (si lo fueran, el allocator
        // daria a un valor SSA el registro que el emisor usa para materializar
        // un derrame -> lo pisaria).
        CHECK(!has(t.allocatable[GP], static_cast<uint8_t>(ir::SCRATCH_REG)));
        CHECK(!has(t.allocatable[GP], static_cast<uint8_t>(ir::SCRATCH2_REG)));
        CHECK(!has(t.allocatable[GP], static_cast<uint8_t>(ir::ARGC_REG)));
        CHECK(has(t.scratch[GP], static_cast<uint8_t>(ir::SCRATCH_REG)));
        CHECK(has(t.scratch[GP], static_cast<uint8_t>(ir::SCRATCH2_REG)));
        CHECK(has(t.reserved, static_cast<uint8_t>(ir::ARGC_REG)));

        // Convencion de llamada del emisor: retorno en r0, argumentos r1-r12.
        CHECK(t.ret_reg[GP] == 0);
        CHECK(t.arg_regs[GP].size() == 12);
        CHECK(t.arg_regs[GP].front() == 1);
        CHECK(t.arg_regs[GP].back() == 12);

        /* Que sobrevive a un CALL.  El emisor salva y restaura los valores
         * vivos alrededor de la llamada, asi que r1-r12 SI sobreviven.  r0 NO:
         * es donde el callee deja el RETORNO, y lo escribe despues de cualquier
         * restauracion.
         *
         * Este test afirmaba antes lo contrario ("ningun asignable es volatil:
         * todos sobreviven").  Era falso, y no lo delataba nadie porque el
         * asignador del interprete esquivaba r0 por el ORDEN de su pool -- una
         * preferencia implicita, no una regla.  Al conectar el modelo repartia
         * r0 el primero y rompia cinco programas del corpus.  Ver
         * @c codegen/vm_isa_facts.h. */
        CHECK(t.caller_saved[GP].size() == 1);
        CHECK(has(t.caller_saved[GP], 0)); // r0 = retorno -> volatil
        CHECK(!has(t.callee_saved[GP], 0));
        CHECK(t.callee_saved[GP].size() + t.caller_saved[GP].size() ==
              t.allocatable[GP].size()); // la particion cubre el pool entero
        for (uint8_t r = 1; r <= 12; ++r) CHECK(has(t.callee_saved[GP], r));

        CHECK(t.pointer_size == 8);
    }

    /* --- 2. Reserva POR DEMANDA: sin scratch el pool crece a los 16 ------- */
    {
        const jit::TargetRegInfo &t = codegen::target_vm(/*reserve_scratch=*/false);
        CHECK(t.allocatable[GP].size() == 16);
        CHECK(has(t.allocatable[GP], static_cast<uint8_t>(ir::SCRATCH_REG)));
        CHECK(has(t.allocatable[GP], static_cast<uint8_t>(ir::ARGC_REG)));
        CHECK(t.scratch[GP].empty());
        CHECK(t.reserved.empty());
        // Reservar siempre cuesta 3 de 16 registros: eso es lo que mide esto.
        CHECK(t.allocatable[GP].size() - codegen::target_vm().allocatable[GP].size() == 3);
    }

    /* --- 3. El banco fisico se construye desde el descriptor -------------- */
    {
        const PhysicalRegisterBank bank = physical_bank_x86_64_from_reginfo(
            codegen::target_vm(), jit::backend_caps_host());
        // Cada asignable del descriptor existe como lane consultable del banco.
        for (uint8_t r = 0; r < ir::ALLOC_REGS; ++r) {
            const Lane *l = bank.by_id(r);
            CHECK(l != nullptr);
            if (l) CHECK(l->cls == ResourceClass::GP);
        }
        // El aliasing debe ser describible para TODA lane asignable: sin el, la
        // ocupacion se vuelve fail-safe y el allocator pierde precision.
        for (uint8_t r = 0; r < ir::ALLOC_REGS; ++r)
            CHECK(bank.aliases_of(r) != nullptr);
    }

    std::printf("--- %d checks, %d fallos ---\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
