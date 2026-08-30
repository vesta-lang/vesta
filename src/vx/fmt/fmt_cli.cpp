/**
 * @file fmt_cli.cpp
 * @brief El subcomando `vesta fmt`.
 *
 * Da formato a ficheros `.vx` segun el estandar del lenguaje
 * (`doc/VMdoc/Vesta/EstiloYFormato.md`).  Es un SUBCOMANDO y no una opcion
 * suelta, como `pkg` y `vxdbg`: dar formato no modifica una compilacion, es
 * una herramienta con su propio trabajo y sus propias ordenes.
 *
 * Por defecto ESCRIBE, igual que `go fmt`.  Formatear a mano no existe -- el
 * estandar dice que solo hay una forma valida --, asi que lo normal es querer
 * el fichero arreglado, no mirarlo.  Para lo otro estan `check` y `print`.
 *
 * Solo usa el lexer: no compila nada, no resuelve tipos y no toca la cache.
 * Eso es lo que le permite trabajar sobre codigo a medio escribir -- el caso
 * normal en un editor -- y no costarle un microsegundo al compilador.
 */

#include "vx/fmt/fmt_cli.h"

#include "vx/diag/diag_catalog.h"
#include "vx/fmt/fmt_driver.h"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace vx {
namespace fmt {
namespace cli {
namespace {

namespace fs = std::filesystem;

/// @brief Que hacer con el resultado.
enum class Mode {
    Write, ///< escribirlo sobre el fichero (lo normal)
    Check, ///< no escribir; fallar si algo no esta formateado
    Print, ///< sacarlo por la salida estandar
};

void usage() {
    std::printf(
        "uso: vesta fmt [check|print] <fichero.vx | directorio> ...\n"
        "\n"
        "  (sin orden)  da formato y ESCRIBE los ficheros que cambien\n"
        "  check        no escribe; termina con 1 si alguno no esta\n"
        "               formateado.  Para un gancho de integracion continua\n"
        "  print        escribe el resultado por la salida estandar\n"
        "\n"
        "Un directorio se recorre entero buscando .vx.  El estandar esta en\n"
        "doc/VMdoc/Vesta/EstiloYFormato.md.\n");
}

/// @brief Lee un fichero entero; devuelve falso si no se puede.
bool read_all(const std::string &path, std::string &out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

/**
 * @brief Anade a @p out los `.vx` que haya en @p target.
 *
 * Un fichero se anade tal cual; un directorio se recorre entero.  Se ordenan
 * para que dos ejecuciones den la misma salida: el orden del sistema de
 * ficheros no esta garantizado, y sin ordenar el informe de `check` cambiaria
 * de una vez a otra.
 */
void collect(const std::string &target, std::vector<std::string> &out) {
    std::error_code ec;
    if (fs::is_directory(target, ec)) {
        std::vector<std::string> found;
        for (const auto &e : fs::recursive_directory_iterator(target, ec)) {
            if (ec) break;
            if (!e.is_regular_file(ec)) continue;
            if (e.path().extension() == ".vx")
                found.push_back(e.path().string());
        }
        std::sort(found.begin(), found.end());
        for (std::string &f : found)
            out.push_back(std::move(f));
        return;
    }
    out.push_back(target);
}

} // namespace

int run(int argc, char **argv) {
    Mode mode = Mode::Write;
    int first = 1;
    if (argc >= 2) {
        const std::string a1 = argv[1];
        if (a1 == "check") {
            mode = Mode::Check;
            first = 2;
        } else if (a1 == "print") {
            mode = Mode::Print;
            first = 2;
        } else if (a1 == "-h" || a1 == "--help") {
            usage();
            return 0;
        }
    }
    if (first >= argc) {
        usage();
        return 1;
    }

    // Lo que se pide formatear, y de paso TODO el proyecto: de ahi salen los
    // nombres de las funciones que capturan el texto de su argumento
    // (`R110`), que no se pueden saber mirando un fichero solo.
    std::vector<std::string> targets;
    for (int i = first; i < argc; ++i)
        collect(argv[i], targets);
    if (targets.empty()) {
        std::fprintf(stderr, "[fmt] no hay ningun .vx que formatear\n");
        return 1;
    }

    /* Los nombres de `R110` se sacan UNA vez, no por fichero.
     *
     * `format_file` los busca en todo el proyecto, asi que llamarlo dentro del
     * bucle releia y troceaba los 752 ficheros 752 veces -- medio millon de
     * lecturas para obtener siempre la misma lista, porque el conjunto de
     * nombres no depende de cual se este formateando. */
    FormatOptions options;
    options.raw_capture_names = capture_names_in_files(targets);

    /* Y los ficheros se reparten entre hilos.  Formatear uno no depende de los
     * demas -- cada cual lee su texto y produce el suyo --, asi que no hay
     * nada que compartir: los hilos van tomando trozos con un indice atomico y
     * escriben cada uno en SU casilla del resultado.  Sin cerrojos: lo unico
     * compartido es ese contador, y ahi un `fetch_add` basta. */
    struct Salida {
        bool ok = true;      ///< se pudo formatear
        bool cambia = false; ///< el resultado difiere del fichero
        std::string texto;   ///< resultado, solo si hace falta escribirlo
        std::string error;   ///< motivo, ya traducido
    };
    std::vector<Salida> salidas(targets.size());
    std::atomic<size_t> siguiente{0};

    const auto trabajar = [&]() {
        for (;;) {
            const size_t k = siguiente.fetch_add(1, std::memory_order_relaxed);
            if (k >= targets.size()) return;
            const std::string &path = targets[k];
            Salida &s = salidas[k];
            std::string original;
            if (!read_all(path, original)) {
                s.ok = false;
                s.error = "no se puede leer";
                continue;
            }
            FormatResult r = format(original, path, options);
            if (!r.ok) {
                // Nunca en silencio: el codigo dice exactamente que paso, y el
                // fichero se queda como estaba.
                s.ok = false;
                s.error = vx::diag::format(r.code, r.args);
                continue;
            }
            s.cambia = (r.text != original);
            if (mode == Mode::Print || s.cambia) s.texto = std::move(r.text);
        }
    };

    unsigned hilos = std::thread::hardware_concurrency();
    if (hilos == 0) hilos = 1;
    if (hilos > targets.size()) hilos = static_cast<unsigned>(targets.size());
    if (hilos <= 1) {
        trabajar();
    } else {
        std::vector<std::thread> equipo;
        equipo.reserve(hilos - 1);
        for (unsigned t = 1; t < hilos; ++t)
            equipo.emplace_back(trabajar);
        trabajar(); // el hilo principal tambien trabaja
        for (std::thread &t : equipo)
            t.join();
    }

    /* Escribir va aparte y EN ORDEN.  Lo que saca `check` es una lista que
     * alguien va a leer o a comparar entre ejecuciones; si cada hilo la
     * imprimiera segun acaba, saldria en un orden distinto cada vez. */
    int changed = 0, failed = 0;
    for (size_t k = 0; k < targets.size(); ++k) {
        const std::string &path = targets[k];
        const Salida &s = salidas[k];
        if (!s.ok) {
            std::fprintf(stderr, "[fmt] %s: %s\n", path.c_str(),
                         s.error.c_str());
            ++failed;
            continue;
        }
        if (mode == Mode::Print) {
            std::fputs(s.texto.c_str(), stdout);
            continue;
        }
        if (!s.cambia) continue; // ya estaba bien: no se toca
        ++changed;
        if (mode == Mode::Check) {
            std::printf("%s\n", path.c_str());
            continue;
        }
        std::ofstream out(path, std::ios::binary);
        if (!out.is_open()) {
            std::fprintf(stderr, "[fmt] no se puede escribir: %s\n",
                         path.c_str());
            ++failed;
            continue;
        }
        out << s.texto;
    }

    if (mode == Mode::Check) {
        if (changed > 0)
            std::fprintf(stderr, "[fmt] %d fichero(s) sin formatear.\n",
                         changed);
        return (changed > 0 || failed > 0) ? 1 : 0;
    }
    if (mode == Mode::Write && changed > 0)
        std::printf("[fmt] %d fichero(s) formateado(s).\n", changed);
    return failed > 0 ? 1 : 0;
}

} // namespace cli
} // namespace fmt
} // namespace vx
