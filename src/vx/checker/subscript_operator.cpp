/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vx/checker/subscript_operator.cpp
 * @brief Los operadores de subindice de un struct o clase: `x[i]`,
 *        `x[i] = v` y `x[a..b]`.
 *
 * Un dueno para "que metodo atiende este subindice".  La lectura y la
 * escritura por indice tenian cada una su busqueda escrita a mano, con el
 * mismo recorrido de los metodos del tipo; el corte habria sido la tercera.
 */

#include "vx/method_names.h"
#include "vx/type_checker.h"

namespace vx {

const ClassMethodInfo *
TypeChecker::find_subscript_operator(const Type &recv, const char *name,
                                     SubscriptArg *args, size_t n) {
    /* Solo struct y clase.  `receiver_methods` acepta ademas un `Struct*`,
     * porque el punto lo desreferencia solo; en un subindice no: `p[i]` sobre
     * un puntero indexa el PUNTERO. */
    if (recv.kind != PrimitiveKind::STRUCT && recv.kind != PrimitiveKind::CLASS)
        return nullptr;
    const std::vector<ClassMethodInfo> *ms =
        receiver_methods(recv, nullptr, nullptr, nullptr);
    if (ms == nullptr) return nullptr;
    for (const ClassMethodInfo &m : *ms) {
        if (m.is_constructor || m.is_static || m.name != name) continue;
        if (m.param_types.size() != n) continue;
        /* Cada argumento cabe donde cabria en una llamada escrita -- un literal
         * en un `usize`, una subclase donde se pide la base --: con la regla a
         * mano, `s[0]` no encontraba `__index__(usize)` y `s.__index__(0)` si.
         * Un limite omitido (expresion nula) cabe siempre: lo pone la
         * normalizacion, y es un entero sin signo. */
        bool fits = true;
        for (size_t i = 0; i < n && fits; ++i)
            fits = arg_fits_param(args[i].expr, m.param_types[i], args[i].type);
        if (fits) return &m;
    }
    return nullptr;
}

Type TypeChecker::check_range_operator_(ast::IndexExpr *e, const Type &bt) {
    const Type lo_t = e->index ? e->index->result_type : Type{};
    const Type hi_t = e->range_hi ? e->range_hi->result_type : Type{};
    SubscriptArg args[2] = {{e->index.get(), lo_t}, {e->range_hi.get(), hi_t}};
    const ClassMethodInfo *rd = find_subscript_operator(bt, kSliceMethod, args, 2);
    const ClassMethodInfo *wr =
        find_subscript_operator(bt, kSliceMutMethod, args, 2);
    if (rd == nullptr && wr == nullptr) {
        diags_.diag(e->loc, DiagLevel::ERR, "VX2154", {written_type_name(bt)});
        return Type{};
    }

    /* Cual lo decide el DESTINO, como en las listas de inicializacion: si lo
     * que se pide es lo que da `__slice_mut__` y no lo que da `__slice__`, se
     * escribe; en cualquier otro caso, se lee.  Sin destino -- un argumento de
     * una generica, un `auto` -- se lee.  Y si el tipo declara uno solo, ese. */
    const ClassMethodInfo *pick = rd != nullptr ? rd : wr;
    if (rd != nullptr && wr != nullptr && expected_value_node_ == e &&
        expected_value_type_.kind != PrimitiveKind::COUNT &&
        types_assignable(expected_value_type_, wr->return_type) &&
        !types_assignable(expected_value_type_, rd->return_type))
        pick = wr;

    /* Sin superior (`x[a..]`), hasta el final: la longitud la da el tipo con
     * `len()`, que es lo que la normalizacion llama al bajar. */
    if (!e->range_hi &&
        find_subscript_operator(bt, kLengthMethod, nullptr, 0) == nullptr) {
        diags_.diag(e->loc, DiagLevel::ERR, "VX2157", {written_type_name(bt)});
        return Type{};
    }
    e->overload_method = pick->name;
    return pick->return_type;
}

} // namespace vx
