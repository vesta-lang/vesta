/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file overload.cpp
 * @brief La regla de la sobrecarga.  Ver @ref overload.h.
 */

#include "vx/overload.h"

#include "vx/generics/generic_clone.h"

namespace vx {
namespace overload {

bool same_params(const std::vector<Type> &a, const std::vector<Type> &b) {
    // Comparar dos listas de tipos no reserva nada; construir una clave de
    // texto para lo mismo reservaria una cadena por metodo del programa.
    return a == b;
}

std::string discriminator(const std::vector<Type> &params) {
    return vxgen::mangle_args(params);
}

uint32_t select(const Candidate *cands, size_t n, const std::vector<Type> &args,
                AcceptsFn accepts, void *ctx) {
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < n; ++i) {
            const std::vector<Type> *p = cands[i].params;
            if (p == nullptr || p->size() != args.size()) continue;
            bool fits = true;
            for (size_t k = 0; k < args.size() && fits; ++k) {
                // Un argumento que no se pudo tipar no descarta a nadie: su
                // error ya esta dado, y descartar por el solo anyadiria un
                // segundo mensaje sobre algo que si existe.
                if (args[k].kind == PrimitiveKind::COUNT) continue;
                /* Un parametro por REFERENCIA se compara contra lo apuntado:
                 * quien llama cede el hueco, no su direccion. */
                const Type &declared = (*p)[k];
                const bool by_ref =
                    k < 64 && (cands[i].by_ref_mask & (1ull << k)) != 0;
                const Type &expected =
                    (by_ref && declared.pointee) ? *declared.pointee : declared;
                fits = (pass == 0) ? (expected == args[k])
                                   : accepts(ctx, expected, args[k]);
            }
            if (fits) return cands[i].slot;
        }
    }
    return kNoPick;
}

} // namespace overload
} // namespace vx
