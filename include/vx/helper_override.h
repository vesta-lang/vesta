/**
 * @file helper_override.h
 * @brief `@HelperOverride`: que ayudantes admiten un sustituto y con que firma.
 *
 * Un ayudante con VARIAS versiones (una por nivel de CPU) se elige al arrancar
 * y el programa puede aportar la suya con `@HelperOverride(<ayudante>)`.  Que
 * ayudantes lo admiten y que firma tienen que tener estaba escrito DOS veces --
 * en el camino de fichero suelto y en el de proyecto -- con los mensajes a mano
 * en cada uno.  Aqui, una vez.
 */
#ifndef VX_HELPER_OVERRIDE_H
#define VX_HELPER_OVERRIDE_H

#include "util/name_pool.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace vx {

class Diagnostics;
namespace ast {
struct FunctionDecl;
} // namespace ast

/**
 * @brief Los ayudantes con varias versiones que admiten sustituto.  Es un
 *        vocabulario CERRADO: se indexa, no se busca por nombre.
 */
enum class Helper : uint8_t {
    Memcpy, ///< `memcpy`
    Strcmp, ///< `strcmp`
    Strlen, ///< `strlen`
    Count   ///< cuantos hay (no es un ayudante)
};

/// Cuantos ayudantes admiten sustituto.
constexpr size_t kHelperCount = static_cast<size_t>(Helper::Count);

/// El sustituto de cada ayudante, por su simbolo internado; vacio = ninguno.
using HelperOverrides = std::array<util::InternedName, kHelperCount>;

/**
 * @brief El ayudante que nombra @p name.
 * @param name Nombre escrito en `@HelperOverride(<name>)`.
 * @param out  El ayudante, si lo es.
 * @return @c false si ese nombre no es un ayudante con sustituto.
 */
bool helper_from_name(const std::string &name, Helper &out);

/**
 * @brief El nombre de un ayudante, para los mensajes.
 * @param h El ayudante.
 * @return Su nombre.
 */
const char *helper_name(Helper h);

/**
 * @brief Comprueba un `@HelperOverride` y dice lo que no cuadra.
 *
 * Un ayudante que no admite sustituto se avisa (VX4014) y la anotacion se
 * ignora.  Una firma que no es la esperada se avisa (VX4015) pero se acepta:
 * manda quien la escribe.
 *
 * @param fd    La funcion anotada.
 * @param diags Donde se avisa.
 * @return @c true si el sustituto se puede usar.
 */
bool check_helper_override(const ast::FunctionDecl &fd, Diagnostics &diags);

} // namespace vx

#endif // VX_HELPER_OVERRIDE_H
