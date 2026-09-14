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
        /* Y dentro de cada pasada, las de aridad CERRADA antes que las
         * variadicas.  Una variadica es por definicion la que lo recoge todo,
         * asi que si compite a la vez que una que dice exactamente lo que toma,
         * gana esa: `f(i64)` frente a `f(i64...)` con un argumento tiene que
         * ser la primera, que es lo que el usuario escribio para ese caso. */
        for (int open = 0; open < 2; ++open) {
            for (size_t i = 0; i < n; ++i) {
                const std::vector<Type> *p = cands[i].params;
                if (p == nullptr) continue;
                const Type *elem = cands[i].variadic_elem;
                const bool abierta = elem != nullptr || cands[i].raw_variadic;
                if (abierta != (open == 1)) continue;
                /* La aridad de una abierta es un MINIMO, no un numero, y de ahi
                 * en adelante admite cualquier cantidad.  Es la misma regla que
                 * aplica la llamada cuando el nombre no esta sobrecargado;
                 * pedir aqui medida exacta hacia que dejara de aceptarlos al
                 * aparecer un hermano.
                 *
                 * Cuantos son los FIJOS depende de cual de las dos es: en un
                 * `T... xs` el ultimo parametro ES el array y no cuenta; en un
                 * `...` crudo no hay parametro que anyadir, asi que cuentan
                 * todos.  Restar uno a este ultimo se comia un parametro de
                 * verdad. */
                const size_t fixed =
                    (elem != nullptr) ? p->size() - 1 : p->size();
                if (abierta ? (args.size() < fixed) : (args.size() != fixed))
                    continue;
                bool fits = true;
                for (size_t k = 0; k < args.size() && fits; ++k) {
                    // Un argumento que no se pudo tipar no descarta a nadie: su
                    // error ya esta dado, y descartar por el solo anyadiria un
                    // segundo mensaje sobre algo que si existe.
                    if (args[k].kind == PrimitiveKind::COUNT) continue;
                    /* Lo que sobra de un `...` crudo no se comprueba contra
                     * nada: no hay tipo declarado con que hacerlo, y es lo que
                     * el lenguaje promete de el. */
                    if (k >= fixed && elem == nullptr) continue;
                    /* Los de mas se comparan con el tipo del ELEMENTO; los
                     * fijos, con el suyo.  Y un parametro por REFERENCIA, con
                     * lo apuntado: quien llama cede el hueco, no su
                     * direccion. */
                    const Type &declared = (k >= fixed) ? *elem : (*p)[k];
                    const bool by_ref =
                        k < fixed && k < 64 &&
                        (cands[i].by_ref_mask & (1ull << k)) != 0;
                    const Type &expected = (by_ref && declared.pointee)
                                               ? *declared.pointee
                                               : declared;
                    fits = (pass == 0) ? (expected == args[k])
                                       : accepts(ctx, expected, args[k]);
                }
                if (fits) return cands[i].slot;
            }
        }
    }
    return kNoPick;
}

} // namespace overload
} // namespace vx
