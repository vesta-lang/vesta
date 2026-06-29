/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file snippet_loader.cpp
 * @brief Implementacion del cargador de snippets de @c stdlib/port/c.
 */

#include "port/snippet_loader.h"

#include "util/fs_utils.h" // fs::get_executable_path

#include <filesystem>
#include <fstream>
#include <sstream>

namespace port {

std::string resolve_port_c_dir(const std::string &override_dir) {
    if (!override_dir.empty()) return override_dir;
    // Autodetect: paths comunes relativos al cwd (los proyectos Vesta suelen
    // ejecutarse desde el root del repo).
    static const char *candidates[] = {
        "stdlib/port/c",
        "../stdlib/port/c",
        "../../stdlib/port/c",
    };
    for (const char *c : candidates) {
        std::ifstream test(std::string(c) + "/vex_macros.v.c");
        if (test.good()) return c;
    }
    // Fallback relativo al EJECUTABLE (instalacion: vesta.exe junto a
    // stdlib/port/c/; build-tree: vm.exe en cmake-build-X/).
    const std::string exe = fs::get_executable_path();
    if (!exe.empty()) {
        const std::filesystem::path ed =
            std::filesystem::path(exe).parent_path();
        const std::filesystem::path exe_cands[] = {
            ed / "stdlib" / "port" / "c",
            ed.parent_path() / "stdlib" / "port" / "c"};
        for (const auto &c : exe_cands) {
            std::ifstream test((c / "vex_macros.v.c").string());
            if (test.good()) return c.string();
        }
    }
    return "stdlib/port/c"; // fallback razonable
}

std::string load_snippet_text(const std::string &name,
                              const std::string &override_dir, bool &ok) {
    ok = false;
    const std::string dir = resolve_port_c_dir(override_dir);
    const std::string path = dir + "/" + name + ".v.c";
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return "";

    // Eliminar la cabecera de metadata (lineas iniciales `// @...`).
    std::string line;
    std::ostringstream content;
    bool in_header = true;
    while (std::getline(in, line)) {
        if (in_header && line.size() > 3 && line[0] == '/' && line[1] == '/' &&
            line[2] == ' ' && line[3] == '@') {
            continue; // linea de metadata
        }
        in_header = false;
        content << line << "\n";
    }
    ok = true;
    return content.str();
}

} // namespace port
