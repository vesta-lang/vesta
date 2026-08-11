/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/analysis/test_locset_indice.cpp
 * @brief La consulta indexada contesta EXACTAMENTE lo mismo que recorrerlo todo.
 *
 * `LocSet::may_alias_any` dejo de recorrer el conjunto entero para mirar solo el
 * cubo de la raiz preguntada.  Eso es una optimizacion sobre una consulta de
 * ALIASING, y ahi equivocarse no se ve: una respuesta de menos no hace nada mas
 * lento -- deja que el optimizador borre codigo vivo, y el programa hace otra
 * cosa sin que nada falle al compilar.
 *
 * Por eso aqui no se comprueba que "funcione": se comprueba que la version
 * indexada y el recorrido lineal dan la MISMA respuesta para toda consulta
 * sobre conjuntos construidos a proposito para tocar cada rama de la regla --
 * el TOP del conjunto y el de la pregunta, la raiz generica por cada clase, dos
 * raices concretas distintas, y el solapamiento de rangos con y sin ancho.
 *
 * El oraculo es el recorrido lineal escrito aqui: es la definicion de la que
 * salio el indice, asi que si los dos coinciden en todo, el indice no miente.
 */

#include "analysis/effects/effects.h"

#include <cstdio>
#include <vector>

using namespace analysis::effects;
using K = AbstractLoc::Kind;

static int g_checks = 0;
static int g_fail = 0;

/// El oraculo: la regla, aplicada a pelo sobre todos los elementos.
static bool lineal(const LocSet &s, const AbstractLoc &l) {
    if (l.kind == K::None) return false;
    if (s.is_top) return true;
    for (const AbstractLoc &e : s.locs)
        if (may_alias(e, l)) return true;
    return false;
}

static AbstractLoc loc(K k, uint32_t id, int64_t off = 0, int32_t w = 0) {
    AbstractLoc a;
    a.kind = k;
    a.id = id;
    a.off = off;
    a.width = w;
    return a;
}

/// Compara las dos respuestas para @p l sobre @p s.
static void comparar(const LocSet &s, const AbstractLoc &l, const char *que) {
    ++g_checks;
    const bool esperado = lineal(s, l);
    const bool obtenido = s.may_alias_any(l);
    if (esperado != obtenido) {
        ++g_fail;
        std::printf("  [FALLO] %s: lineal=%d indexado=%d (kind=%d id=%u off=%lld"
                    " w=%d)\n",
                    que, esperado ? 1 : 0, obtenido ? 1 : 0,
                    static_cast<int>(l.kind), l.id,
                    static_cast<long long>(l.off), l.width);
    }
}

/// Todas las consultas interesantes contra @p s.
static void barrer_consultas(const LocSet &s, const char *que) {
    static const K kinds[] = {K::None,       K::Stack,  K::Heap,
                              K::Global,     K::ArgDerived, K::Unknown};
    static const uint32_t ids[] = {0, 1, 2, 7, LOC_GENERIC};
    static const int64_t offs[] = {-8, 0, 4, 8, 64};
    static const int32_t anchos[] = {0, 1, 4, 8};
    for (K k : kinds)
        for (uint32_t id : ids)
            for (int64_t off : offs)
                for (int32_t w : anchos) comparar(s, loc(k, id, off, w), que);
}

int main() {
    std::printf("=== LocSet: el indice contesta lo mismo que el recorrido ===\n");

    /* Vacio: nada aliasa nada. */
    {
        LocSet s;
        barrer_consultas(s, "vacio");
    }
    /* TOP del conjunto: aliasa cualquier cosa que no sea None. */
    {
        LocSet s;
        s.is_top = true;
        barrer_consultas(s, "conjunto TOP");
    }
    /* Raices concretas de varias clases, con y sin ancho. */
    {
        LocSet s;
        s.add(loc(K::Stack, 1, 0, 4));
        s.add(loc(K::Stack, 1, 8, 4));
        s.add(loc(K::Stack, 2, 0, 8));
        s.add(loc(K::Heap, 1, 0, 0));
        s.add(loc(K::Global, 7, 16, 8));
        s.add(loc(K::ArgDerived, 0, 0, 4));
        barrer_consultas(s, "raices concretas");
    }
    /* Con la raiz generica de una clase: aliasa TODA su clase. */
    {
        LocSet s;
        s.add(loc(K::Stack, 1, 0, 4));
        s.add(loc(K::Heap, LOC_GENERIC));
        barrer_consultas(s, "generico de heap");
    }
    /* Con un TOP DENTRO del conjunto (distinto de is_top). */
    {
        LocSet s;
        s.add(loc(K::Stack, 1, 0, 4));
        s.add(loc(K::Unknown, 0));
        barrer_consultas(s, "TOP dentro");
    }
    /* Y el indice tiene que seguir al conjunto cuando este CAMBIA: se consulta
     * (que construye el indice), se anade, y se vuelve a consultar.  Un indice
     * que se quedara con la foto vieja contestaria que no aliasa algo que si. */
    {
        LocSet s;
        s.add(loc(K::Stack, 1, 0, 4));
        barrer_consultas(s, "antes de crecer");
        s.add(loc(K::Global, 3, 0, 4));
        barrer_consultas(s, "tras anadir");
        LocSet otro;
        otro.add(loc(K::Heap, 9, 0, 4));
        s.unite(otro);
        barrer_consultas(s, "tras unir");
        s.clear();
        barrer_consultas(s, "tras vaciar");
    }
    /* Copiar un conjunto no puede llevarse un indice que no corresponda. */
    {
        LocSet s;
        s.add(loc(K::Stack, 1, 0, 4));
        (void)s.may_alias_any(loc(K::Stack, 1, 0, 4)); // construye el indice
        LocSet copia = s;
        copia.add(loc(K::Global, 5, 0, 4));
        barrer_consultas(copia, "copia que crece");
        barrer_consultas(s, "original tras copiar");
    }

    std::printf("%d comprobaciones, %d fallos\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
