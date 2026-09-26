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
 * @file test_walk_tree.cpp
 * @brief Comprueba @c fs::walk_tree: que ve todo lo que hay, que dice bien
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

namespace {

int g_failures = 0; ///< Cuantas comprobaciones han fallado.

/**
 * @brief Comprueba una condicion y deja constancia del nombre del caso.
 * @param ok   Lo que tiene que ser cierto.
 * @param what El caso.
 */
void check(bool ok, const std::string &what) {
    if (ok) {
        std::cout << "  ok   " << what << "\n";
    } else {
        std::cout << "  FALLA " << what << "\n";
        ++g_failures;
    }
}

/**
 * @brief Crea un fichero vacio, creando antes su directorio si hace falta.
 * @param p Ruta del fichero.
 */
void create_file(const stdfs::path &p) {
    stdfs::create_directories(p.parent_path());
    std::ofstream f(p.string(), std::ios::binary);
}

/**
 * @brief Ultimo componente de una ruta con separadores '/'.
 * @param path La ruta.
 * @return Su ultimo componente.
 */
std::string last_component(const std::string &path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

/// Apunta todo lo que ve y baja a todo.
struct CollectAll {
    std::set<std::string> *seen;      ///< nombres vistos
    std::vector<std::string> *dirs;   ///< de ellos, los directorios
    /// @brief Visita una entrada.  @return Siempre bajar.
    bool operator()(const std::string &path, bool is_dir) const {
        seen->insert(last_component(path));
        if (is_dir) dirs->push_back(last_component(path));
        return true;
    }
};

/// Apunta lo que ve y no baja a los directorios ocultos.
struct SkipHidden {
    std::set<std::string> *seen; ///< nombres vistos
    /// @brief Visita una entrada.  @return Bajar si no es oculto.
    bool operator()(const std::string &path, bool is_dir) const {
        seen->insert(last_component(path));
        // Misma convencion que el indice de namespaces: no bajar a los
        // directorios ocultos.
        if (is_dir) return last_component(path)[0] != '.';
        return false;
    }
};

/// Solo cuenta las llamadas.
struct CountCalls {
    long *calls; ///< cuantas
    /// @brief Visita una entrada.  @return Siempre bajar.
    bool operator()(const std::string &, bool) const {
        ++*calls;
        return true;
    }
};

/// Intenta abrir `d.vx` con la ruta que entrega el recorrido.
struct OpenDeep {
    bool *opened; ///< si se pudo abrir
    /// @brief Visita una entrada.  @return Siempre bajar.
    bool operator()(const std::string &path, bool is_dir) const {
        if (!is_dir && last_component(path) == "d.vx") {
            std::ifstream f(path, std::ios::binary);
            *opened = f.is_open();
        }
        return true;
    }
};

} // namespace

/**
 * @brief Punto de entrada del test.
 * @return 0 si todo paso, 1 si hubo algun fallo.
 */
int main() {
    std::cout << "== walk_tree ==\n";

    // Arbol de prueba, en un directorio propio para no depender del entorno.
    const stdfs::path root =
        stdfs::temp_directory_path() / "vesta_test_walk_tree";
    stdfs::remove_all(root);
    create_file(root / "a.vx");
    create_file(root / "b.txt");
    create_file(root / "sub" / "c.vx");
    create_file(root / "sub" / "hondo" / "d.vx");
    create_file(root / ".oculto" / "no_mirar.vx");

    // -- Caso 1: recorrido completo, bajando a todo ---------------------------
    {
        std::set<std::string> seen;
        std::vector<std::string> dirs;
        fs::walk_tree(root.string(), CollectAll{&seen, &dirs});
        check(seen.count("a.vx") == 1, "ve un fichero de la raiz");
        check(seen.count("b.txt") == 1, "no filtra por extension");
        check(seen.count("c.vx") == 1, "baja un nivel");
        check(seen.count("d.vx") == 1, "baja dos niveles");
        check(seen.count("no_mirar.vx") == 1,
              "un directorio oculto se ve si se pide bajar");
        check(seen.count(".") == 0 && seen.count("..") == 0,
              "nunca entrega `.` ni `..`");
        std::sort(dirs.begin(), dirs.end());
        const std::vector<std::string> expected = {".oculto", "hondo", "sub"};
        check(dirs == expected, "distingue los directorios de los ficheros");
    }

    // -- Caso 2: podar una rama devolviendo false -----------------------------
    {
        std::set<std::string> seen;
        fs::walk_tree(root.string(), SkipHidden{&seen});
        check(seen.count(".oculto") == 1, "el directorio podado SI se entrega");
        check(seen.count("no_mirar.vx") == 0,
              "lo que hay dentro del podado no se visita");
        check(seen.count("d.vx") == 1, "podar una rama no afecta a otra");
    }

    // -- Caso 3: raiz que no existe ------------------------------------------
    {
        long calls = 0;
        fs::walk_tree((root / "no_existe").string(), CountCalls{&calls});
        check(calls == 0, "una raiz inexistente no entrega nada");
    }

    // -- Caso 4: la ruta entregada sirve para abrir el fichero ---------------
    {
        bool opened = false;
        fs::walk_tree(root.string(), OpenDeep{&opened});
        check(opened, "la ruta entregada abre el fichero");
    }

    stdfs::remove_all(root);
    std::cout << (g_failures == 0 ? "TODO OK\n" : "HAY FALLOS\n");
    return g_failures == 0 ? 0 : 1;
}
