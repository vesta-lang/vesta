/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/vx/test_ast_clone.cpp
 * @brief El clon de un arbol es el original: en todo el corpus, y en los tres
 *        casos que el clonador viejo perdia.
 *
 * El clonador de las genericas enumeraba a mano lo que copiaba y se quedaba
 * corto en silencio: los argumentos con nombre de una llamada, el `static` de
 * una variable, los patrones de valor de un `match`.  Ahora clon y comparacion
 * recorren la MISMA lista de campos (`vx/ast/fields.h`); aqui se comprueba que
 * clonar sin sustituir devuelve lo mismo para cada cuerpo del corpus, que la
 * comparacion no es trivialmente cierta, y que la sustitucion hace lo suyo.
 *
 * Uso:  ./test_vx_test_ast_clone [raiz_del_repositorio]
 */
#include "vx/ast/ast_equal.h"
#include "vx/diagnostic.h"
#include "vx/generics/generic_clone.h"
#include "vx/lexer.h"
#include "vx/parser.h"

#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0; ///< Cuantas comprobaciones han fallado.
int g_bodies = 0;   ///< Cuantos cuerpos se han clonado y comparado.

/**
 * @brief Deja constancia si una condicion no se cumple.
 * @param ok   La condicion.
 * @param what Que se estaba comprobando.
 * @param where Fichero o caso (puede ir vacio).
 */
void check(bool ok, const char *what, const std::string &where = "") {
    if (!ok) {
        std::printf("FALLO: %s %s\n", what, where.c_str());
        ++g_failures;
    }
}

/**
 * @brief Lee un fichero entero.
 * @param path Ruta.
 * @param out  Contenido.
 * @return Si se pudo leer.
 */
bool read_file(const std::string &path, std::string &out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

/**
 * @brief Parsea un fuente.
 * @param src   Texto.
 * @param name  Nombre para las posiciones.
 * @param diags Diagnosticos.
 * @return El modulo.
 */
std::unique_ptr<vx::ast::ModuleNode> parse(const std::string &src,
                                           const std::string &name,
                                           vx::Diagnostics &diags) {
    vx::Lexer lx(src, name, diags);
    vx::Parser p(lx, diags);
    return p.parse_program();
}

/**
 * @brief Clona un cuerpo con sus parametros y su retorno y lo compara.
 * @param params Parametros.
 * @param ret    Retorno (puede ser nulo).
 * @param body   Cuerpo (puede ser nulo).
 * @param where  Donde, para el mensaje.
 */
void check_callable(const std::vector<std::unique_ptr<vx::ast::ParamDecl>> &params,
                    const vx::ast::TypeNode *ret,
                    const vx::ast::BlockStmt *body, const std::string &where) {
    for (const auto &p : params) {
        if (!p) continue;
        auto c = vx::vxgen::clone_param_with_subst(*p, {});
        check(vx::ast::same_parsed(*p, *c), "el clon de un parametro", where);
    }
    auto r = vx::vxgen::clone_type_with_subst(ret);
    check(vx::ast::same_parsed(ret, r.get()), "el clon del retorno", where);
    if (!body) return;
    auto b = vx::vxgen::clone_stmt(body);
    check(vx::ast::same_parsed(body, b.get()), "el clon del cuerpo", where);
    ++g_bodies;
}

void check_decls(const std::vector<std::unique_ptr<vx::ast::Node>> &decls,
                 const std::string &where);

/**
 * @brief Los metodos de un tipo.
 * @param methods Metodos.
 * @param where   Donde.
 */
void check_methods(
    const std::vector<std::unique_ptr<vx::ast::ClassMethodDecl>> &methods,
    const std::string &where) {
    for (const auto &m : methods)
        if (m)
            check_callable(m->params, m->return_type.get(), m->body.get(),
                           where + ":" + m->name);
}

/**
 * @brief Cada cosa con cuerpo de una lista de declaraciones.
 * @param decls Declaraciones.
 * @param where Donde.
 */
void check_decls(const std::vector<std::unique_ptr<vx::ast::Node>> &decls,
                 const std::string &where) {
    using vx::ast::NodeKind;
    for (const auto &d : decls) {
        if (!d) continue;
        switch (d->kind) {
        case NodeKind::FunctionDecl: {
            const auto &f = static_cast<const vx::ast::FunctionDecl &>(*d);
            check_callable(f.params, f.return_type.get(), f.body.get(),
                           where + ":" + f.name);
            break;
        }
        case NodeKind::StructDecl:
            check_methods(static_cast<const vx::ast::StructDecl &>(*d).methods,
                          where);
            break;
        case NodeKind::ClassDecl:
            check_methods(static_cast<const vx::ast::ClassDecl &>(*d).methods,
                          where);
            break;
        case NodeKind::ConceptDecl:
            check_methods(
                static_cast<const vx::ast::ConceptDecl &>(*d).methods, where);
            break;
        case NodeKind::NamespaceDecl:
            check_decls(static_cast<const vx::ast::NamespaceDecl &>(*d).decls,
                        where);
            break;
        default: break;
        }
    }
}

/**
 * @brief Todo el corpus: cada cuerpo clonado sin sustituir es el original.
 * @param root Raiz del repositorio.
 * @return Falso si no se encontro la lista del corpus.
 */
bool check_corpus(const std::string &root) {
    // La lista la mantiene el test del formateador; se usa la misma.
    std::string list;
    if (!read_file(root + "/tests/vx/fmt_corpus.txt", list)) return false;
    std::istringstream lines(list);
    std::string path;
    while (std::getline(lines, path)) {
        while (!path.empty() && (path.back() == '\r' || path.back() == '\n'))
            path.pop_back();
        if (path.empty() || path[0] == '#') continue;
        std::string src;
        if (!read_file(root + "/" + path, src)) continue;
        vx::Diagnostics diags;
        auto mod = parse(src, path, diags);
        // Un fichero que no parsea solo (le falta el preprocesador, o es un
        // caso que DEBE fallar) no tiene arbol que clonar.
        if (!mod || diags.has_errors()) continue;
        check_decls(mod->decls, path);
    }
    return true;
}

/**
 * @brief La primera funcion de un modulo con ese nombre.
 * @param mod  Modulo.
 * @param name Nombre.
 * @return La funcion, o nula.
 */
const vx::ast::FunctionDecl *find_fn(const vx::ast::ModuleNode &mod,
                                     const char *name) {
    for (const auto &d : mod.decls)
        if (d && d->kind == vx::ast::NodeKind::FunctionDecl &&
            static_cast<const vx::ast::FunctionDecl &>(*d).name == name)
            return static_cast<const vx::ast::FunctionDecl *>(d.get());
    return nullptr;
}

/**
 * @brief Los tres casos que el clonador viejo perdia, y que la comparacion
 *        distingue un campo cambiado.
 */
void check_lost_fields() {
    const std::string src = "i64 resta(i64 a, i64 b) => a - b;\n"
                            "T nombrados<T>(T x) => (T)resta(.b = 2, .a = (i64)x);\n"
                            "T clasifica<T>(T x) {\n"
                            "    match (x) {\n"
                            "        case 1..5 => return (T)100;\n"
                            "        case 7 => return (T)200;\n"
                            "        case _ => return (T)300;\n"
                            "    }\n"
                            "    return (T)0;\n"
                            "}\n"
                            "T cuenta<T>(T x) {\n"
                            "    static i64 n = 0;\n"
                            "    n = n + 1;\n"
                            "    return (T)n;\n"
                            "}\n";
    vx::Diagnostics diags;
    auto mod = parse(src, "<clon>", diags);
    check(mod && !diags.has_errors(), "el caso escrito parsea");
    if (!mod || diags.has_errors()) return;

    // `static`: el clon de la declaracion lo conserva.
    const auto *cuenta = find_fn(*mod, "cuenta");
    check(cuenta && cuenta->body && !cuenta->body->body.empty(),
          "cuenta tiene cuerpo");
    if (cuenta && cuenta->body && !cuenta->body->body.empty()) {
        const auto *decl = cuenta->body->body[0].get();
        check(decl->kind == vx::ast::NodeKind::VarDeclStmt &&
                  static_cast<const vx::ast::VarDeclStmt *>(decl)->is_static,
              "el original lleva `static`");
        auto c = vx::vxgen::clone_stmt(decl);
        check(c && c->kind == vx::ast::NodeKind::VarDeclStmt &&
                  static_cast<vx::ast::VarDeclStmt *>(c.get())->is_static,
              "el clon conserva `static`");

        // La comparacion no es trivialmente cierta: un campo del parser
        // cambiado la rompe, uno anotado no.
        auto *vd = static_cast<vx::ast::VarDeclStmt *>(c.get());
        vd->declared_deleter = "otro";
        check(vx::ast::same_parsed(decl, c.get()),
              "un campo ANOTADO distinto no cuenta");
        vd->is_static = false;
        check(!vx::ast::same_parsed(decl, c.get()),
              "un campo del parser distinto si cuenta");
    }

    // Argumentos con nombre: el clon de la llamada los conserva.
    const auto *nombrados = find_fn(*mod, "nombrados");
    check(nombrados && nombrados->body, "nombrados tiene cuerpo");
    if (nombrados && nombrados->body) {
        auto c = vx::vxgen::clone_stmt(nombrados->body.get());
        check(vx::ast::same_parsed(nombrados->body.get(), c.get()),
              "el clon de la llamada con nombres es el original");
    }

    // Patrones de valor del match: el clon los conserva.
    const auto *clasifica = find_fn(*mod, "clasifica");
    check(clasifica && clasifica->body && !clasifica->body->body.empty(),
          "clasifica tiene cuerpo");
    if (clasifica && clasifica->body && !clasifica->body->body.empty()) {
        const auto *st = clasifica->body->body[0].get();
        check(st->kind == vx::ast::NodeKind::ExprStmt, "el match es sentencia");
        if (st->kind == vx::ast::NodeKind::ExprStmt) {
            auto c = vx::vxgen::clone_stmt(st);
            const auto *m = static_cast<const vx::ast::ExprStmt *>(c.get())
                                ->expr.get();
            check(m && m->kind == vx::ast::NodeKind::MatchExpr,
                  "el clon es un match");
            if (m && m->kind == vx::ast::NodeKind::MatchExpr) {
                const auto &arms =
                    static_cast<const vx::ast::MatchExpr *>(m)->arms;
                check(arms.size() == 3, "tres ramas");
                check(arms.size() == 3 && arms[0].value_pattern &&
                          arms[0].value_pattern_hi && arms[1].value_pattern,
                      "el rango y el valor siguen en el clon");
            }
            check(vx::ast::same_parsed(st, c.get()),
                  "el clon del match es el original");
        }
    }
}

/**
 * @brief La sustitucion: el parametro cambia por su argumento, con las marcas
 *        de nivel sumadas, y `new T[n]` pasa a nombrar el argumento.
 */
void check_substitution() {
    const std::vector<std::string> params = {"T"};
    auto arg = std::make_unique<vx::ast::PrimitiveTypeNode>();
    arg->prim = vx::PrimitiveKind::I64;
    const std::vector<const vx::ast::TypeNode *> arg_nodes = {arg.get()};
    vx::vxgen::GenSubst g;
    g.params = &params;
    g.arg_nodes = &arg_nodes;

    vx::ast::NamedTypeNode t;
    t.name = "T";
    t.is_const = true;
    auto c = vx::vxgen::clone_type_with_subst(&t, g);
    check(c && c->kind == vx::ast::NodeKind::PrimitiveTypeNode,
          "T pasa a ser el primitivo");
    check(c && c->is_const, "y conserva el `const` escrito sobre T");

    vx::ast::NewExpr n;
    n.class_name = "T";
    auto cn = vx::vxgen::clone_expr(&n, g);
    check(cn && static_cast<vx::ast::NewExpr *>(cn.get())->class_name == "i64",
          "new T[...] pasa a nombrar i64");

    // Un nombre que no es parametro se queda como esta.
    vx::ast::NamedTypeNode u;
    u.name = "Punto";
    auto cu = vx::vxgen::clone_type_with_subst(&u, g);
    check(vx::ast::same_parsed(&u, cu.get()), "otro nombre no se toca");
}

} // namespace

int main(int argc, char **argv) {
    std::printf("=== test_ast_clone ===\n");
    const std::string root = (argc > 1) ? argv[1] : ".";
    check_lost_fields();
    check_substitution();
    if (!check_corpus(root)) {
        std::printf("  no se encontro tests/vx/fmt_corpus.txt\n");
        return 2;
    }
    check(g_bodies > 1000, "el corpus aporta cuerpos de sobra");
    std::printf("  %d cuerpos clonados y comparados\n", g_bodies);
    if (g_failures) {
        std::printf("%d fallos\n", g_failures);
        return 1;
    }
    std::printf("OK\n");
    return 0;
}
