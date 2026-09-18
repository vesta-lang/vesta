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

bool same_signature(const std::vector<Type> &ta, const ParamNames &na,
                    const std::vector<Type> &tb, const ParamNames &nb) {
    if (ta != tb) return false;
    /* Sin nombres no se puede afirmar que sean distintas, y ahi lo conservador
     * es decir que son la MISMA: inventarse que difieren dejaria pasar dos
     * declaraciones que de verdad chocan. */
    if (na.size() != ta.size() || nb.size() != tb.size()) return true;
    // Del pozo: comparar dos nombres es comparar dos punteros.
    for (size_t i = 0; i < na.size(); ++i)
        if (!(na[i] == nb[i])) return false;
    return true;
}

std::string discriminator(const std::vector<Type> &params,
                          const ParamNames *names) {
    std::string d = vxgen::mangle_args(params);
    /* Los nombres solo entran cuando son lo UNICO que separa.  Asi el simbolo
     * de todo lo que ya existia no cambia -- y lo que separa dos hermanas esta
     * DENTRO de su etiqueta, que es lo que impide que acaben compartiendola. */
    if (names != nullptr)
        for (const PooledName &n : *names) {
            d += '_';
            d += n.str();
        }
    return d;
}

/**
 * @brief Pone los argumentos de @p args en el orden de ESTA candidata.
 *
 * Lo NOMBRADO va a su ranura y lo posicional a las que quedan libres.  Ese
 * orden, y no el de los indices, es lo que hace que el receptor de una llamada
 * con punto caiga donde debe: en `5.restar(.a = 25)` el 25 toma `a`, asi que el
 * 5 -- que viaja como posicional -- va a `b`, la unica que queda.
 *
 * @return false si esta candidata no puede recibir esta llamada: no tiene un
 *         parametro con ese nombre, dos argumentos caen en la misma ranura, o
 *         alguna queda sin llenar (los parametros no tienen valor por defecto).
 */
static bool reorder_named(const Candidate &c, const std::vector<Type> &args,
                          const ParamNames &names, std::vector<Type> &out) {
    if (c.params == nullptr || c.param_names == nullptr) return false;
    const ParamNames &pn = *c.param_names;
    const size_t np = c.params->size();
    if (pn.size() != np || args.size() != np) return false;
    out.assign(np, Type{});
    // En pila: una firma de hasta ocho parametros no toca el monton.
    util::SmallVector<uint8_t, 8> taken;
    taken.resize(np, 0);
    for (size_t k = 0; k < args.size(); ++k) {
        if (k >= names.size() || names[k].empty()) continue;
        size_t at = np;
        for (size_t j = 0; j < np; ++j)
            if (pn[j] == names[k]) {
                at = j;
                break;
            }
        if (at == np || taken[at]) return false;
        out[at] = args[k];
        taken[at] = 1;
    }
    size_t free_slot = 0;
    for (size_t k = 0; k < args.size(); ++k) {
        if (k < names.size() && !names[k].empty()) continue;
        while (free_slot < np && taken[free_slot])
            ++free_slot;
        if (free_slot == np) return false;
        out[free_slot] = args[k];
        taken[free_slot] = 1;
    }
    for (size_t j = 0; j < np; ++j)
        if (!taken[j]) return false;
    return true;
}

/**
 * @brief Si esta candidata acepta estos argumentos, ya puestos en su orden.
 *
 * Sacada de @c select para que alli quede la REGLA DE PREFERENCIA -- exacta
 * antes que compatible, cerrada antes que variadica -- y aqui la comparacion,
 * que es otra cosa.  Juntas eran cuatro niveles de anidamiento y un `continue`
 * que habia que seguir con el dedo.
 *
 * @param exact true en la pasada estricta: el tipo tiene que ser EL MISMO, no
 *              uno al que se pueda convertir.
 */
static bool candidate_fits(const Candidate &c, const std::vector<Type> &args,
                           bool exact, AcceptsFn accepts, void *ctx) {
    const std::vector<Type> &p = *c.params;
    const Type *elem = c.variadic_elem;
    const bool open = elem != nullptr || c.raw_variadic;
    /* La aridad de una abierta es un MINIMO, no un numero.  Cuantos son los
     * FIJOS depende de cual de las dos es: en un `T... xs` el ultimo parametro
     * ES el array y no cuenta; en un `...` crudo no hay parametro que anyadir,
     * asi que cuentan todos.  Restar uno a este ultimo se comia un parametro de
     * verdad. */
    const size_t fixed = (elem != nullptr) ? p.size() - 1 : p.size();
    if (open ? (args.size() < fixed) : (args.size() != fixed)) return false;
    for (size_t k = 0; k < args.size(); ++k) {
        // Un argumento que no se pudo tipar no descarta a nadie: su error ya
        // esta dado, y descartar por el solo anyadiria un segundo mensaje sobre
        // algo que si existe.
        if (args[k].kind == PrimitiveKind::COUNT) continue;
        /* Lo que sobra de un `...` crudo no se comprueba contra nada: no hay
         * tipo declarado con que hacerlo, y es lo que el lenguaje promete. */
        if (k >= fixed && elem == nullptr) continue;
        /* Los de mas se comparan con el tipo del ELEMENTO; los fijos, con el
         * suyo.  Y un parametro por REFERENCIA, con lo APUNTADO: quien llama
         * cede el hueco, no su direccion. */
        const Type &declared = (k >= fixed) ? *elem : p[k];
        const bool by_ref =
            k < fixed && k < 64 && (c.by_ref_mask & (1ull << k)) != 0;
        const Type &expected =
            (by_ref && declared.pointee) ? *declared.pointee : declared;
        if (!(exact ? (expected == args[k]) : accepts(ctx, expected, args[k])))
            return false;
    }
    return true;
}

uint32_t select(const Candidate *cands, size_t n, const std::vector<Type> &args,
                AcceptsFn accepts, void *ctx, const ParamNames *arg_names,
                uint32_t *other_fit) {
    if (other_fit != nullptr) *other_fit = kNoPick;
    /* Con nombres la lista hay que reordenarla POR CANDIDATA; sin ellos, que es
     * el caso normal, esto es una sonda a un puntero y el camino de siempre. */
    const bool named = arg_names != nullptr && !arg_names->empty();
    std::vector<Type> reordered;
    for (int pass = 0; pass < 2; ++pass) {
        /* La que va ganando, cuando hay una hermana que toma LO MISMO y podria
         * encajar tambien.  En el caso normal no llega a usarse: la primera que
         * encaja se devuelve y el recorrido acaba ahi. */
        uint32_t winner = kNoPick;
        /* Y dentro de cada pasada, las de aridad CERRADA antes que las
         * variadicas.  Una variadica es por definicion la que lo recoge todo,
         * asi que si compite a la vez que una que dice exactamente lo que toma,
         * gana esa: `f(i64)` frente a `f(i64...)` con un argumento tiene que
         * ser la primera, que es lo que el usuario escribio para ese caso. */
        for (int open = 0; open < 2; ++open) {
            for (size_t i = 0; i < n; ++i) {
                const Candidate &c = cands[i];
                if (c.params == nullptr) continue;
                const bool is_open =
                    c.variadic_elem != nullptr || c.raw_variadic;
                if (is_open != (open == 1)) continue;
                /* Los argumentos EN EL ORDEN DE ESTA candidata.  Una variadica
                 * no admite nombres: a que ranura va un `.x = v` cuando la
                 * ultima recoge cuantos vengan no esta decidido, y decidirlo a
                 * medias seria peor que no aceptarlo. */
                const std::vector<Type> *use = &args;
                if (named) {
                    if (is_open) continue;
                    if (!reorder_named(c, args, *arg_names, reordered))
                        continue;
                    use = &reordered;
                }
                if (!candidate_fits(c, *use, pass == 0, accepts, ctx)) continue;
                /* El caso normal: la primera que encaja ES la respuesta, y el
                 * recorrido acaba aqui.  Solo cuando hay una hermana que toma
                 * LO MISMO se sigue mirando, para poder DECIR que la llamada no
                 * distingue entre las dos en vez de quedarse con una. */
                if (!c.needs_names) return c.slot;
                if (winner == kNoPick) {
                    winner = c.slot;
                    continue;
                }
                if (other_fit != nullptr) *other_fit = c.slot;
                return winner;
            }
        }
        if (winner != kNoPick) return winner;
    }
    return kNoPick;
}

} // namespace overload
} // namespace vx
