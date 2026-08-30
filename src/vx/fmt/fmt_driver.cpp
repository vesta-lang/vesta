/**
 * @file fmt_driver.cpp
 * @brief El puente entre el formateador y el sistema de ficheros.
 *
 * La libreria del formateador es PURA: recibe un texto y devuelve otro, sin
 * abrir nada.  Eso la hace facil de probar y sirve igual al compilador, al LSP
 * y a un test.  Pero hay una cosa que no se puede saber mirando un solo
 * fichero -- que funciones capturan el TEXTO de su argumento (`R110`) --,
 * porque las importadas viven en otro modulo.
 *
 * Aqui esta esa mitad: leer los demas ficheros y sacar de ellos los nombres
 * que faltan.  Quien llama decide QUE ficheros son -- el compilador conoce sus
 * fuentes, el LSP su espacio de trabajo --, asi que este modulo no recorre
 * directorios ni adivina rutas: recibe la lista y la lee.
 */

#include "vx/fmt/fmt_driver.h"

#include "vx/fmt/fmt_internal.h"

#include <fstream>
#include <sstream>

namespace vx {
namespace fmt {

std::vector<std::string>
capture_names_in_files(const std::vector<std::string> &paths) {
    std::vector<std::string> names;
    for (const std::string &path : paths) {
        std::ifstream in(path, std::ios::binary);
        if (!in) continue; // un fichero que no se puede leer no aporta nombres
        std::ostringstream ss;
        ss << in.rdbuf();
        const std::string source = ss.str();

        std::string_view tail;
        std::string code;
        const std::vector<Piece> pieces = scan_pieces(source, path, tail, code);
        if (!code.empty()) continue; // no se pudo trocear: se salta

        for (std::string &n : capture_names_in(pieces)) {
            // Sin repetidos: la lista se recorre una vez por cada llamada del
            // fichero que se formatea, y los nombres suelen repetirse entre
            // modulos que se importan entre si.
            bool seen = false;
            for (const std::string &m : names)
                if (m == n) {
                    seen = true;
                    break;
                }
            if (!seen) names.push_back(std::move(n));
        }
    }
    return names;
}

FormatResult format_file(const std::string &path,
                         const std::vector<std::string> &project_files,
                         FormatOptions options) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        FormatResult r;
        r.ok = false;
        r.code = "VXF005";
        r.args = {path};
        return r;
    }
    std::ostringstream ss;
    ss << in.rdbuf();

    // Los nombres que el fichero no puede ver por si solo (`R110`).  Se anaden
    // a los que ya trajera el llamador en vez de sustituirlos: el LSP puede
    // tener resueltos unos y el proyecto otros.
    if (!project_files.empty()) {
        for (std::string &n : capture_names_in_files(project_files))
            options.raw_capture_names.push_back(std::move(n));
    }
    return format(ss.str(), path, options);
}

} // namespace fmt
} // namespace vx
