/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file jit/asm_deferred.cpp
 * @brief Implementacion del ensamblado diferido.  Ver jit/asm_deferred.h.
 */

#include "jit/asm_deferred.h"

#include "vx/asm/asm_backend.h"
#include "vx/asm/asm_phys_reg.h"

#include <cctype>

namespace jit {

AsmDeferredResult asm_deferred_assemble(const AsmBlob &b,
                                        const codegen::AllocationResult &alloc,
                                        const MFunction &vf) {
    AsmDeferredResult r;
    if (vx::g_asm_backend == nullptr) {
        r.fallo = AsmDeferredFallo::SIN_ENSAMBLADOR;
        return r;
    }
    std::string nasm;
    const std::string &t = b.deferred_tmpl;

    for (size_t i = 0; i < t.size();) {
        if (t[i] != '$') {
            nasm += t[i++];
            continue;
        }
        // Leer el numero del marcador.
        size_t j = i + 1;
        uint32_t idx = 0;
        bool any = false;
        while (j < t.size() && std::isdigit(static_cast<unsigned char>(t[j]))) {
            idx = idx * 10 + static_cast<uint32_t>(t[j] - '0');
            ++j;
            any = true;
        }
        if (!any || idx >= b.deferred_ops.size()) {
            nasm += t[i++]; // '$' suelto o fuera de rango: tal cual
            continue;
        }
        const AsmBlob::DeferredOp &d = b.deferred_ops[idx];
        /* Un numero no tiene registro que repartir: se escribe y ya. */
        if (d.es_inmediato) {
            nasm += std::to_string(d.inmediato);
            i = j;
            continue;
        }
        int phys = d.fixed_phys;
        if (phys < 0) {
            const auto loc = alloc.timeline.first_location(d.vreg);
            if (!loc.is_register()) {
                /* Sin registro no hay bloque: un operando de asm no se puede
                 * leer de la pila, porque el cuerpo lo nombra como registro.
                 * Pasa cuando se piden mas operandos de los que tiene el banco
                 * del objetivo. */
                r.fallo = AsmDeferredFallo::SIN_REGISTRO;
                r.operando = idx;
                r.vreg = d.vreg;
                r.clase = d.regclass;
                r.ancho = d.width;
                r.en_memoria = loc.is_memory();
                return r;
            }
            phys = static_cast<int>(loc.register_id());
        }
        /* El asignador numera las ranuras de forma CORRIDA sobre todo el banco:
         * las del banco ancho empiezan en XMM0 = 16.  El nombre, en cambio,
         * lleva el numero DENTRO de su banco: `ymm3`, no `ymm19`. */
        if (d.fixed_phys < 0 && d.regclass != vx::ASM_RC_GP)
            phys -= static_cast<int>(MReg::XMM0);
        const std::string nm =
            vx::asm_phys_reg_name(b.deferred_isa, d.regclass, phys, d.width);
        if (nm.empty()) {
            r.fallo = AsmDeferredFallo::SIN_NOMBRE;
            r.operando = idx;
            r.vreg = d.vreg;
            r.clase = d.regclass;
            r.ancho = d.width;
            r.ranura = phys;
            return r;
        }
        /* Una direccion se escribe entera y con la sintaxis de SU ISA: x86 pone
         * `[rax + 8]` y arm64 `[x0, #8]`.  Componerla aqui seria escribir una
         * arquitectura concreta en el resolvedor de todas. */
        if (d.es_direccion) {
            const std::string dir =
                vx::asm_mem_operando(b.deferred_isa, nm, d.desplazamiento);
            if (dir.empty()) {
                r.fallo = AsmDeferredFallo::SIN_NOMBRE;
                r.operando = idx;
                r.vreg = d.vreg;
                r.clase = d.regclass;
                r.ancho = d.width;
                r.ranura = phys;
                return r;
            }
            nasm += dir;
            i = j;
            continue;
        }
        nasm += nm;
        i = j;
    }

    r.texto = nasm;
    /* El texto FINAL, ya con registros puestos.  Es el dato que faltaba las dos
     * veces que hubo que averiguar por que un bloque salia mal: los veredictos
     * por operando se veian, pero lo que de verdad se ensambla no.  Y cuando el
     * mismo bloque da un resultado en el interprete y otro aqui, la diferencia
     * esta justo en estos dos textos. */
    if (std::getenv("VESTA_JIT_ASM_DUMP") != nullptr) {
        std::fprintf(stderr, "[asm-jit] plantilla: %s\n",
                     b.deferred_tmpl.c_str());
        std::fprintf(stderr, "[asm-jit] final:\n%s\n", nasm.c_str());
    }
    vx::AsmAssembleResult ar =
        vx::g_asm_backend->assemble(nasm, vx::AsmArch::X86_64);
    if (!ar.ok) {
        r.fallo = AsmDeferredFallo::NO_ENSAMBLA;
        r.detalle = ar.error;
        return r;
    }
    /* Cero bytes con `ok` es legitimo: un bloque que solo lleva comentarios no
     * emite nada. */
    r.bytes = std::move(ar.bytes);
    r.ok = true;
    return r;
}

} // namespace jit
