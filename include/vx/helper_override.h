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

#include <string>

namespace vx {

class Diagnostics;
namespace ast {
struct FunctionDecl;
} // namespace ast

/**
 * @brief Si un ayudante tiene varias versiones y admite sustituto.
 * @param helper Nombre del ayudante (`memcpy`, `strcmp`, `strlen`).
 * @return @c true si lo admite.
 */
bool is_multiversioned_helper(const std::string &helper);

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
