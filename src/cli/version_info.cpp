/**
 * @file version_info.cpp
 * @brief Implementacion del banner de version de VestaVM.
 *
 * Esta TU se recompila en cada build porque incluye build_info_generated.h
 * (regenerado con el timestamp del momento por cmake/gen_build_info.cmake).
 * Es pequena a proposito para que ese rebuild constante no arrastre a main.cpp.
 */
#include "cli/version_info.h"

#include <algorithm>
#include <string>
#include <vector>
#include <cstdint>

// Deteccion de TTY: banner bonito en terminal, linea plana parseable en pipe.
#if defined(_WIN32)
#include <io.h>
#define VESTA_ISATTY_STDOUT() (_isatty(_fileno(stdout)) != 0)
#else
#include <unistd.h>
#define VESTA_ISATTY_STDOUT() (isatty(fileno(stdout)) != 0)
#endif

#include "pkg/ui.h"

// Numeros de version (fuente de verdad: project(...) en CMakeLists.txt).
#if __has_include("vx/version_generated.h")
#include "vx/version_generated.h"
#endif

// Fecha/hora del build + hash de git (regenerado en cada build por CMake).
// Gateado por si el header aun no existe (build sin el target vm_build_info).
#if __has_include("version/build_info_generated.h")
#include "version/build_info_generated.h"
#endif

#ifndef VESTA_BUILD_DATE
#define VESTA_BUILD_DATE "desconocida"
#endif
#ifndef VESTA_GIT_HASH
#define VESTA_GIT_HASH "desconocido"
#endif
#ifndef VESTA_GIT_HASH_KNOWN
#define VESTA_GIT_HASH_KNOWN 0
#endif

#ifndef VEX_VM_VERSION_STRING
#define VEX_VM_VERSION_STRING "0.1.0"
#endif

// CPU dispatch (cimiento): deteccion de features del host via cpuid.  Definida
// en src/runtime/native_callback.cpp; misma que usa el builtin cpu_features().
// Bit layout: 0=SSE2 1=SSE4.2 2=POPCNT 3=AVX 4=AVX2 5=BMI1 6=BMI2 7=AVX512F 8=ERMS.
extern "C" uint64_t vesta_runtime_cpu_features(void);

namespace cli {

namespace {

namespace ui = pkg::ui;

// Plataforma (SO) detectada en compile-time.
const char *platform_os() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "posix";
#endif
}

// Arquitectura detectada en compile-time.
const char *platform_arch() {
#if defined(__x86_64__) || defined(_M_X64)
    return "x86-64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86-32";
#else
    return "desconocida";
#endif
}

// Fila del banner: texto plano (para medir ancho) + texto con color.
struct Row {
    std::string plain;
    std::string colored;
};

// Longitud visible = la del texto plano (los codigos ANSI no ocupan columnas).
size_t visible_len(const Row &r) { return r.plain.size(); }

// Construye una fila "etiqueta:  valor" con la etiqueta en cyan/bold.
Row make_row(const std::string &label, const std::string &plain_value,
             const std::string &colored_value) {
    // Etiqueta alineada a 8 columnas para que los valores queden en columna.
    std::string lab = label;
    while (lab.size() < 8) lab.push_back(' ');
    Row r;
    r.plain = lab + plain_value;
    r.colored = std::string(ui::cyan()) + ui::bold() + lab + ui::reset() +
                colored_value;
    return r;
}

// Devuelve "[ok]" en verde si @p present, o "[--]" en gris tenue si no.
std::string mark(bool present) {
    if (present) {
        return std::string(ui::green()) + "[ok]" + ui::reset();
    }
    return std::string(ui::gray()) + ui::dim() + "[--]" + ui::reset();
}

// Version plana del mark (para medir ancho): siempre 4 chars.
std::string mark_plain(bool /*present*/) { return "[--]"; }

} // namespace

void print_version_banner(std::ostream &os) {
    // Salida MACHINE-READABLE cuando stdout no es un terminal (pipe/redireccion,
    // p.ej. el harness de benchmark que captura `vm --version`): una sola linea
    // parseable, sin la caja ni codigos ANSI.  El banner bonito solo en TTY.
    if (!VESTA_ISATTY_STDOUT()) {
        os << "Vesta v" << VEX_VM_VERSION_STRING << "-alpha"
           << " (build " << VESTA_BUILD_DATE;
        if (VESTA_GIT_HASH_KNOWN)
            os << ", " << VESTA_GIT_HASH;
        os << ")\n";
        return;
    }

    ui::init(); // Habilita ANSI en Windows 10+ / respeta NO_COLOR.

    const uint64_t feats = vesta_runtime_cpu_features();
    const bool has_sse2 = (feats & (1ull << 0)) != 0;
    const bool has_avx = (feats & (1ull << 3)) != 0;
    const bool has_avx2 = (feats & (1ull << 4)) != 0;
    const bool has_avx512 = (feats & (1ull << 7)) != 0;

    // ---- Construir las filas del banner --------------------------------
    std::vector<Row> rows;

    // Titulo: "Vesta vX.Y.Z-alpha" en bold.  Todo el ecosistema esta en alfa;
    // el sufijo -alpha se resalta en amarillo para dejarlo claro de un vistazo.
    {
        std::string base = std::string("Vesta v") + VEX_VM_VERSION_STRING;
        const char *alpha = "-alpha";
        Row r;
        r.plain = base + alpha;
        r.colored = std::string(ui::bold()) + ui::green() + base +
                    ui::yellow() + alpha + ui::reset();
        rows.push_back(r);
    }

    // build: fecha (+ hash de git si se conoce).
    {
        std::string plain_val = VESTA_BUILD_DATE;
        std::string colored_val = std::string(ui::reset()) + VESTA_BUILD_DATE;
#if VESTA_GIT_HASH_KNOWN
        plain_val += std::string("  (") + VESTA_GIT_HASH + ")";
        colored_val += std::string("  ") + ui::gray() + "(" + VESTA_GIT_HASH +
                       ")" + ui::reset();
#endif
        rows.push_back(make_row("build:", plain_val, colored_val));
    }

    // target: SO + arch.
    {
        std::string tgt = std::string(platform_os()) + " " + platform_arch();
        rows.push_back(make_row("target:", tgt, std::string(ui::reset()) + tgt));
    }

    // modos: interprete . JIT . AOT (los tres siempre compilados).
    {
        std::string plain_val = "interprete . JIT . AOT";
        std::string colored_val = std::string(ui::green()) + "interprete" +
                                  ui::gray() + " . " + ui::green() + "JIT" +
                                  ui::gray() + " . " + ui::green() + "AOT" +
                                  ui::reset();
        rows.push_back(make_row("modos:", plain_val, colored_val));
    }

    // SIMD: marcado segun la CPU actual.
    {
        struct Feat {
            const char *name;
            bool present;
        };
        const Feat fs[] = {
            {"SSE2", has_sse2},
            {"AVX", has_avx},
            {"AVX2", has_avx2},
            {"AVX-512", has_avx512},
        };
        std::string plain_val, colored_val = std::string(ui::reset());
        for (size_t i = 0; i < sizeof(fs) / sizeof(fs[0]); ++i) {
            if (i) {
                plain_val += "  ";
                colored_val += "  ";
            }
            const char *nm = fs[i].name;
            plain_val += std::string(nm) + " " + mark_plain(fs[i].present);
            // Nombre tenue si no soportado, normal si soportado.
            if (fs[i].present) {
                colored_val += std::string(nm) + " " + mark(true);
            } else {
                colored_val += std::string(ui::gray()) + ui::dim() + nm +
                               ui::reset() + " " + mark(false);
            }
        }
        rows.push_back(make_row("SIMD:", plain_val, colored_val));
    }

    // ---- Calcular ancho interior de la caja ----------------------------
    size_t inner = 0;
    for (const Row &r : rows) inner = std::max(inner, visible_len(r));
    const size_t pad_left = 1;  // espacio tras el borde izquierdo
    const size_t pad_right = 1; // espacio antes del borde derecho
    const size_t width = inner + pad_left + pad_right;

    const std::string cyan = ui::cyan();
    const std::string reset = ui::reset();

    // Borde superior: +----...----+
    os << cyan << "+";
    for (size_t i = 0; i < width; ++i) os << "-";
    os << "+" << reset << "\n";

    // Filas: | <colored> <padding> |
    for (const Row &r : rows) {
        os << cyan << "|" << reset;
        os << std::string(pad_left, ' ');
        os << r.colored;
        const size_t used = visible_len(r);
        if (used < inner) os << std::string(inner - used, ' ');
        os << std::string(pad_right, ' ');
        os << cyan << "|" << reset << "\n";
    }

    // Borde inferior.
    os << cyan << "+";
    for (size_t i = 0; i < width; ++i) os << "-";
    os << "+" << reset << "\n";
}

} // namespace cli
