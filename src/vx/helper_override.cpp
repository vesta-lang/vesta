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

/// Los ayudantes que admiten sustituto, indexados por `Helper`.  La firma se
/// comprueba por aridad y por si devuelve algo: los tipos concretos los
/// decide quien lo escribe.
constexpr HelperShape kHelpers[kHelperCount] = {
    {"memcpy", 3, false, "void(u8*, u8*, u64)"},
    {"strcmp", 4, true, "i64(u8*, i64, u8*, i64)"},
    {"strlen", 1, true, "i64(u8*)"},
};

/// Los nombres de `kHelpers`, internados una vez: el ayudante se reconoce por
/// IDENTIDAD del nombre, no comparando cadenas.
struct HelperNames {
    std::array<util::InternedName, kHelperCount> names; ///< por `Helper`

    /// @brief Interna los nombres de la tabla.
    HelperNames() {
        for (size_t i = 0; i < kHelperCount; ++i)
            names[i] = util::InternedName::intern(std::string(kHelpers[i].name));
    }
};

/**
 * @brief Los nombres internados de los ayudantes.
 * @return La tabla, construida la primera vez que se pide.
 */
const HelperNames &helper_names() {
    static const HelperNames names;
    return names;
}

/**
 * @brief La forma de un ayudante, o nulo si no admite sustituto.
 * @param helper Su nombre.
 * @return La forma.
 */
const HelperShape *find_helper(const std::string &helper) {
    Helper h;
    if (!helper_from_name(helper, h)) return nullptr;
    return &kHelpers[static_cast<size_t>(h)];
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

bool helper_from_name(const std::string &name, Helper &out) {
    const util::InternedName key = util::InternedName::intern(name);
    const HelperNames &table = helper_names();
    for (size_t i = 0; i < kHelperCount; ++i) {
        if (table.names[i] == key) {
            out = static_cast<Helper>(i);
            return true;
        }
    }
    return false;
}

const char *helper_name(Helper h) {
    return kHelpers[static_cast<size_t>(h)].name;
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
