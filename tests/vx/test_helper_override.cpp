/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/vx/test_helper_override.cpp
 * @brief Comprueba la comprobacion de `@HelperOverride`.
 *
 * Estaba escrita dos veces, una por camino de compilacion.  Aqui se fijan las
 * tres respuestas que dan los dos: un ayudante que no admite sustituto se
 * avisa y se ignora (VX4014), una firma que no cuadra se avisa pero se acepta
 * (VX4015), y una que cuadra no dice nada.
 */
#include "vx/helper_override.h"

#include "vx/ast.h"
#include "vx/diagnostic.h"

#include <cstdio>
#include <memory>
#include <string>

namespace {

int g_failures = 0; ///< Cuantas comprobaciones han fallado.

/**
 * @brief Comprueba una condicion y deja constancia si no se cumple.
 * @param cond Lo que tiene que ser cierto.
 * @param what Que se estaba comprobando.
 */
void check(bool cond, const char *what) {
    if (!cond) {
        std::printf("FALLO: %s\n", what);
        ++g_failures;
    }
}

/**
 * @brief Una funcion anotada con `@HelperOverride(helper)`.
 * @param helper      El ayudante que sustituye.
 * @param params      Cuantos parametros tiene.
 * @param returns_i64 Si devuelve i64 (si no, `void`).
 * @return La declaracion.
 */
std::unique_ptr<vx::ast::FunctionDecl> make_override(const char *helper,
                                                     size_t params,
                                                     bool returns_i64) {
    auto fd = std::make_unique<vx::ast::FunctionDecl>();
    fd->name = "my_helper";
    fd->helper_override_target = helper;
    auto ret = std::make_unique<vx::ast::PrimitiveTypeNode>();
    ret->prim = returns_i64 ? vx::PrimitiveKind::I64 : vx::PrimitiveKind::VOID;
    fd->return_type = std::move(ret);
    for (size_t k = 0; k < params; ++k)
        fd->params.push_back(std::make_unique<vx::ast::ParamDecl>());
    return fd;
}

/**
 * @brief Si se emitio un diagnostico con ese codigo.
 * @param diags Los diagnosticos.
 * @param code  El codigo.
 * @return @c true si hay alguno.
 */
bool has_code(const vx::Diagnostics &diags, const char *code) {
    for (const auto &d : diags.all())
        if (d.code == code) return true;
    return false;
}

} // namespace

/**
 * @brief Punto de entrada del test.
 * @return 0 si todo paso, 1 si hubo algun fallo.
 */
int main() {
    check(vx::is_multiversioned_helper("memcpy"), "memcpy admite sustituto");
    check(vx::is_multiversioned_helper("strlen"), "strlen admite sustituto");
    check(!vx::is_multiversioned_helper("printf"), "printf no");

    {
        vx::Diagnostics diags;
        auto fd = make_override("memcpy", 3, false);
        check(vx::check_helper_override(*fd, diags), "memcpy bien firmado vale");
        check(diags.all().empty(), "y no dice nada");
    }
    {
        vx::Diagnostics diags;
        auto fd = make_override("strcmp", 2, true);
        check(vx::check_helper_override(*fd, diags),
              "firma que no cuadra: se acepta igual");
        check(has_code(diags, "VX4015"), "pero se avisa con VX4015");
    }
    {
        vx::Diagnostics diags;
        auto fd = make_override("strlen", 1, false);
        check(vx::check_helper_override(*fd, diags),
              "strlen que no devuelve nada: se acepta");
        check(has_code(diags, "VX4015"), "y se avisa");
    }
    {
        vx::Diagnostics diags;
        auto fd = make_override("printf", 1, true);
        check(!vx::check_helper_override(*fd, diags),
              "ayudante sin varias versiones: se ignora");
        check(has_code(diags, "VX4014"), "y se avisa con VX4014");
    }

    if (g_failures == 0) std::printf("test_helper_override: OK\n");
    return g_failures == 0 ? 0 : 1;
}
