/**
 * @file helper_override.cpp
 * @brief Implementacion de la comprobacion de `@HelperOverride`.
 * @see vx/helper_override.h
 */
#include "vx/helper_override.h"

#include "vx/ast.h"
#include "vx/diagnostic.h"

namespace vx {

namespace {

/// Un ayudante con varias versiones y la forma de su sustituto.
struct HelperShape {
    const char *name;      ///< nombre del ayudante
    size_t param_count;    ///< cuantos parametros toma
    bool returns_value;    ///< si devuelve algo (no `void`)
    const char *signature; ///< la firma, para el mensaje
};

/// Los ayudantes que admiten sustituto.  La firma se comprueba por aridad y
/// por si devuelve algo: los tipos concretos los decide quien lo escribe.
constexpr HelperShape kHelpers[] = {
    {"memcpy", 3, false, "void(u8*, u8*, u64)"},
    {"strcmp", 4, true, "i64(u8*, i64, u8*, i64)"},
    {"strlen", 1, true, "i64(u8*)"},
};

/**
 * @brief La forma de un ayudante, o nulo si no admite sustituto.
 * @param helper Su nombre.
 * @return La forma.
 */
const HelperShape *find_helper(const std::string &helper) {
    for (const HelperShape &h : kHelpers)
        if (helper == h.name) return &h;
    return nullptr;
}

/**
 * @brief Si una funcion devuelve algo.
 * @param fd La funcion.
 * @return @c false si su tipo de vuelta es `void` o no se escribio.
 */
bool returns_value(const ast::FunctionDecl &fd) {
    if (!fd.return_type) return false;
    if (fd.return_type->kind != ast::NodeKind::PrimitiveTypeNode) return true;
    return static_cast<const ast::PrimitiveTypeNode *>(fd.return_type.get())
               ->prim != PrimitiveKind::VOID;
}

} // namespace

bool is_multiversioned_helper(const std::string &helper) {
    return find_helper(helper) != nullptr;
}

bool check_helper_override(const ast::FunctionDecl &fd, Diagnostics &diags) {
    const std::string &helper = fd.helper_override_target;
    const HelperShape *shape = find_helper(helper);
    if (shape == nullptr) {
        diags.diag(fd.loc, DiagLevel::WARN, "VX4014", {helper});
        return false;
    }
    if (fd.params.size() != shape->param_count ||
        returns_value(fd) != shape->returns_value)
        diags.diag(fd.loc, DiagLevel::WARN, "VX4015",
                   {helper, shape->signature});
    return true;
}

} // namespace vx
