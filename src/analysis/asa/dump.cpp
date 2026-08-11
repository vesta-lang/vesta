/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/asa/dump.cpp
 * @brief La vista del almacen de hechos (ver @c analysis/asa/dump.h).
 *
 * Ni calcula ni decide: ordena, filtra y escribe.  Todo lo que sale de aqui ya
 * estaba en el almacen, puesto por el productor de su dominio.
 */

#include "analysis/asa/dump.h"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace analysis {
namespace asa {

namespace {

/// Nombre corto: `asa.rangos` se ensena como `rangos`.
const char *corto(const char *s) {
    const char *punto = std::strrchr(s, '.');
    return punto != nullptr ? punto + 1 : s;
}

/// Como se nombra al sujeto de un hecho.
std::string texto_sujeto(const Sujeto &s) {
    std::ostringstream o;
    if (s.funcion == nullptr || s.funcion[0] == '\0') return "<modulo>";
    o << s.funcion;
    switch (s.clase) {
    case Sujeto::Clase::Valor: o << ":v" << s.id; break;
    case Sujeto::Clase::Bloque: o << ":b" << s.id; break;
    case Sujeto::Clase::Instruccion: o << ":i" << s.id; break;
    default: break;
    }
    return o.str();
}

/// Orden estable: por sujeto y, dentro, por dominio.  Dos volcados del mismo
/// programa tienen que poder compararse linea a linea.
std::vector<FactId> ordenar(const FactStore &a, const OpcionesVista &vista) {
    std::vector<FactId> ids;
    ids.reserve(a.size());
    for (FactId i = 0; i < a.size(); ++i) {
        if (!vista.desconocidos &&
            a.at(i).sello.certeza == Certeza::Desconocida)
            continue;
        ids.push_back(i);
    }
    std::stable_sort(ids.begin(), ids.end(), [&a](FactId x, FactId y) {
        const std::string sx = texto_sujeto(a.at(x).de_quien);
        const std::string sy = texto_sujeto(a.at(y).de_quien);
        if (sx != sy) return sx < sy;
        return std::strcmp(a.at(x).que.dominio, a.at(y).que.dominio) < 0;
    });
    return ids;
}

void escribir_hecho(const FactStore &a, FactId id, FILE *salida,
                    const OpcionesVista &vista) {
    const Fact &f = a.at(id);
    std::fprintf(salida, "      %-12s %-24s", corto(f.que.dominio),
                 corto(f.que.codigo));
    if (f.que.detalle != nullptr && f.que.detalle[0] != '\0')
        std::fprintf(salida, " %s", f.que.detalle);
    /* Certeza, fuente y regla, en ese orden y siempre.  La certeza es lo que
     * decide al consumidor; la fuente y la regla son para entenderlo.  Cuando
     * entren los hechos observados en ejecucion o medidos en corridas
     * anteriores, apareceran aqui al lado de los estaticos, con su certeza
     * propia y sin que la vista tenga que cambiar. */
    std::fprintf(salida, "   [%s, de %s", nombre_certeza(f.sello.certeza),
                 nombre_fuente(f.sello.origen.fuente));
    if (f.prueba.regla != nullptr && f.prueba.regla[0] != '\0')
        std::fprintf(salida, ", por %s", f.prueba.regla);
    std::fprintf(salida, "]\n");
    if (!vista.pruebas || f.prueba.de.empty()) return;
    /* La derivacion: de que hechos CONCRETOS se sigue este.  Es lo que convierte
     * "confia en mi" en algo comprobable. */
    for (FactId d : a.explicar(id)) {
        if (d == id) continue;
        const Fact &o = a.at(d);
        std::fprintf(salida, "          <- %s %s %s\n", texto_sujeto(o.de_quien).c_str(),
                     corto(o.que.codigo),
                     o.que.detalle != nullptr ? o.que.detalle : "");
    }
}

} // namespace

void imprimir_volcado(const FactStore                      &almacen,
                      const std::vector<ResumenProduccion> &resumenes,
                      const OpcionesVista &vista, FILE *salida) {
    std::fprintf(salida, "Lo que se sabe del programa (ASA)\n");
    std::fprintf(salida,
                 "Cada linea: dominio | que se afirma | detalle | cuanto "
                 "fiarse.\n");
    std::fprintf(salida, "%s\n", std::string(78, '=').c_str());

    const std::vector<FactId> ids = ordenar(almacen, vista);
    std::string ultimo;
    for (FactId id : ids) {
        const std::string s = texto_sujeto(almacen.at(id).de_quien);
        if (s != ultimo) {
            std::fprintf(salida, "\n  %s\n", s.c_str());
            ultimo = s;
        }
        escribir_hecho(almacen, id, salida, vista);
    }

    std::fprintf(salida, "\n%s\n", std::string(78, '=').c_str());
    std::fprintf(salida, "Por dominio:\n");
    for (const ResumenProduccion &r : resumenes) {
        std::fprintf(salida,
                     "  %-12s %6u hechos de %6u miradas (%u sin sacar nada), "
                     "%ld us\n",
                     corto(r.dominio), r.hechos, r.miradas, r.callados,
                     r.micros);
        /* El POR QUE de lo que no se supo, siempre.  Un dominio que se calla sin
         * motivo no se puede arreglar: no se distingue "puede valer cualquier
         * cosa" de "no me dio tiempo" ni de "no lo mire". */
        for (const MotivoIgnorancia &m : r.motivos)
            std::fprintf(salida, "                 no supo %6u por %s\n",
                         m.veces, corto(m.codigo));
    }
    /* Y de que fuente viene lo que se sabe.  Hoy todo es estatico; el dia que
     * entren la observacion en ejecucion y el perfil, se veran aqui repartidos
     * -- que es la prueba de que entraron como una fuente mas y no como otro
     * sistema. */
    uint32_t por_fuente[4] = {0, 0, 0, 0};
    for (FactId id : ids) {
        const uint8_t f =
            static_cast<uint8_t>(almacen.at(id).sello.origen.fuente);
        if (f < 4) ++por_fuente[f];
    }
    std::fprintf(salida, "Por fuente: estatico=%u ejecucion=%u perfil=%u "
                         "declarado=%u\n",
                 por_fuente[0], por_fuente[1], por_fuente[2], por_fuente[3]);

    const FactStore::Recuento c = almacen.recuento();
    std::fprintf(salida,
                 "\nEn total %zu hechos: %u demostrados, %u inferidos, %u sin "
                 "certeza.\n",
                 almacen.size(), c.demostradas, c.inferidas, c.desconocidas);
    std::fprintf(salida,
                 "Lo que se miro sin sacar nada es donde hay sitio para saber "
                 "mas.\n");
}

namespace {
/// Escape minimo de JSON.  No se usa la libreria para no arrastrarla a la capa
/// de analisis por seis campos de texto.
void json_cadena(std::ostringstream &o, const char *s) {
    o << '"';
    for (const char *p = s; p != nullptr && *p != '\0'; ++p) {
        switch (*p) {
        case '"': o << "\\\""; break;
        case '\\': o << "\\\\"; break;
        case '\n': o << "\\n"; break;
        case '\t': o << "\\t"; break;
        default:
            if (static_cast<unsigned char>(*p) < 0x20) o << ' ';
            else o << *p;
        }
    }
    o << '"';
}
void json_cadena(std::ostringstream &o, const std::string &s) {
    json_cadena(o, s.c_str());
}
} // namespace

std::string volcado_json(const FactStore                      &almacen,
                         const std::vector<ResumenProduccion> &resumenes,
                         const OpcionesVista                  &vista) {
    std::ostringstream o;
    o << "{\"hechos\":[";
    bool primero = true;
    for (FactId id : ordenar(almacen, vista)) {
        const Fact &f = almacen.at(id);
        if (!primero) o << ",";
        primero = false;
        o << "{\"id\":" << id << ",\"dominio\":";
        json_cadena(o, corto(f.que.dominio));
        o << ",\"sujeto\":";
        json_cadena(o, texto_sujeto(f.de_quien));
        o << ",\"clase\":";
        json_cadena(o, nombre_clase_sujeto(f.de_quien.clase));
        o << ",\"codigo\":";
        json_cadena(o, f.que.codigo);
        o << ",\"a\":" << f.que.a << ",\"b\":" << f.que.b << ",\"detalle\":";
        json_cadena(o, f.que.detalle);
        o << ",\"certeza\":";
        json_cadena(o, nombre_certeza(f.sello.certeza));
        o << ",\"fuente\":";
        json_cadena(o, nombre_fuente(f.sello.origen.fuente));
        o << ",\"regla\":";
        json_cadena(o, f.prueba.regla);
        o << ",\"de\":[";
        bool p2 = true;
        for (FactId d : f.prueba.de) {
            if (!p2) o << ",";
            p2 = false;
            o << d;
        }
        o << "]}";
    }
    o << "],\"dominios\":[";
    primero = true;
    for (const ResumenProduccion &r : resumenes) {
        if (!primero) o << ",";
        primero = false;
        o << "{\"dominio\":";
        json_cadena(o, corto(r.dominio));
        o << ",\"hechos\":" << r.hechos << ",\"miradas\":" << r.miradas
          << ",\"callados\":" << r.callados << ",\"micros\":" << r.micros << "}";
    }
    const FactStore::Recuento c = almacen.recuento();
    o << "],\"certeza\":{\"demostradas\":" << c.demostradas
      << ",\"inferidas\":" << c.inferidas
      << ",\"desconocidas\":" << c.desconocidas << "}}";
    return o.str();
}

} // namespace asa
} // namespace analysis
