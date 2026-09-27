/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/vx/test_macro_report.cpp
 * @brief Comprueba que el motivo por el que un `@Macro` no se bajo se escribe
 *        del catalogo, en el idioma activo, y con la auxiliar por la que se
 *        llego a el.
 *
 * El motivo era una frase en espanol escrita a mano que llegaba tal cual al
 * editor.  Ahora es un dato (codigo + nombre) y la frase se escribe al
 * mostrarla: lo que se comprueba es justo eso -- que el mismo dato da la
 * frase de cada idioma, con el nombre dentro, y que la auxiliar la envuelve.
 */
#include "vx/comptime/macro_report.h"
#include "vx/diag/diag_catalog.h"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0; ///< Cuantas comprobaciones han fallado.

/**
 * @brief Comprueba que @p text contiene @p piece.
 * @param text  El texto obtenido.
 * @param piece Lo que tiene que aparecer.
 * @param what  Que se estaba comprobando.
 */
void check_contains(const std::string &text, const std::string &piece,
                    const char *what) {
    if (text.find(piece) == std::string::npos) {
        std::printf("FALLO: %s: '%s' no contiene '%s'\n", what, text.c_str(),
                    piece.c_str());
        ++g_failures;
    }
}

/**
 * @brief Un motivo con su codigo y su nombre.
 * @param code    Codigo del catalogo.
 * @param subject El nombre al que se refiere.
 * @return El motivo.
 */
vx::MacroSkipReason reason(const char *code, const char *subject) {
    vx::MacroSkipReason r;
    r.code = code;
    r.subject = util::InternedName::intern(subject);
    return r;
}

} // namespace

/**
 * @brief Punto de entrada del test.
 * @return 0 si todo paso, 1 si hubo algun fallo.
 */
int main() {
    const int en = vx::diag::language_index("en");
    const int es = vx::diag::language_index("es");

    // Un motivo vacio es "se puede bajar".
    const vx::MacroSkipReason none;
    if (!none.empty()) {
        std::printf("FALLO: un motivo sin codigo no esta vacio\n");
        ++g_failures;
    }

    // El mismo dato, en los dos idiomas, con el nombre dentro.
    const vx::MacroSkipReason builtin = reason("VXT125", "comptime_compile");
    vx::diag::set_language(en);
    const std::string text_en = vx::macro_skip_text(builtin);
    check_contains(text_en, "comptime_compile", "el nombre, en ingles");
    check_contains(text_en, "compile-time-only", "la frase inglesa");
    vx::diag::set_language(es);
    const std::string text_es = vx::macro_skip_text(builtin);
    check_contains(text_es, "comptime_compile", "el nombre, en espanol");
    check_contains(text_es, "solo existe al compilar", "la frase espanola");

    // Un motivo sin nombre no deja un hueco `{0}` a la vista.
    vx::diag::set_language(en);
    vx::MacroSkipReason nested;
    nested.code = "VXT129";
    const std::string plain = vx::macro_skip_text(nested);
    if (plain.find('{') != std::string::npos) {
        std::printf("FALLO: queda un hueco sin rellenar: '%s'\n",
                    plain.c_str());
        ++g_failures;
    }

    // Llegar por una auxiliar la nombra y conserva el motivo de dentro.
    vx::MacroSkipReason via = reason("VXT122", "GREETING");
    via.via = util::InternedName::intern("build_text");
    const std::string wrapped = vx::macro_skip_text(via);
    check_contains(wrapped, "build_text", "la auxiliar");
    check_contains(wrapped, "GREETING", "el motivo de dentro");

    if (g_failures == 0) std::printf("test_macro_report: OK\n");
    return g_failures == 0 ? 0 : 1;
}
