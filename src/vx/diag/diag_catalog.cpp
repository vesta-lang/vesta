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
 * @file diag_catalog.cpp
 * @brief Seleccion de idioma y formateo del catalogo de diagnosticos.  La TABLA
 *        de mensajes vive en el fichero generado diag_catalog_gen.cpp.
 */

#include "vx/diag_catalog.h"

#include <cctype>
#include <cstdlib>

namespace vx {
namespace diag {

namespace {
/// Idioma activo global (indice en el catalogo).  0 = primer idioma (fallback).
int g_current = 0;

/// Prefijo de idioma de una locale: "es_ES.UTF-8" -> "es"; minusculas.
std::string lang_prefix(const std::string &s) {
    std::string out;
    for (char c : s) {
        if (c == '_' || c == '.' || c == '-' || c == '@' || c == ' ')
            break;
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}
} // namespace

int language_count() {
    int n = 0;
    catalog_languages(&n);
    return n;
}

const char *language_code(int idx) {
    int n = 0;
    const char *const *langs = catalog_languages(&n);
    if (idx < 0 || idx >= n)
        return "";
    return langs[idx];
}

int language_index(const std::string &code) {
    const std::string want = lang_prefix(code);
    if (want.empty())
        return -1;
    int n = 0;
    const char *const *langs = catalog_languages(&n);
    for (int i = 0; i < n; ++i)
        if (want == langs[i])
            return i;
    return -1;
}

int language_from_env() {
    // Orden: VESTA_LANG (override explicito del compilador) > LC_ALL > LANG.
    const char *vars[] = {"VESTA_LANG", "LC_ALL", "LANG"};
    for (const char *v : vars) {
        const char *val = std::getenv(v);
        if (val && *val) {
            int idx = language_index(val);
            if (idx >= 0)
                return idx;
        }
    }
    return 0; // fallback: primer idioma del catalogo.
}

void set_language(int idx) {
    if (idx >= 0 && idx < language_count())
        g_current = idx;
}

int current_language() { return g_current; }

bool has_code(const std::string &code) {
    // Existe si tiene plantilla en ALGUN idioma (probamos el 0, que es el
    // completo por convencion; si faltara, probamos los demas).
    if (catalog_template(code.c_str(), 0))
        return true;
    for (int i = 1; i < language_count(); ++i)
        if (catalog_template(code.c_str(), i))
            return true;
    return false;
}

std::string format(const std::string &code, int lang,
                   const std::vector<std::string> &args) {
    const char *tmpl = catalog_template(code.c_str(), lang);
    if (!tmpl || !*tmpl)
        tmpl = catalog_template(code.c_str(), 0); // fallback al idioma 0.
    if (!tmpl)
        return code; // codigo desconocido -> el propio codigo.

    // Sustituir los placeholders posicionales {N} por args[N].  Un '{' que no
    // abre un indice valido se copia literal (no rompe con texto con llaves).
    std::string out;
    for (const char *p = tmpl; *p;) {
        if (*p == '{' && std::isdigit(static_cast<unsigned char>(p[1]))) {
            int idx = 0;
            const char *q = p + 1;
            while (std::isdigit(static_cast<unsigned char>(*q))) {
                idx = idx * 10 + (*q - '0');
                ++q;
            }
            if (*q == '}') {
                if (idx >= 0 && idx < static_cast<int>(args.size()))
                    out += args[idx];
                p = q + 1;
                continue;
            }
        }
        out += *p++;
    }
    return out;
}

std::string format(const std::string &code,
                   const std::vector<std::string> &args) {
    return format(code, g_current, args);
}

} // namespace diag
} // namespace vx
