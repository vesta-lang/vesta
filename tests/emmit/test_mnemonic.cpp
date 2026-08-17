/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/emmit/test_mnemonic.cpp
 * @brief Comprueba la lista unica de instrucciones: nombres, categorias y que
 *        cada categoria sea un rango CONTIGUO.
 *
 * Lo de la contiguidad no es cosmetico: de ella depende que preguntar por
 * categoria sean dos comparaciones.  Si alguien reordena la lista y la rompe, las
 * preguntas siguen dando un resultado -- el equivocado --, asi que se comprueba.
 *
 * Y se comprueban mnemonicos POR NOMBRE, no solo el total: la lista se genero
 * extrayendo las claves de la tabla del emisor y el primer intento se dejo SIETE
 * fuera -- `mov` entre ellas -- porque el patron no cogia las entradas escritas
 * en varias lineas.  Un recuento que cuadra no demuestra que esten las que hacen
 * falta.
 */

#include "emmit/mnemonic.h"

#include <cstdio>
#include <cstring>

static int fallos = 0;

static void exige(bool cond, const char *que) {
    if (cond) return;
    std::printf("  FALLA    %s\n", que);
    ++fallos;
}

int main() {
    using namespace emmit;

    std::printf("[mnemonicos] %u instrucciones\n", mnemonic_count());
    exige(mnemonic_count() > 300, "la lista tiene todas las instrucciones");

    /* Por nombre: las que se sabe que existen y las que costo que estuvieran. */
    exige(std::strcmp(text_of(Mnemonic::MOV), "mov") == 0, "MOV -> \"mov\"");
    exige(std::strcmp(text_of(Mnemonic::JMP_JE), "jmp.je") == 0,
          "el punto del mnemonico se conserva en el texto");
    exige(std::strcmp(text_of(Mnemonic::AND), "and") == 0,
          "AND existe (en minuscula seria un token de C++)");
    exige(std::strcmp(text_of(Mnemonic::CALLVIRT), "callvirt") == 0,
          "CALLVIRT -> \"callvirt\"");

    /* Categorys: cada una tiene que ser un intervalo sin huecos. */
    for (uint8_t ci = 0; ci < static_cast<uint8_t>(Category::kCount); ++ci) {
        const auto c = static_cast<Category>(ci);
        const CategoryRange r = range_of(c);
        if (r.empty) continue;
        for (uint16_t i = r.first; i <= r.last; ++i) {
            if (category_of(static_cast<Mnemonic>(i)) == c) continue;
            std::printf("  FALLA    la categoria '%s' no es contigua: %s cae "
                        "dentro de su rango y es de '%s'\n",
                        category_name(c),
                        text_of(static_cast<Mnemonic>(i)),
                        category_name(category_of(static_cast<Mnemonic>(i))));
            ++fallos;
            break;
        }
    }

    /* Y que se resuelve al COMPILAR: si esto no fuera constexpr, no compila. */
    static_assert(category_of(Mnemonic::CALLVIRT) == Category::Object ||
                      category_of(Mnemonic::CALLVIRT) == Category::Call,
                  "la categoria se conoce al compilar");

    /* La frontera texto -> mnemonico. */
    exige(mnemonic_from_text("mov") == Mnemonic::MOV, "\"mov\" -> MOV");
    exige(mnemonic_from_text("jmp.je") == Mnemonic::JMP_JE,
          "un mnemonico con punto se reconoce");
    exige(!is_valid(mnemonic_from_text("movv")),
          "un mnemonico inexistente no se inventa");
    exige(!is_valid(mnemonic_from_text("")), "la cadena vacia no es instruccion");
    exige(!is_valid(mnemonic_from_text(nullptr)), "un puntero nulo no revienta");

    /* Ida y vuelta sobre TODAS.  Es la comprobacion que descubre una entrada
     * REPETIDA en la lista: dos mnemonicos con el mismo texto compilan sin
     * queja, y uno de los dos se queda sin poder alcanzarse nunca. */
    for (uint16_t i = 0; i < mnemonic_count(); ++i) {
        const auto m = static_cast<Mnemonic>(i);
        if (mnemonic_from_text(text_of(m)) == m) continue;
        std::printf("  FALLA    '%s' no vuelve a su mnemonico (texto repetido?)\n",
                    text_of(m));
        ++fallos;
    }

    std::printf("[mnemonicos] %s\n", fallos == 0 ? "TODO OK" : "CON FALLOS");
    return fallos == 0 ? 0 : 1;
}
