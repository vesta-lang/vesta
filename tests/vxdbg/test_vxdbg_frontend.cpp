/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file test_vxdbg_frontend.cpp
 * @brief De un fuente Vesta de verdad al grafo semantico.
 *
 * Los demas tests del subsistema construyen el grafo a mano.  Eso comprueba que
 * las piezas encajan, pero no que lo que el compilador SABE quepa en el modelo:
 * un grafo escrito para pasar el test siempre cabe.  Aqui se compila codigo
 * Vesta y se comprueba lo que salio.
 */

#include "vx/type_checker.h"
#include "vx/vxdbg_emit.h"
#include "vxdbg/codec.h"
#include "vxdbg/store.h"

#include "vx/lexer.h"
#include "vx/parser.h"

#include <cstdio>
#include <filesystem>
#include <string>

static int fallos = 0;

/**
 * @brief Comprueba una condicion y deja constancia.
 * @param cond Lo que debe cumplirse.
 * @param que Descripcion.
 */
static void comprobar(bool cond, const char *que) {
    if (cond) {
        std::printf("  OK   %s\n", que);
    } else {
        std::printf("  FALLA %s\n", que);
        ++fallos;
    }
}

/// Un programa con las formas que Vesta distingue de verdad.
static const char *FUENTE = R"(
interface Cerrable {
    void cerrar();
}

class Flujo {
    i64 pos;
    i64 leer() { return this.pos; }
}

class Lector : Flujo, Cerrable {
    i64 total;
    void cerrar() { this.total = 0; }
}

struct Punto {
    f64 x;
    f64 y;
}

union Palabra {
    u32 entero;
    f32 real;
}

enum Color : u8 {
    Rojo = 1,
    Verde = 2,
}

i32 main() {
    return 0;
}
)";

/**
 * @brief Busca una entidad emitida por su clave.
 * @param stats Lo que reporto el emisor.
 * @param clave Clave del compilador.
 * @return Su identificador, o uno vacio.
 */
static vxdbg::LanguageEntityId buscar(const vx::VxdbgEmitStats &stats,
                                      const std::string &clave) {
    for (const auto &r : stats.roots)
        if (r.first == clave) return r.second;
    return {};
}

/**
 * @brief Lee una entidad del almacen.
 * @param store Almacen.
 * @param id Identificador.
 * @param out Recibe la entidad.
 * @return @c true si estaba.
 */
static bool leer(const vxdbg::NodeStore &store, vxdbg::LanguageEntityId id,
                 vxdbg::LanguageEntity &out) {
    if (id.hash.empty()) return false;
    return vxdbg::load_node(store, id.hash, out);
}

int main() {
    std::printf("=== vxdbg: del fuente Vesta al grafo semantico ===\n");

    // Carpeta propia para no mezclarse con el cache real del compilador.
    const std::string dir =
        (std::filesystem::temp_directory_path() / "vxdbg_frontend_test")
            .string();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    vx::Diagnostics diags;
    vx::Lexer lex(FUENTE, "prueba.vx", diags);
    vx::Parser parser(lex, diags);
    auto mod = parser.parse_program();
    if (!mod || diags.has_errors()) {
        std::printf("  FALLA el fuente de prueba no compila\n");
        return 1;
    }
    vx::TypeChecker tc(*mod, diags);
    if (!tc.run()) {
        std::printf("  FALLA el fuente de prueba no pasa el checker\n");
        for (const auto &d : diags.all()) std::printf("    %s\n", d.message.c_str());
        return 1;
    }

    vx::VxdbgEmitStats stats;
    std::string err;
    comprobar(vx::emit_vxdbg_source(tc, "prueba.vx", FUENTE, dir, stats, err),
              "se emite el grafo");
    comprobar(stats.entities > 0, "y no sale vacio");

    vxdbg::FileNodeStore store(dir);

    std::printf("Los tipos, con la especie que les corresponde\n");
    vxdbg::LanguageEntity e;
    comprobar(leer(store, buscar(stats, "Lector"), e), "la clase esta");
    comprobar(e.kind == vxdbg::EntityKind::Type &&
                  e.lang_kind == "class",
              "  es un tipo, y Vesta lo llama clase");
    // Lo que pedia el usuario ver en un fallo: de quien deriva y que cumple.
    size_t deriva = 0, cumple = 0;
    for (const auto &r : e.relations) {
        if (r.kind == vxdbg::RelationKind::Derives) ++deriva;
        if (r.kind == vxdbg::RelationKind::Implements) ++cumple;
    }
    comprobar(deriva == 1, "  deriva de una");
    comprobar(cumple == 1, "  y cumple un contrato");

    comprobar(leer(store, buscar(stats, "Cerrable"), e), "la interfaz esta");
    comprobar(e.kind == vxdbg::EntityKind::Contract,
              "  y es un contrato, no un tipo: se cumple, no se instancia");

    comprobar(leer(store, buscar(stats, "Punto"), e), "el struct esta");
    comprobar(e.kind == vxdbg::EntityKind::Type && e.lang_kind == "struct",
              "  con su genero");
    comprobar(e.byte_size == 16, "  y su tamano de verdad, no uno inventado");
    comprobar(e.alignment == 8, "  con su alineamiento");

    comprobar(leer(store, buscar(stats, "Palabra"), e), "la union esta");
    comprobar(e.lang_kind == "union", "  distinguida de un struct normal");

    comprobar(leer(store, buscar(stats, "Color"), e), "el enum esta");
    comprobar(e.kind == vxdbg::EntityKind::Enumeration, "  como enumeracion");
    comprobar(e.lang_kind == "valued enum",
              "  sabiendo que sus casos SON valores de otro tipo");

    std::printf("Nada se inventa\n");
    comprobar(buscar(stats, "NoExiste").hash.empty(),
              "un tipo que no existe no aparece");
    comprobar(stats.unresolved == 0,
              "y no queda ninguna relacion sin destino");

    std::printf("Se puede volver a emitir\n");
    // Emitir dos veces lo mismo tiene que dar exactamente los mismos nodos: es
    // la propiedad que hace incremental al sistema, y si se rompiera aqui el
    // cache creceria sin parar sin que nadie lo notara.
    vx::VxdbgEmitStats stats2;
    comprobar(vx::emit_vxdbg_source(tc, "prueba.vx", FUENTE, dir, stats2, err),
              "la segunda vez tambien va");
    comprobar(stats2.roots.size() == stats.roots.size(),
              "con los mismos tipos");
    bool iguales = true;
    for (size_t i = 0; i < stats.roots.size() && iguales; ++i)
        iguales = (stats.roots[i] == stats2.roots[i]);
    comprobar(iguales, "y exactamente las mismas huellas");

    std::filesystem::remove_all(dir, ec);

    if (fallos == 0) {
        std::printf("=== todo correcto ===\n");
        return 0;
    }
    std::printf("=== %d comprobaciones fallidas ===\n", fallos);
    return 1;
}
