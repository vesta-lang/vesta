/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file roots.cpp
 * @brief Implementacion de la entrada al grafo.
 */

#include "vxdbg/roots.h"

#include "vxdbg/codec.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace vxdbg {

LanguageEntityId ArtifactMap::find(const std::string &symbol) const {
    // Busqueda binaria: el orden se mantiene al insertar precisamente para
    // poder buscar sin construir ningun indice al leer, que es lo que hace que
    // resolver una traza no cueste mas que la traza.
    auto it = std::lower_bound(
        symbols.begin(), symbols.end(), symbol,
        [](const std::pair<std::string, LanguageEntityId> &a,
           const std::string &b) { return a.first < b; });
    if (it == symbols.end() || it->first != symbol) return {};
    return it->second;
}

void ArtifactMap::add(std::string symbol, LanguageEntityId entity) {
    auto it = std::lower_bound(
        symbols.begin(), symbols.end(), symbol,
        [](const std::pair<std::string, LanguageEntityId> &a,
           const std::string &b) { return a.first < b; });
    // Un simbolo corresponde a UNA entidad.  Si llega otra vez es que quien lo
    // construyo se contradice; se queda la primera, igual que con las claves
    // repetidas del grafo.
    if (it != symbols.end() && it->first == symbol) return;
    symbols.insert(it, std::make_pair(std::move(symbol), entity));
}

std::string CacheRootRepository::path_for(BuildId build) const {
    return dir_ + "/roots/" + build.hash.to_hex() + ".ptr";
}

bool CacheRootRepository::publish(BuildId build, ContentHash map) const {
    if (build.empty() || map.empty()) return false;
    const std::string path = path_for(build);
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path(), ec);

    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    // Se escribe tambien de quien es: si el fichero acabara donde no toca, al
    // leerlo se nota en vez de servir el mapa de otro programa.
    const std::string body = map.to_hex() + "\n" + build.hash.to_hex() + "\n";
    const bool ok = std::fwrite(body.data(), 1, body.size(), f) == body.size();
    std::fclose(f);
    return ok;
}

bool CacheRootRepository::lookup(BuildId build, ArtifactMap &out_map) const {
    if (build.empty()) return false;
    std::FILE *f = std::fopen(path_for(build).c_str(), "rb");
    if (!f) return false;
    char buf[256];
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    buf[n] = '\0';

    const std::string body(buf, n);
    const size_t nl = body.find('\n');
    if (nl == std::string::npos) return false;
    const ContentHash map = ContentHash::from_hex(body.substr(0, nl));
    if (map.empty()) return false;

    const size_t nl2 = body.find('\n', nl + 1);
    if (nl2 == std::string::npos) return false;
    const ContentHash owner =
        ContentHash::from_hex(body.substr(nl + 1, nl2 - nl - 1));
    // Explicar un fallo con los simbolos de otra compilacion es peor que no
    // explicarlo: ante la duda, no se devuelve nada.
    if (owner != build.hash) return false;
    // Que el mapa viva en un almacen por contenido es asunto de aqui: quien
    // pregunta recibe el mapa y no se entera de que hubo una huella por medio.
    return load_node(store_, map, out_map);
}

} // namespace vxdbg
