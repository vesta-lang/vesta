/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file param_hints.cpp
 * @brief Implementacion de los parameter hints (inlay) del LSP de Vesta.
 */

#include "lsp/param_hints.h"

#include "lsp/builtin_docs.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "vx/ast.h"
#include "vx/diagnostic.h"
#include "vx/lexer.h"
#include "vx/parser.h"
#include "vx/token.h"

namespace lsp {

namespace {

/**
 * @brief Indice byte -> (linea 0-based, columna 0-based en UTF-16).
 *
 * Misma construccion que el resaltado semantico: una pasada decodificando UTF-8
 * que anota, por cada byte, la posicion LSP del code point al que pertenece.
 */
class PosIndex {
  public:
    explicit PosIndex(const std::string &src) { build(src); }

    void at(size_t off, uint32_t &line, uint32_t &col) const {
        if (off >= line_.size()) {
            line = end_line_;
            col = end_col_;
            return;
        }
        line = line_[off];
        col = col_[off];
    }

  private:
    void build(const std::string &src) {
        const size_t n = src.size();
        line_.assign(n, 0);
        col_.assign(n, 0);
        uint32_t line = 0, col = 0;
        size_t i = 0;
        while (i < n) {
            const unsigned char b = static_cast<unsigned char>(src[i]);
            size_t seq = 1;
            uint32_t cp = b;
            if (b < 0x80) {
                seq = 1;
            } else if ((b & 0xE0) == 0xC0) {
                seq = 2;
                cp = b & 0x1F;
            } else if ((b & 0xF0) == 0xE0) {
                seq = 3;
                cp = b & 0x0F;
            } else if ((b & 0xF8) == 0xF0) {
                seq = 4;
                cp = b & 0x07;
            } else {
                seq = 1;
            }
            if (seq > 1) {
                if (i + seq > n) {
                    seq = 1;
                } else {
                    bool ok = true;
                    for (size_t k = 1; k < seq; ++k) {
                        const unsigned char cb =
                            static_cast<unsigned char>(src[i + k]);
                        if ((cb & 0xC0) != 0x80) {
                            ok = false;
                            break;
                        }
                        cp = (cp << 6) | (cb & 0x3F);
                    }
                    if (!ok)
                        seq = 1;
                }
            }
            for (size_t k = 0; k < seq && (i + k) < n; ++k) {
                line_[i + k] = line;
                col_[i + k] = col;
            }
            if (seq == 1 && b == '\n') {
                ++line;
                col = 0;
            } else {
                col += (seq == 4 && cp >= 0x10000) ? 2u : 1u;
            }
            i += seq;
        }
        end_line_ = line;
        end_col_ = col;
    }

    std::vector<uint32_t> line_;
    std::vector<uint32_t> col_;
    uint32_t end_line_ = 0;
    uint32_t end_col_ = 0;
};

/// Clave compacta (linea<<32 | columna) de una posicion fuente, para el set de
/// posiciones de los parametros de las declaraciones.
inline uint64_t loc_key(const vx::SourceLoc &l) {
    return (static_cast<uint64_t>(l.line) << 32) | static_cast<uint64_t>(l.column);
}

/// Recoleccion del modulo: nombres de parametros de funciones libres, de
/// metodos (por nombre, marcando los ambiguos) y las posiciones de TODOS los
/// parametros declarados (para distinguir una declaracion de una llamada).
struct DeclInfo {
    std::unordered_map<std::string, std::vector<std::string>> fn_params;
    std::unordered_map<std::string, std::vector<std::string>> method_params;
    std::unordered_set<std::string> ambiguous_methods; ///< mismo nombre, params != .
    std::unordered_set<uint64_t> param_decl_locs;      ///< loc_key de cada param.
};

/// Anota los nombres de @p params (en @p names) y registra sus posiciones en
/// @p locs (para reconocer luego el sitio de la declaracion).
void take_params(const std::vector<std::unique_ptr<vx::ast::ParamDecl>> &params,
                 std::vector<std::string> &names,
                 std::unordered_set<uint64_t> &locs) {
    names.reserve(params.size());
    for (const auto &p : params) {
        if (!p)
            continue;
        names.push_back(p->name);
        locs.insert(loc_key(p->loc));
    }
}

/// Registra un metodo en el mapa por-nombre, marcando ambiguo si ya existe con
/// una lista de nombres de parametros distinta (no podemos saber a que clase
/// pertenece el receptor sin resolver tipos).
void register_method(DeclInfo &di, const std::string &name,
                     std::vector<std::string> names) {
    if (name.empty())
        return;
    auto it = di.method_params.find(name);
    if (it == di.method_params.end()) {
        di.method_params.emplace(name, std::move(names));
    } else if (it->second != names) {
        di.ambiguous_methods.insert(name);
    }
}

void collect_decls(const vx::ast::ModuleNode *mod, DeclInfo &di) {
    if (!mod)
        return;
    for (const auto &node : mod->decls) {
        if (!node)
            continue;
        switch (node->kind) {
        case vx::ast::NodeKind::FunctionDecl: {
            auto *d = static_cast<const vx::ast::FunctionDecl *>(node.get());
            std::vector<std::string> names;
            take_params(d->params, names, di.param_decl_locs);
            if (!d->name.empty())
                di.fn_params[d->name] = std::move(names);
            break;
        }
        case vx::ast::NodeKind::ExternFnDecl: {
            auto *d = static_cast<const vx::ast::ExternFnDecl *>(node.get());
            std::vector<std::string> names;
            take_params(d->params, names, di.param_decl_locs);
            if (!d->name.empty())
                di.fn_params[d->name] = std::move(names);
            break;
        }
        case vx::ast::NodeKind::ClassDecl: {
            auto *d = static_cast<const vx::ast::ClassDecl *>(node.get());
            for (const auto &m : d->methods) {
                if (!m)
                    continue;
                std::vector<std::string> names;
                take_params(m->params, names, di.param_decl_locs);
                register_method(di, m->name, std::move(names));
            }
            break;
        }
        case vx::ast::NodeKind::StructDecl: {
            auto *d = static_cast<const vx::ast::StructDecl *>(node.get());
            for (const auto &m : d->methods) {
                if (!m)
                    continue;
                std::vector<std::string> names;
                take_params(m->params, names, di.param_decl_locs);
                register_method(di, m->name, std::move(names));
            }
            break;
        }
        default:
            break;
        }
    }
}

} // namespace

std::vector<ParamHint> compute_param_hints(const std::string &text,
                                           const std::string &filename) {
    std::vector<ParamHint> hints;
    try {
        // 1) Recolectar declaraciones: params de funciones/metodos + posiciones
        //    de los parametros declarados (para no confundir una DECLARACION con
        //    una llamada).
        DeclInfo di;
        {
            vx::Diagnostics diags;
            vx::Lexer lex(text, filename, diags);
            vx::Parser parser(lex, diags);
            std::unique_ptr<vx::ast::ModuleNode> mod = parser.parse_program();
            collect_decls(mod.get(), di);
        }
        if (di.fn_params.empty() && di.method_params.empty())
            return hints;

        // 2) Lexar a un vector de tokens (necesitamos mirar adelante/atras).
        std::vector<vx::Token> toks;
        {
            vx::Diagnostics diags;
            vx::Lexer lex(text, filename, diags);
            const size_t kMaxTokens = text.size() + 1024;
            for (;;) {
                vx::Token t = lex.next();
                if (t.kind == vx::TokenKind::END_OF_FILE)
                    break;
                toks.push_back(t);
                if (toks.size() > kMaxTokens)
                    break;
            }
        }

        PosIndex idx(text);
        using TK = vx::TokenKind;
        const int n = static_cast<int>(toks.size());

        // 3) Buscar el patron IDENT( y resolver sus parametros.
        for (int i = 0; i + 1 < n; ++i) {
            if (toks[i].kind != TK::IDENTIFIER)
                continue;
            if (toks[i + 1].kind != TK::LPAREN)
                continue;

            const bool is_method =
                (i > 0 && toks[i - 1].kind == TK::DOT);
            const bool is_new =
                (i > 0 && toks[i - 1].kind == TK::KW_NEW);
            if (is_new)
                continue; // construccion new T(): los params del ctor en v2.

            const std::vector<std::string> *pnames = nullptr;
            if (is_method) {
                // Llamada a metodo obj.m(): resolver por nombre (sin tipo del
                // receptor); si el nombre es ambiguo entre clases, omitir.
                if (di.ambiguous_methods.count(toks[i].lexeme))
                    continue;
                auto it = di.method_params.find(toks[i].lexeme);
                if (it == di.method_params.end())
                    continue;
                pnames = &it->second;
            } else {
                // Llamada a funcion libre.  Si el '(' es el de una DECLARACION
                // (el primer token tras '(' es el tipo de un parametro
                // declarado), NO es una llamada: saltar.
                if (i + 2 < n &&
                    di.param_decl_locs.count(loc_key(toks[i + 2].loc)))
                    continue;
                auto it = di.fn_params.find(toks[i].lexeme);
                if (it != di.fn_params.end()) {
                    pnames = &it->second;
                } else if (const BuiltinDoc *b = lookup_builtin(toks[i].lexeme)) {
                    // Builtin del lenguaje: usar los nombres de parametro de la
                    // tabla central para los ghost args (print, str_*, ...).
                    if (b->params.empty())
                        continue;
                    pnames = &b->params;
                } else {
                    continue;
                }
            }
            if (!pnames || pnames->empty())
                continue;
            const std::vector<std::string> &names = *pnames;

            // Recorrer los argumentos balanceando parentesis/corchetes/llaves.
            int depth = 1;       // ya consumimos el '(' de la llamada
            int arg_index = 0;   // indice del argumento actual (nivel 1)
            bool at_start = true; // estamos al inicio de un argumento
            for (int j = i + 2; j < n; ++j) {
                const TK k = toks[j].kind;
                if (k == TK::LPAREN || k == TK::LBRACKET || k == TK::LBRACE) {
                    if (at_start && depth == 1)
                        ; // un arg que empieza con (/[/{ : igual emitimos abajo
                    ++depth;
                    at_start = false;
                    continue;
                }
                if (k == TK::RPAREN || k == TK::RBRACKET || k == TK::RBRACE) {
                    --depth;
                    if (depth == 0)
                        break; // cerro la llamada
                    at_start = false;
                    continue;
                }
                if (k == TK::COMMA && depth == 1) {
                    ++arg_index;
                    at_start = true;
                    continue;
                }
                // Primer token (no delimitador) de un argumento de nivel 1.
                if (at_start && depth == 1) {
                    at_start = false;
                    if (arg_index < static_cast<int>(names.size())) {
                        const std::string &pname = names[arg_index];
                        if (!pname.empty()) {
                            // Omitir el hint redundante: el argumento es un solo
                            // identificador igual al nombre del parametro
                            // (p.ej. open(path: path)).
                            bool redundant =
                                toks[j].kind == TK::IDENTIFIER &&
                                toks[j].lexeme == pname && j + 1 < n &&
                                (toks[j + 1].kind == TK::COMMA ||
                                 toks[j + 1].kind == TK::RPAREN);
                            if (!redundant) {
                                ParamHint h;
                                idx.at(toks[j].loc.offset, h.line, h.character);
                                h.label = pname + ":";
                                hints.push_back(std::move(h));
                            }
                        }
                    }
                }
            }
        }
    } catch (...) {
        // Robustez: devolver lo acumulado sin lanzar.
    }
    return hints;
}

} // namespace lsp
