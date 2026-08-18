/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/jit/test_lane_occupancy.cpp
 * @brief @c LaneOccupancy: el conocimiento compartido "¿esta libre esta lane
 * aqui?".
 *
 * Se prueba AISLADO de sus tres consumidores (Recovery, taxonomia, splitting)
 * porque es justo su razon de ser: que los tres respondan lo MISMO.  Cubre las
 * dos propiedades que el componente promete y que nadie mas verifica:
 *
 *   - el INVARIANTE de normalizacion (ordenada, disjunta, con los contiguos
 * fusionados),
 *   - el ALIASING (ocupar una lane ocupa las que comparten recurso con ella) --
 * la propiedad de la que depende la correctitud cuando una vista estrecha (EAX)
 * y una ancha (RAX) son el mismo registro fisico.
 */

#include "codegen/rbank/lane_occupancy.h"

#include <cstdio>
#include <vector>

using namespace codegen::rbank;

static int g_checks = 0, g_fails = 0;
#define CHECK(c)                                                               \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(c)) {                                                            \
            ++g_fails;                                                         \
            std::printf("  FALLO L%d: %s\n", __LINE__, #c);                    \
        }                                                                      \
    } while (0)

namespace {

/**
 * @brief Banco SINTETICO de 3 lanes donde 0 y 1 COMPARTEN recurso fisico (como
 * RAX y EAX) y la 2 es independiente.  Sintetico a proposito: el banco x86-64
 * real da hoy a cada lane su propio bit, asi que el aliasing no se ejercitaria
 * nunca -- y es precisamente la logica que hay que blindar antes de que el
 * banco modele vistas solapadas (o llegue una ISA que las tenga).
 */
PhysicalRegisterBank make_aliasing_bank() {
    PhysicalRegisterBank b;
    for (uint8_t i = 0; i < 3; ++i) {
        Lane l;
        l.id = i;
        l.cls = ResourceClass::GP;
        b.id_to_index[i] =
            static_cast<int16_t>(b.lanes.size()); // indice, no id (I3).
        b.lanes.push_back(l);
    }
    b.lanes[0].aliases.add(0); // lane 0 ocupa el recurso 0 ...
    b.lanes[1].aliases.add(
        0); // ... y la lane 1 TAMBIEN (vista estrecha del mismo).
    b.lanes[2].aliases.add(1); // independiente.
    return b;
}

} // namespace

int main() {
    std::printf("=== test_lane_occupancy ===\n");

    /* --- 1. Invariante de normalizacion: ordenada, disjunta, contiguos
     * fusionados --- */
    {
        std::vector<PosRange> l;
        insert_merged(l, {10, 20});
        insert_merged(
            l, {21, 25}); // CONTIGUO (no solapa) -> misma ocupacion: fusiona.
        CHECK(l.size() == 1);
        CHECK(l[0].from == 10 && l[0].to == 25);

        insert_merged(l,
                      {40, 50}); // disjunto -> se anyade manteniendo el orden.
        CHECK(l.size() == 2);
        CHECK(l[1].from == 40);

        insert_merged(l,
                      {26, 39}); // cierra el hueco -> los tres colapsan en uno.
        CHECK(l.size() == 1);
        CHECK(l[0].from == 10 && l[0].to == 50);

        insert_merged(l, {0, 5}); // por delante, sin tocar -> queda PRIMERO.
        CHECK(l.size() == 2);
        CHECK(l[0].from == 0 && l[0].to == 5);
        CHECK(l[1].from == 10);

        insert_merged(l, {12, 13}); // contenido -> no cambia nada.
        CHECK(l.size() == 2);
        CHECK(l[1].from == 10 && l[1].to == 50);

        // El numero de tramos NO crece con el de inserciones dentro de lo ya
        // ocupado: es lo que mantiene acotado el coste cuando el splitting
        // comprometa muchos huecos.
        for (uint32_t i = 0; i < 100; ++i)
            insert_merged(l, {10 + i, 11 + i});
        CHECK(l.size() == 2);
    }

    /* --- 2. Consultas basicas sobre una lane independiente --- */
    {
        const PhysicalRegisterBank bank = make_aliasing_bank();
        LaneOccupancy occ(bank);
        CHECK(occ.is_free(2, {0, 100})); // vacia -> libre.

        occ.occupy(2, {20, 40});
        CHECK(!occ.is_free(2, {30, 35})); // dentro.
        CHECK(!occ.is_free(2, {0, 20}));  // toca el borde inicial.
        CHECK(occ.is_free(2, {0, 19}));   // justo antes.
        CHECK(occ.is_free(2, {41, 100})); // justo despues.

        std::vector<PosRange> w;
        occ.free_windows(2, {0, 100}, w);
        CHECK(w.size() == 2);
        if (w.size() == 2) {
            CHECK(w[0].from == 0 && w[0].to == 19);
            CHECK(w[1].from == 41 && w[1].to == 100);
        }
        CHECK(occ.free_time(2, {0, 100}) == 20 + 60); // [0,19] + [41,100]
    }

    /* --- 3. ALIASING: ocupar la lane 0 ocupa TAMBIEN la 1 (mismo recurso
     * fisico) --- */
    {
        const PhysicalRegisterBank bank = make_aliasing_bank();
        LaneOccupancy occ(bank);
        occ.occupy(0, {10, 30});

        CHECK(!occ.is_free(0, {15, 20})); // obvio: es la propia lane.
        CHECK(!occ.is_free(
            1, {15, 20})); // CLAVE: la vista aliasada tampoco esta libre.
        CHECK(occ.is_free(2, {15, 20})); // la independiente no se entera.

        std::vector<PosRange> w;
        occ.free_windows(1, {12, 25}, w);
        CHECK(w.empty()); // enteramente dentro del tramo ocupado por su alias.
        CHECK(occ.free_time(1, {10, 30}) == 0);

        occ.free_windows(1, {0, 40}, w); // el hueco se ve por los dos lados.
        CHECK(w.size() == 2);
        if (w.size() == 2) {
            CHECK(w[0].to == 9);
            CHECK(w[1].from == 31);
        }
    }

    /* --- 4. Varios tramos en la misma lane: los huecos INTERMEDIOS se ven ---
     */
    {
        const PhysicalRegisterBank bank = make_aliasing_bank();
        LaneOccupancy occ(bank);
        occ.occupy(2, {0, 10});
        occ.occupy(2, {30, 40});
        std::vector<PosRange> w;
        occ.free_windows(2, {0, 50}, w);
        CHECK(w.size() == 2);
        if (w.size() == 2) {
            CHECK(w[0].from == 11 &&
                  w[0].to == 29); // el hueco util del splitting.
            CHECK(w[1].from == 41 && w[1].to == 50);
        }
        // Recortar la consulta recorta la respuesta (nunca devuelve fuera de
        // rango).
        occ.free_windows(2, {12, 20}, w);
        CHECK(w.size() == 1);
        if (w.size() == 1) CHECK(w[0].from == 12 && w[0].to == 20);
    }

    /* --- 5. PROPIEDAD DE SEGURIDAD (fail-safe): la falta de informacion de
     * aliasado NUNCA puede producir una falsa disponibilidad.
     *
     *        Se simula un banco INCOMPLETO -- una lane que existe pero que el
     * banco no sabe describir (@c aliases_of devuelve nullptr, como pasaria con
     * un registro nuevo, una vista nueva o una ISA cuyo alias se olvido
     * declarar).  El contrato es que se trate como OCUPADA: se pierde la
     * oportunidad de optimizar, jamas se planifica sobre algo realmente
     * ocupado. --- */
    {
        PhysicalRegisterBank bank = make_aliasing_bank();
        bank.id_to_index[1] =
            -1; // la lane 1 deja de ser describible por el banco.
        LaneOccupancy occ(bank);
        occ.occupy(0, {10, 30});

        /* Consultar una lane indescriptible: NUNCA libre, en ninguna posicion
         * -- no se puede alojar nada donde no se sabe con quien se choca. */
        CHECK(!occ.is_free(1, {15, 20}));
        CHECK(!occ.is_free(1,
                           {90, 95})); // ni siquiera donde no hay nada ocupado.
        std::vector<PosRange> w;
        occ.free_windows(1, {0, 100}, w);
        CHECK(w.empty()); // sin huecos que ofrecer al splitting.
        CHECK(occ.free_time(1, {0, 100}) == 0);

        /* Y la degradacion NO se propaga: las lanes que el banco SI describe se
         * siguen razonando con precision (la 2 no comparte recurso con la 0).
         */
        CHECK(occ.is_free(2, {15, 20}));
        CHECK(occ.is_free(2, {0, 100}));
        CHECK(!occ.is_free(0,
                           {15, 20})); // la 0 sigue viendo su propia ocupacion.
    }

    /* --- 6. La otra mitad del fail-safe: ocupacion registrada SOBRE una lane
     * que el banco no describe.  Ahi si se ignora que toca, asi que choca con
     * todas -- pero solo en los tramos que cubre de verdad (no inventa
     * ocupacion). --- */
    {
        PhysicalRegisterBank bank = make_aliasing_bank();
        bank.id_to_index[1] = -1;
        LaneOccupancy occ(bank);
        occ.occupy(1, {10, 30}); // alguien ocupa una lane indescriptible.

        CHECK(!occ.is_free(
            2, {15, 20})); // la 2 no puede demostrar que no le afecta.
        CHECK(!occ.is_free(0, {15, 20}));
        CHECK(occ.is_free(
            2, {40, 50})); // fuera del tramo: sigue libre (no se inventa).
        std::vector<PosRange> w;
        occ.free_windows(2, {0, 100}, w);
        CHECK(w.size() == 2);
        if (w.size() == 2) {
            CHECK(w[0].to == 9);
            CHECK(w[1].from == 31);
        }
    }

    std::printf("--- %d checks, %d fallos ---\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
