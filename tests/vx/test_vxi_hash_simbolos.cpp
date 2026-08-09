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
 * @file test_vxi_hash_simbolos.cpp
 * @brief Comprueba @ref vx::vxi_hash_de_simbolos: la huella de lo que un
 *        consumidor VE de un modulo importado.
 *
 * De ella depende si se reutiliza lo ya compilado de quien importa, y tiene dos
 * formas de fallar: si cambia cuando no debe, se recompila de balde; si NO
 * cambia cuando debe, se sirve un artefacto compilado contra otra version.  Lo
 * segundo es lo que hay que cazar, y por eso la mitad de los casos son de
 * "esto TIENE que cambiar la huella".
 */

#include "vx/module/vxi_format.h"

#include <iostream>
#include <string>
#include <vector>

using namespace vx;

static int fallos = 0;

/// @brief Comprueba una condicion y deja constancia del caso.
static void comprobar(bool ok, const std::string &caso) {
    if (ok) {
        std::cout << "  ok   " << caso << "\n";
    } else {
        std::cout << "  FALLA " << caso << "\n";
        ++fallos;
    }
}

/// @brief Una funcion publica con su firma.
static VxiSymbol funcion(const std::string &nombre, const std::string &ret,
                         const std::vector<std::string> &params) {
    VxiSymbol s;
    s.kind = VxiSymbolKind::FUNCTION;
    s.name = nombre;
    s.return_type = ret;
    s.param_types = params;
    for (size_t i = 0; i < params.size(); ++i)
        s.param_names.push_back("p" + std::to_string(i));
    return s;
}

/// @brief Un struct con sus campos, offsets incluidos.
static VxiSymbol estructura(const std::string &nombre,
                            const std::vector<std::string> &tipos) {
    VxiSymbol s;
    s.kind = VxiSymbolKind::STRUCT;
    s.name = nombre;
    uint32_t off = 0;
    for (size_t i = 0; i < tipos.size(); ++i) {
        VxiSymbol::FieldInfo f;
        f.name = "c" + std::to_string(i);
        f.type_str = tipos[i];
        f.offset = off;
        f.size = 8;
        off += 8;
        s.fields.push_back(f);
    }
    s.size_bytes = off;
    s.align_bytes = 8;
    return s;
}

int main() {
    std::cout << "== vxi_hash_de_simbolos ==\n";

    VxiModule base;
    base.symbols.push_back(funcion("calc", "i64", {"i64"}));
    base.symbols.push_back(estructura("Punto", {"i64", "i64"}));

    const std::vector<std::string> solo_calc = {"calc"};
    const uint64_t h_calc = vxi_hash_de_simbolos(base, solo_calc);

    // -- Lo que NO debe cambiar la huella de quien solo usa `calc` -----------
    {
        VxiModule m = base;
        m.symbols.push_back(funcion("extra", "i64", {"i64"}));
        comprobar(vxi_hash_de_simbolos(m, solo_calc) == h_calc,
                  "anadir una funcion publica que no se usa");
    }
    {
        VxiModule m = base;
        // Cambia OTRO simbolo del modulo, no el que se usa.
        m.symbols[1] = estructura("Punto", {"i64", "i64", "i64"});
        comprobar(vxi_hash_de_simbolos(m, solo_calc) == h_calc,
                  "cambiar un tipo que no se usa");
    }
    {
        // El orden en que se pidan los simbolos no puede importar.
        const std::vector<std::string> a = {"calc", "Punto"};
        const std::vector<std::string> b = {"Punto", "calc"};
        comprobar(vxi_hash_de_simbolos(base, a) ==
                      vxi_hash_de_simbolos(base, b),
                  "el orden de los nombres no importa");
        const std::vector<std::string> c = {"calc", "calc", "Punto"};
        comprobar(vxi_hash_de_simbolos(base, c) ==
                      vxi_hash_de_simbolos(base, a),
                  "repetir un nombre no importa");
    }

    // -- Lo que SI debe cambiarla -------------------------------------------
    {
        VxiModule m = base;
        m.symbols[0] = funcion("calc", "i64", {"i64", "i64"});
        comprobar(vxi_hash_de_simbolos(m, solo_calc) != h_calc,
                  "la funcion usada gana un parametro");
    }
    {
        VxiModule m = base;
        m.symbols[0] = funcion("calc", "i32", {"i64"});
        comprobar(vxi_hash_de_simbolos(m, solo_calc) != h_calc,
                  "cambia su tipo de retorno");
    }
    {
        VxiModule m = base;
        m.symbols[0] = funcion("calc", "i64", {"u64"});
        comprobar(vxi_hash_de_simbolos(m, solo_calc) != h_calc,
                  "cambia el tipo de un parametro");
    }
    {
        VxiModule m;
        m.symbols.push_back(base.symbols[1]); // se queda sin `calc`
        comprobar(vxi_hash_de_simbolos(m, solo_calc) != h_calc,
                  "desaparece el simbolo que se usaba");
    }
    {
        // El LAYOUT de un tipo usado: quien lo usa reserva y lee con el.
        const std::vector<std::string> solo_punto = {"Punto"};
        const uint64_t h = vxi_hash_de_simbolos(base, solo_punto);
        VxiModule m = base;
        m.symbols[1] = estructura("Punto", {"i64", "i64", "i64"});
        comprobar(vxi_hash_de_simbolos(m, solo_punto) != h,
                  "el tipo usado gana un campo");
        VxiModule m2 = base;
        m2.symbols[1] = estructura("Punto", {"i64", "f64"});
        comprobar(vxi_hash_de_simbolos(m2, solo_punto) != h,
                  "cambia el tipo de un campo");
    }
    {
        // Dos consumidores que usan cosas distintas ven huellas distintas.
        comprobar(vxi_hash_de_simbolos(base, {"calc"}) !=
                      vxi_hash_de_simbolos(base, {"Punto"}),
                  "usar `calc` no es lo mismo que usar `Punto`");
    }
    {
        // Pedir nada es estable y no coincide con pedir algo.
        const std::vector<std::string> nada;
        comprobar(vxi_hash_de_simbolos(base, nada) ==
                      vxi_hash_de_simbolos(base, nada),
                  "pedir nada es estable");
        comprobar(vxi_hash_de_simbolos(base, nada) != h_calc,
                  "pedir nada no es pedir `calc`");
    }

    std::cout << (fallos == 0 ? "TODO OK\n" : "HAY FALLOS\n");
    return fallos == 0 ? 0 : 1;
}
