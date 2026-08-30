/**
 * @file literals.cpp
 * @brief Forma canonica de un literal numerico (`R106`-`R109`).
 *
 * Es la unica parte del formateador que cambia el TEXTO de un token en vez de
 * moverlo.  Se permite porque ninguna de estas reescrituras cambia el valor:
 * rellenar con ceros por la izquierda, pasar los digitos a mayusculas y
 * agrupar con `_` dan exactamente el mismo numero.  La salvaguarda de
 * `fmt.cpp` lo comprueba de todas formas, comparando el valor ya leido por el
 * lexer y no el lexema.
 *
 * Por que agrupar, que es lo que las cuatro reglas tienen en comun: un numero
 * largo no se lee de un vistazo, se CUENTA.  `10000000` obliga a contar ceros
 * para saber si son diez millones o cien; `10_000_000` se lee entero.  Lo
 * mismo con los bits: `0xF` y `0x0F` valen igual, pero solo el segundo deja
 * ver que ocupa un byte.  Cada base se agrupa por lo que significa en ella:
 *
 *   - hexadecimal: de dos en dos, que es un byte, hasta la anchura de un tipo
 *     del lenguaje (2, 4, 8 y 16 digitos son u8, u16, u32 y u64).
 *   - binario: de cuatro en cuatro, que es un digito hexadecimal.
 *   - octal: de tres en tres, que es un byte redondeado hacia arriba, como los
 *     permisos de un fichero (`0o755`).
 *   - decimal: de tres en tres con `_`, la separacion de millares de siempre.
 */

#include "vx/fmt/fmt_internal.h"

#include <cctype>

namespace vx {
namespace fmt {
namespace {

/// @brief A partir de cuantos digitos se agrupa un decimal con `_` (`R109`).
///
/// Cuatro digitos se leen de un vistazo (`1000`, `2026`), y meterles un `_`
/// solo anade ruido; cinco ya no (`10000` puede ser diez mil o cien mil).
constexpr size_t kGroupFrom = 5;

/// @brief Digitos hexadecimales en mayuscula (`R106`).
char upper_digit(char c) {
    return (c >= 'a' && c <= 'f') ? (char)(c - 'a' + 'A') : c;
}

/**
 * @brief Anchura a la que se rellena un literal, o 0 para dejarlo como esta.
 *
 * @param digits Cuantos digitos tiene ya.
 * @param step Tamano del grupo en digitos de esa base.
 * @param max_width Anchura mas alla de la cual no se toca.
 * @param powers Cierto si solo valen las anchuras de un tipo (2, 4, 8, 16).
 */
size_t target_width(size_t digits, size_t step, size_t max_width, bool powers) {
    if (digits == 0 || digits > max_width) return 0;
    if (powers) {
        // Las anchuras que nombran un tipo: 2, 4, 8 y 16 digitos hex son
        // u8, u16, u32 y u64.  Una de 6 no es ningun tipo, y sube a 8.
        for (size_t w = step; w <= max_width; w *= 2)
            if (digits <= w) return w;
        return 0;
    }
    return ((digits + step - 1) / step) * step;
}

/**
 * @brief Agrupa los millares de una tira de digitos con `_`.
 *
 * Solo se aplica a la parte ENTERA.  Detras del punto no hay millares que
 * contar, y agrupar ahi deja restos como `0.123_456_7`, que se lee peor que
 * el numero sin tocar.
 *
 * @param digits Digitos sin separadores.
 */
std::string group_digits(std::string_view digits) {
    if (digits.size() < kGroupFrom) return std::string(digits);
    std::string out;
    out.reserve(digits.size() + digits.size() / 3);
    // El grupo corto va delante: `1_000_000`, no `100_000_0`.
    const size_t head = ((digits.size() - 1) % 3) + 1;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i >= head && (i - head) % 3 == 0) out += '_';
        out += digits[i];
    }
    return out;
}

/// @brief Copia una tira quitando los `_`, para poder reagruparla.
std::string strip_separators(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s)
        if (c != '_') out += c;
    return out;
}

/**
 * @brief Forma canonica de un literal con base explicita (`0x`, `0b`, `0o`).
 * @param text Lexema completo.
 * @return El texto canonico, o vacio si no hay nada que cambiar.
 */
std::string canonical_based(std::string_view text) {
    size_t step = 0, max_width = 0;
    bool powers = false, hex = false, octal = false;
    switch (text[1]) {
    case 'x':
    case 'X':
        step = 2; // un byte
        max_width = 16;
        powers = true;
        hex = true;
        break;
    case 'b':
    case 'B':
        step = 4; // un digito hexadecimal
        max_width = 64;
        break;
    case 'o':
    case 'O':
        step = 3; // un byte redondeado hacia arriba (`0o377` = 255)
        max_width = 24;
        octal = true;
        break;
    default: return {};
    }

    // Separar los digitos del sufijo de tipo, que empieza por letra.  Se
    // anota si aparece un `_` ENTRE digitos, que es distinto del que separa
    // el sufijo (`0xFF_u32`): aquel es una agrupacion que eligio el autor.
    size_t end = 2;
    bool grouped = false;
    while (end < text.size()) {
        const char c = text[end];
        const bool is_digit = hex     ? (bool)std::isxdigit((unsigned char)c)
                              : octal ? (c >= '0' && c <= '7')
                                      : (c == '0' || c == '1');
        if (!is_digit) {
            // Un `_` solo sigue siendo parte del numero si detras hay digito.
            if (c == '_' && end + 1 < text.size()) {
                const char n = text[end + 1];
                const bool next_digit =
                    hex     ? (bool)std::isxdigit((unsigned char)n)
                    : octal ? (n >= '0' && n <= '7')
                            : (n == '0' || n == '1');
                if (next_digit) {
                    grouped = true;
                    ++end;
                    continue;
                }
            }
            break;
        }
        ++end;
    }
    // En estas bases un `_` entre digitos marca campos de un formato, no
    // millares (`0xDEAD_BEEF`), y reagrupar seria discutir con el autor.
    if (grouped) return {};

    const std::string_view digits = text.substr(2, end - 2);
    std::string_view suffix = text.substr(end);
    if (!suffix.empty() && suffix.front() == '_') suffix.remove_prefix(1);
    if (digits.empty()) return {}; // literal roto: no es cosa del formateador

    const size_t width = target_width(digits.size(), step, max_width, powers);
    if (width == 0) return {};

    std::string out;
    out.reserve(3 + width + suffix.size());
    out += '0';
    out += hex ? 'x' : (octal ? 'o' : 'b');
    out.append(width - digits.size(), '0');
    for (const char c : digits)
        out += hex ? upper_digit(c) : c;
    // `R108`: el sufijo se separa con `_`.  Pegado al numero se confunde con
    // el, y en hexadecimal hasta con un digito: `0xFFu32` frente a
    // `0xFF_u32`.
    if (!suffix.empty()) {
        out += '_';
        out += suffix;
    }
    return out;
}

/**
 * @brief Forma canonica de un decimal o un flotante (`R109`).
 *
 * Reagrupa de tres en tres con `_` la parte entera; la fraccionaria se deja
 * como esta.  A diferencia de las bases explicitas aqui SI se rehace la
 * agrupacion que hubiera, porque en decimal `_` solo puede significar
 * millares y una separacion distinta seria un error de tecleo.
 *
 * @param text Lexema completo.
 * @return El texto canonico, o vacio si no hay nada que cambiar.
 */
std::string canonical_decimal(std::string_view text) {
    // Trocear en parte entera, fraccionaria, exponente y sufijo.  El exponente
    // no se agrupa: son dos o tres digitos y `_` ahi solo estorba.
    size_t i = 0;
    while (i < text.size() &&
           (std::isdigit((unsigned char)text[i]) || text[i] == '_'))
        ++i;
    const std::string_view int_part = text.substr(0, i);
    if (int_part.empty()) return {};

    bool has_dot = false;
    std::string_view frac_part;
    if (i < text.size() && text[i] == '.') {
        has_dot = true;
        const size_t start = ++i;
        while (i < text.size() &&
               (std::isdigit((unsigned char)text[i]) || text[i] == '_'))
            ++i;
        frac_part = text.substr(start, i - start);
        // El `_` final no es de la fraccion: es el que separa el sufijo de
        // `R108`.  Sin quitarlo, `3.14_f64` acababa con dos.
        while (!frac_part.empty() && frac_part.back() == '_')
            frac_part.remove_suffix(1);
    }
    // Lo que queda puede ser exponente, sufijo o los dos (`1e9_i64`).  Se
    // separan porque el `_` de `R108` va delante del sufijo, no del exponente.
    std::string_view rest = text.substr(i);
    std::string_view exponent;
    if (!rest.empty() && (rest.front() == 'e' || rest.front() == 'E')) {
        size_t j = 1;
        if (j < rest.size() && (rest[j] == '+' || rest[j] == '-')) ++j;
        while (j < rest.size() && std::isdigit((unsigned char)rest[j]))
            ++j;
        exponent = rest.substr(0, j);
        rest = rest.substr(j);
    }
    if (!rest.empty() && rest.front() == '_') rest.remove_prefix(1);

    std::string out = group_digits(strip_separators(int_part));
    if (has_dot) {
        out += '.';
        out += frac_part; // la fraccion se queda tal cual
    }
    out += exponent;
    if (!rest.empty()) {
        out += '_'; // `R108`: el sufijo de tipo va separado
        out += rest;
    }
    return out;
}

} // namespace

/**
 * @brief Devuelve la forma canonica de un literal, o vacio si ya la tiene.
 *
 * @param text Lexema tal como lo escribio el autor, sufijo incluido.
 * @return El texto canonico, o una cadena vacia si no hay nada que cambiar.
 */
std::string canonical_literal(std::string_view text) {
    if (text.empty()) return {};
    std::string out;
    if (text.size() > 2 && text[0] == '0' &&
        (std::isalpha((unsigned char)text[1]) != 0) && text[1] != 'e' &&
        text[1] != 'E')
        out = canonical_based(text);
    else if (std::isdigit((unsigned char)text[0]))
        out = canonical_decimal(text);
    return out == text ? std::string{} : out;
}

} // namespace fmt
} // namespace vx
