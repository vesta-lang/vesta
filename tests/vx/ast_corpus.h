/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/vx/ast_corpus.h
 * @brief Lo que comparten los tests que recorren arboles del corpus: leer la
 *        lista del corpus, parsear cada fichero y sacar de un modulo cada cosa
 *        con cuerpo (funciones y metodos, tambien dentro de un `namespace`).
 *
 * Los tests del clon y del recorrido de hijos miran lo mismo -- cada cuerpo del
 * corpus -- y lo sacaban cada uno a su manera.  La lista del corpus la mantiene
 * el test del formateador (`tests/vx/fmt_corpus.txt`).
 */

#ifndef VX_TESTS_AST_CORPUS_H
#define VX_TESTS_AST_CORPUS_H

#include "vx/ast.h"
#include "vx/diagnostic.h"
#include "vx/lexer.h"
#include "vx/parser.h"

#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace vx_tests {

/**
 * @brief Lee un fichero entero.
 * @param path Ruta.
 * @param out  Contenido.
 * @return Si se pudo leer.
 */
inline bool read_file(const std::string &path, std::string &out) {
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
inline std::unique_ptr<vx::ast::ModuleNode>
parse(const std::string &src, const std::string &name,
      vx::Diagnostics &diags) {
    vx::Lexer lx(src, name, diags);
    vx::Parser p(lx, diags);
    return p.parse_program();
}

/**
 * @struct Callable
 * @brief Algo con cuerpo: sus parametros, su retorno y su cuerpo.
 */
struct Callable {
    std::vector<std::unique_ptr<vx::ast::ParamDecl>> *params; ///< parametros
    vx::ast::TypeNode *ret;   ///< retorno (puede ser nulo)
    vx::ast::BlockStmt *body; ///< cuerpo (puede ser nulo)
    std::string where;        ///< fichero y nombre, para los mensajes
};

/**
 * @brief Anade a @p out los metodos de un tipo.
 * @param methods Metodos.
 * @param where   Donde.
 * @param out     Destino.
 */
inline void collect_methods(
    std::vector<std::unique_ptr<vx::ast::ClassMethodDecl>> &methods,
    const std::string &where, std::vector<Callable> &out) {
    for (auto &m : methods)
        if (m)
            out.push_back({&m->params, m->return_type.get(), m->body.get(),
                           where + ":" + m->name});
}

/**
 * @brief Anade a @p out cada cosa con cuerpo de una lista de declaraciones,
 *        en orden.
 * @param decls Declaraciones.
 * @param where Donde.
 * @param out   Destino.
 */
inline void collect_callables(std::vector<std::unique_ptr<vx::ast::Node>> &decls,
                              const std::string &where,
                              std::vector<Callable> &out) {
    using vx::ast::NodeKind;
    for (auto &d : decls) {
        if (!d) continue;
        switch (d->kind) {
        case NodeKind::FunctionDecl: {
            auto &f = static_cast<vx::ast::FunctionDecl &>(*d);
            out.push_back({&f.params, f.return_type.get(), f.body.get(),
                           where + ":" + f.name});
            break;
        }
        case NodeKind::StructDecl:
            collect_methods(static_cast<vx::ast::StructDecl &>(*d).methods,
                            where, out);
            break;
        case NodeKind::ClassDecl:
            collect_methods(static_cast<vx::ast::ClassDecl &>(*d).methods,
                            where, out);
            break;
        case NodeKind::ConceptDecl:
            collect_methods(static_cast<vx::ast::ConceptDecl &>(*d).methods,
                            where, out);
            break;
        case NodeKind::NamespaceDecl:
            collect_callables(static_cast<vx::ast::NamespaceDecl &>(*d).decls,
                              where, out);
            break;
        default: break;
        }
    }
}

/**
 * @brief Llama a @p f con la ruta y el fuente de cada fichero del corpus.
 * @param root Raiz del repositorio.
 * @param f    Visitante con `operator()(const std::string &ruta, const
 *             std::string &fuente)`.
 * @return Falso si no se encontro la lista del corpus.
 */
template <class F> bool for_each_corpus_file(const std::string &root, F &f) {
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
        f(path, src);
    }
    return true;
}

} // namespace vx_tests

#endif // VX_TESTS_AST_CORPUS_H
