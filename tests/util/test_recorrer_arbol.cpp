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
 * @file test_recorrer_arbol.cpp
 * @brief Comprueba @c fs::recorrer_arbol: que ve todo lo que hay, que dice bien
 *        quien es directorio, y que se puede podar una rama.
 *
 * Existe porque el recorrido reemplaza a `recursive_directory_iterator` por
 * velocidad (46 ms contra 1,7 sobre el mismo arbol) y un recorrido que se deja
 * ficheros no falla de golpe: falla mas tarde, diciendo que un simbolo no
 * existe.
 */

#include "util/fs_utils.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace stdfs = std::filesystem;

static int fallos = 0;

/// @brief Comprueba una condicion y deja constancia del nombre del caso.
static void comprobar(bool ok, const std::string &caso) {
    if (ok) {
        std::cout << "  ok   " << caso << "\n";
    } else {
        std::cout << "  FALLA " << caso << "\n";
        ++fallos;
    }
}

/// @brief Crea un fichero vacio, creando antes su directorio si hace falta.
static void crear_fichero(const stdfs::path &p) {
    stdfs::create_directories(p.parent_path());
    std::ofstream f(p.string(), std::ios::binary);
}

/// @brief Ultimo componente de una ruta con separadores '/'.
static std::string nombre_de(const std::string &ruta) {
    const size_t barra = ruta.find_last_of('/');
    return barra == std::string::npos ? ruta : ruta.substr(barra + 1);
}

int main() {
    std::cout << "== recorrer_arbol ==\n";

    // Arbol de prueba, en un directorio propio para no depender del entorno.
    const stdfs::path raiz =
        stdfs::temp_directory_path() / "vesta_test_recorrer_arbol";
    stdfs::remove_all(raiz);
    crear_fichero(raiz / "a.vx");
    crear_fichero(raiz / "b.txt");
    crear_fichero(raiz / "sub" / "c.vx");
    crear_fichero(raiz / "sub" / "hondo" / "d.vx");
    crear_fichero(raiz / ".oculto" / "no_mirar.vx");

    // -- Caso 1: recorrido completo, bajando a todo ---------------------------
    {
        std::set<std::string> vistos;
        std::vector<std::string> dirs;
        fs::recorrer_arbol(raiz.string(),
                           [&](const std::string &ruta, bool es_dir) {
                               vistos.insert(nombre_de(ruta));
                               if (es_dir) dirs.push_back(nombre_de(ruta));
                               return true; // bajar a todo
                           });
        comprobar(vistos.count("a.vx") == 1, "ve un fichero de la raiz");
        comprobar(vistos.count("b.txt") == 1, "no filtra por extension");
        comprobar(vistos.count("c.vx") == 1, "baja un nivel");
        comprobar(vistos.count("d.vx") == 1, "baja dos niveles");
        comprobar(vistos.count("no_mirar.vx") == 1,
                  "un directorio oculto se ve si se pide bajar");
        comprobar(vistos.count(".") == 0 && vistos.count("..") == 0,
                  "nunca entrega `.` ni `..`");
        std::sort(dirs.begin(), dirs.end());
        const std::vector<std::string> esperados = {".oculto", "hondo", "sub"};
        comprobar(dirs == esperados, "distingue los directorios de los ficheros");
    }

    // -- Caso 2: podar una rama devolviendo false -----------------------------
    {
        std::set<std::string> vistos;
        fs::recorrer_arbol(raiz.string(),
                           [&](const std::string &ruta, bool es_dir) {
                               vistos.insert(nombre_de(ruta));
                               // Misma convencion que el indice de namespaces:
                               // no bajar a los directorios ocultos.
                               if (es_dir) return nombre_de(ruta)[0] != '.';
                               return false;
                           });
        comprobar(vistos.count(".oculto") == 1,
                  "el directorio podado SI se entrega");
        comprobar(vistos.count("no_mirar.vx") == 0,
                  "lo que hay dentro del podado no se visita");
        comprobar(vistos.count("d.vx") == 1, "podar una rama no afecta a otra");
    }

    // -- Caso 3: raiz que no existe ------------------------------------------
    {
        long llamadas = 0;
        fs::recorrer_arbol((raiz / "no_existe").string(),
                           [&](const std::string &, bool) {
                               ++llamadas;
                               return true;
                           });
        comprobar(llamadas == 0, "una raiz inexistente no entrega nada");
    }

    // -- Caso 4: la ruta entregada sirve para abrir el fichero ---------------
    {
        bool abrio = false;
        fs::recorrer_arbol(raiz.string(),
                           [&](const std::string &ruta, bool es_dir) {
                               if (!es_dir && nombre_de(ruta) == "d.vx") {
                                   std::ifstream f(ruta, std::ios::binary);
                                   abrio = f.is_open();
                               }
                               return true;
                           });
        comprobar(abrio, "la ruta entregada abre el fichero");
    }

    stdfs::remove_all(raiz);
    std::cout << (fallos == 0 ? "TODO OK\n" : "HAY FALLOS\n");
    return fallos == 0 ? 0 : 1;
}
