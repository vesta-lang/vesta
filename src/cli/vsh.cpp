/**
 * @file vsh.cpp
 * @brief Implementacion completa del motor VestaShell (.vsh).
 *
 * Contiene en orden:
 *   1. Metodos de Value (truthy, to_string, operadores)
 *   2. VshEnv (get/define/assign/has)
 *   3. VshLexer (tokenizacion + string interpolation)
 *   4. VshParser (descenso recursivo)
 *   5. VshInterpreter (evaluacion + built-ins + integracion REPL)
 */
#include "cli/vsh.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <cassert>
#include <thread>
#include <chrono>
#include <cstdio>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#else
#  include <glob.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <fcntl.h>
#endif
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <mutex>
#include <atomic>

// ============================================================
// Estado global de sockets para los builtins de red VSH
// ============================================================

namespace {

#if defined(_WIN32)
    typedef SOCKET vsh_sock_t;
    static constexpr vsh_sock_t VSH_BAD_SOCK = INVALID_SOCKET;
    static void vsh_sock_close(vsh_sock_t s) { closesocket(s); }
    static bool vsh_sock_valid(vsh_sock_t s) { return s != INVALID_SOCKET; }
    static void ensure_wsa() {
        static bool done = false;
        if (!done) { WSADATA wd; WSAStartup(MAKEWORD(2,2), &wd); done = true; }
    }
#else
    typedef int vsh_sock_t;
    static constexpr vsh_sock_t VSH_BAD_SOCK = -1;
    static void vsh_sock_close(vsh_sock_t s) { ::close(s); }
    static bool vsh_sock_valid(vsh_sock_t s) { return s >= 0; }
    static void ensure_wsa() {}
#endif

    struct VshSockState {
        vsh_sock_t fd  = VSH_BAD_SOCK;
        SSL       *ssl = nullptr;
        bool    is_udp = false;
    };

    static std::mutex              g_sock_mutex;
    static std::unordered_map<int64_t, VshSockState> g_sockets;
    static int64_t                 g_sock_next_id = 1;

    static SSL_CTX *g_ssl_ctx = nullptr;
    static std::once_flag g_ssl_init_flag;

    static void ensure_ssl_ctx() {
        std::call_once(g_ssl_init_flag, [](){
            SSL_library_init();
            SSL_load_error_strings();
            OpenSSL_add_ssl_algorithms();
            g_ssl_ctx = SSL_CTX_new(TLS_client_method());
            if (g_ssl_ctx)
                SSL_CTX_set_verify(g_ssl_ctx, SSL_VERIFY_NONE, nullptr);
        });
    }

    static int64_t vsh_alloc_sock(vsh_sock_t fd, bool is_udp = false, SSL *ssl = nullptr) {
        std::lock_guard<std::mutex> lk(g_sock_mutex);
        int64_t id = g_sock_next_id++;
        g_sockets[id] = {fd, ssl, is_udp};
        return id;
    }

    static VshSockState *vsh_get_sock(int64_t id) {
        auto it = g_sockets.find(id);
        return it == g_sockets.end() ? nullptr : &it->second;
    }

    static void vsh_free_sock(int64_t id) {
        std::lock_guard<std::mutex> lk(g_sock_mutex);
        auto it = g_sockets.find(id);
        if (it == g_sockets.end()) return;
        auto &s = it->second;
        if (s.ssl) { SSL_shutdown(s.ssl); SSL_free(s.ssl); }
        if (vsh_sock_valid(s.fd)) vsh_sock_close(s.fd);
        g_sockets.erase(it);
    }

    static void vsh_resolve(const std::string &host, uint16_t port, struct sockaddr_in &out) {
        memset(&out, 0, sizeof(out));
        out.sin_family = AF_INET;
        out.sin_port   = htons(port);
        struct addrinfo hints{};
        memset(&hints, 0, sizeof(hints));
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo *res = nullptr;
        int r = getaddrinfo(host.c_str(), nullptr, &hints, &res);
        if (r != 0 || !res) {
            if (res) freeaddrinfo(res);
            throw vsh::VshRuntimeError("no se pudo resolver host: " + host);
        }
        out.sin_addr = reinterpret_cast<struct sockaddr_in*>(res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }

    // Peticion HTTP/HTTPS generica: metodo, cuerpo y tipo de contenido opcionales.
    // Devuelve [status_code, headers_str, body_str] como tres strings concatenados
    // con separador "\x00" — o solo el body si split_response=false.
    struct VshHttpResp {
        int         status = 0;
        std::string headers;
        std::string body;
    };

    static VshHttpResp vsh_http_request(
            const std::string &method,
            const std::string &host, uint16_t port,
            const std::string &path, bool use_tls,
            const std::string &body        = "",
            const std::string &content_type = "application/x-www-form-urlencoded",
            const std::string &extra_headers = "") {
        ensure_wsa();
        struct sockaddr_in addr{};
        vsh_resolve(host, port, addr);
        vsh_sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
        if (!vsh_sock_valid(fd)) throw vsh::VshRuntimeError("error creando socket HTTP");
        if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
            vsh_sock_close(fd);
            throw vsh::VshRuntimeError("no se pudo conectar a " + host);
        }
        SSL *ssl = nullptr;
        if (use_tls) {
            ensure_ssl_ctx();
            ssl = SSL_new(g_ssl_ctx);
            SSL_set_fd(ssl, (int)fd);
            SSL_set_tlsext_host_name(ssl, host.c_str());
            if (SSL_connect(ssl) <= 0) {
                SSL_free(ssl); vsh_sock_close(fd);
                throw vsh::VshRuntimeError("fallo TLS handshake con " + host);
            }
        }
        std::string req = method + " " + path + " HTTP/1.0\r\n"
                        + "Host: " + host + "\r\n"
                        + "User-Agent: VestaShell/1.0\r\n"
                        + "Accept: */*\r\n";
        if (!body.empty()) {
            req += "Content-Type: " + content_type + "\r\n";
            req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        }
        if (!extra_headers.empty()) req += extra_headers;
        req += "Connection: close\r\n\r\n";
        if (!body.empty()) req += body;

        if (ssl) SSL_write(ssl, req.c_str(), (int)req.size());
        else     send(fd, req.c_str(), (int)req.size(), 0);

        std::string raw;
        char buf[4096];
        for (;;) {
            int n = ssl ? SSL_read(ssl, buf, sizeof(buf))
                        : (int)recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            raw.append(buf, (size_t)n);
        }
        if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); }
        vsh_sock_close(fd);

        VshHttpResp r;
        auto sep = raw.find("\r\n\r\n");
        r.headers = sep == std::string::npos ? raw   : raw.substr(0, sep);
        r.body    = sep == std::string::npos ? ""    : raw.substr(sep + 4);
        // extraer codigo de estado de la primera linea (ej: "HTTP/1.0 200 OK")
        auto sp1 = r.headers.find(' ');
        if (sp1 != std::string::npos) {
            auto sp2 = r.headers.find(' ', sp1 + 1);
            std::string code_str = sp2 == std::string::npos
                ? r.headers.substr(sp1+1) : r.headers.substr(sp1+1, sp2-sp1-1);
            try { r.status = std::stoi(code_str); } catch (...) {}
        }
        return r;
    }

    static std::pair<std::string,uint16_t> vsh_parse_url(const std::string &url,
                                                          bool &use_tls,
                                                          std::string &path_out) {
        use_tls = false;
        std::string work = url;
        if (work.substr(0,8) == "https://") { use_tls = true; work = work.substr(8); }
        else if (work.substr(0,7) == "http://") work = work.substr(7);
        auto slash = work.find('/');
        std::string hostport = slash == std::string::npos ? work : work.substr(0, slash);
        path_out = slash == std::string::npos ? "/" : work.substr(slash);
        auto colon = hostport.rfind(':');
        if (colon != std::string::npos) {
            uint16_t p = (uint16_t)std::stoi(hostport.substr(colon+1));
            return {hostport.substr(0, colon), p};
        }
        return {hostport, use_tls ? uint16_t(443) : uint16_t(80)};
    }

} // anonymous namespace

namespace vsh {

// ============================================================
// Seccion 1: Value
// ============================================================

VshType Value::type() const noexcept {
    return static_cast<VshType>(data.index());
}

bool Value::truthy() const noexcept {
    switch (type()) {
        case VshType::Null:     return false;
        case VshType::Bool:     return as_bool();
        case VshType::Int:      return as_int() != 0;
        case VshType::Float:    return as_float() != 0.0;
        case VshType::String:   return !as_string().empty();
        case VshType::List:     return !as_list()->empty();
        case VshType::Map:      return !as_map()->empty();
        case VshType::Function: return true;
        case VshType::Class:    return true;
        case VshType::Instance: return true;
    }
    return false;
}

double Value::as_number() const {
    if (is_int())   return static_cast<double>(as_int());
    if (is_float()) return as_float();
    throw VshRuntimeError("se esperaba un numero, se obtuvo " +
                          std::string(is_string() ? "string" : "otro tipo"));
}

std::string Value::to_string() const {
    switch (type()) {
        case VshType::Null:   return "null";
        case VshType::Bool:   return as_bool() ? "true" : "false";
        case VshType::Int:    return std::to_string(as_int());
        case VshType::Float: {
            std::ostringstream oss;
            oss << as_float();
            std::string s = oss.str();
            if (s.find('.') == std::string::npos && s.find('e') == std::string::npos)
                s += ".0"; // garantizar que parezca flotante
            return s;
        }
        case VshType::String:   return as_string();
        case VshType::List: {
            std::string r = "[";
            auto &v = *as_list();
            for (size_t i = 0; i < v.size(); ++i) {
                if (i) r += ", ";
                r += v[i].to_repr();
            }
            return r + "]";
        }
        case VshType::Map: {
            std::string r = "{";
            bool first = true;
            for (auto &[k, v] : *as_map()) {
                if (!first) r += ", ";
                r += "\"" + k + "\": " + v.to_repr();
                first = false;
            }
            return r + "}";
        }
        case VshType::Function: {
            auto fn = as_fn();
            return "<function " + (fn->name.empty() ? "<lambda>" : fn->name) + ">";
        }
        case VshType::Class: {
            auto cls = as_class();
            return "<class " + cls->name + ">";
        }
        case VshType::Instance: {
            auto inst = as_instance();
            // si tiene __str__, se llamara desde eval; aqui retornamos representacion por defecto
            return "<" + inst->klass->name + " object>";
        }
    }
    return "null";
}

std::string Value::to_repr() const {
    if (is_string()) return "\"" + as_string() + "\"";
    return to_string();
}

bool Value::operator==(const Value &o) const noexcept {
    if (type() != o.type()) {
        // int y float son iguales si el valor numerico coincide
        if (is_numeric() && o.is_numeric())
            return as_number() == o.as_number();
        return false;
    }
    switch (type()) {
        case VshType::Null:   return true;
        case VshType::Bool:   return as_bool()  == o.as_bool();
        case VshType::Int:    return as_int()   == o.as_int();
        case VshType::Float:  return as_float() == o.as_float();
        case VshType::String: return as_string() == o.as_string();
        default:              return false; // list/map/fn: identidad de puntero
    }
}

bool Value::operator<(const Value &o) const {
    if (is_numeric() && o.is_numeric()) return as_number() < o.as_number();
    if (is_string()  && o.is_string())  return as_string() < o.as_string();
    throw VshRuntimeError("tipos no comparables: " +
                          std::string(is_null()?"null":is_bool()?"bool":
                                      is_list()?"list":is_map()?"map":"funcion") +
                          " y " +
                          std::string(o.is_null()?"null":o.is_bool()?"bool":
                                      o.is_list()?"list":o.is_map()?"map":"funcion"));
}

// ============================================================
// Seccion 2: VshEnv
// ============================================================

Value& VshEnv::get(const std::string &name) {
    auto it = vars.find(name);
    if (it != vars.end()) return it->second;
    if (parent) return parent->get(name);
    throw VshRuntimeError("variable no definida: '" + name + "'");
}

void VshEnv::define(const std::string &name, Value v) {
    vars[name] = std::move(v);
}

void VshEnv::assign(const std::string &name, Value v) {
    auto it = vars.find(name);
    if (it != vars.end()) { it->second = std::move(v); return; }
    if (parent) { parent->assign(name, std::move(v)); return; }
    throw VshRuntimeError("asignacion a variable no declarada: '" + name +
                          "' (usa 'let " + name + " = ...' para declararla)");
}

bool VshEnv::has(const std::string &name) const noexcept {
    if (vars.count(name)) return true;
    return parent ? parent->has(name) : false;
}

std::shared_ptr<VshEnv> VshEnv::make_child() {
    return std::make_shared<VshEnv>(shared_from_this());
}

// ============================================================
// Seccion 3: VshLexer
// ============================================================

const std::unordered_map<std::string, TK>& VshLexer::kw_map() {
    static const std::unordered_map<std::string, TK> m = {
        {"let",      TK::KW_LET},     {"fn",       TK::KW_FN},
        {"return",   TK::KW_RETURN},  {"if",       TK::KW_IF},
        {"elif",     TK::KW_ELIF},    {"else",     TK::KW_ELSE},
        {"while",    TK::KW_WHILE},   {"for",      TK::KW_FOR},
        {"in",       TK::KW_IN},      {"break",    TK::KW_BREAK},
        {"continue", TK::KW_CONTINUE},{"try",      TK::KW_TRY},
        {"catch",    TK::KW_CATCH},   {"import",   TK::KW_IMPORT},
        {"and",      TK::KW_AND},     {"or",       TK::KW_OR},
        {"not",      TK::KW_NOT},     {"true",     TK::BOOL_LIT},
        {"false",    TK::BOOL_LIT},   {"null",     TK::NULL_LIT},
        {"class",    TK::KW_CLASS},   {"super",    TK::KW_SUPER},
        {"throw",    TK::KW_THROW},
    };
    return m;
}

VshLexer::VshLexer(std::string source, std::string filename)
    : src_(std::move(source)), filename_(std::move(filename)) {}

char VshLexer::cur()   const { return pos_ < src_.size() ? src_[pos_] : '\0'; }
char VshLexer::peek1() const { return (pos_+1) < src_.size() ? src_[pos_+1] : '\0'; }

char VshLexer::advance() {
    char c = src_[pos_++];
    if (c == '\n') { ++line_; col_ = 1; } else { ++col_; }
    return c;
}

bool VshLexer::match(char c) {
    if (cur() != c) return false;
    advance(); return true;
}

void VshLexer::skip_ws_and_comments() {
    while (pos_ < src_.size()) {
        char c = cur();
        if (c == ' ' || c == '\t' || c == '\r') { advance(); continue; }
        // comentario de linea: // hasta fin de linea
        if (c == '/' && peek1() == '/') {
            while (pos_ < src_.size() && cur() != '\n') advance();
            continue;
        }
        // comentario de bloque: /* ... */ (puede abarcar multiples lineas)
        if (c == '/' && peek1() == '*') {
            advance(); advance(); // consume '/*'
            while (pos_ < src_.size()) {
                if (cur() == '*' && peek1() == '/') { advance(); advance(); break; }
                advance(); // advance() ya actualiza line_/col_ ante '\n'
            }
            continue;
        }
        // barra invertida + newline = continuacion de linea
        if (c == '\\' && peek1() == '\n') { advance(); advance(); continue; }
        break;
    }
}

Token VshLexer::make_tok(TK k, std::string lex) const {
    Token t; t.kind = k; t.lexeme = std::move(lex); t.line = line_; t.col = col_;
    return t;
}

bool VshLexer::at_end() const {
    return buf_.empty() && pos_ >= src_.size();
}

const Token& VshLexer::peek(int offset) {
    while ((int)buf_.size() <= offset) buf_.push_back(read_one_token());
    return buf_[offset];
}

Token VshLexer::next() {
    if (!buf_.empty()) { Token t = buf_.front(); buf_.pop_front(); return t; }
    return read_one_token();
}

Token VshLexer::read_number() {
    int sline = line_, scol = col_;
    std::string lex;
    bool is_float = false;
    while (pos_ < src_.size() && (std::isdigit(cur()) || cur() == '_')) {
        if (cur() != '_') lex += cur();
        advance();
    }
    if (cur() == '.' && std::isdigit(peek1())) {
        is_float = true;
        lex += advance(); // '.'
        while (pos_ < src_.size() && (std::isdigit(cur()) || cur() == '_')) {
            if (cur() != '_') lex += cur();
            advance();
        }
    }
    if (cur() == 'e' || cur() == 'E') {
        is_float = true;
        lex += advance();
        if (cur() == '+' || cur() == '-') lex += advance();
        while (std::isdigit(cur())) { lex += cur(); advance(); }
    }
    Token t; t.line = sline; t.col = scol;
    if (is_float) {
        t.kind = TK::FLOAT_LIT; t.lexeme = lex;
        t.flt_val = std::stod(lex);
    } else {
        t.kind = TK::INT_LIT; t.lexeme = lex;
        t.int_val = std::stoll(lex, nullptr, 0);
    }
    return t;
}

// empuja en buf_ la secuencia ISTR_TEXT / ISTR_EXPR_BEGIN ... ISTR_EXPR_END / ISTR_END
void VshLexer::push_interp_tokens(const std::string &raw, int base_line, int base_col) {
    std::string text;
    size_t i = 0;

    auto flush_text = [&]() {
        if (text.empty()) return;
        Token t; t.kind = TK::ISTR_TEXT; t.lexeme = text;
        t.line = base_line; t.col = base_col;
        buf_.push_back(std::move(t));
        text.clear();
    };

    while (i < raw.size()) {
        // escape sequences
        if (raw[i] == '\\' && i+1 < raw.size()) {
            char e = raw[i+1];
            switch (e) {
                case 'n':  text += '\n'; break;
                case 't':  text += '\t'; break;
                case 'r':  text += '\r'; break;
                case '"':  text += '"';  break;
                case '\\': text += '\\'; break;
                case '$':  text += '$';  break; // \$ literal
                default:   text += '\\'; text += e; break;
            }
            i += 2; continue;
        }
        // inicio de expresion embebida
        if (raw[i] == '$' && i+1 < raw.size() && raw[i+1] == '{') {
            flush_text();
            // token ISTR_EXPR_BEGIN
            Token tb; tb.kind = TK::ISTR_EXPR_BEGIN;
            tb.line = base_line; tb.col = base_col; buf_.push_back(tb);
            // extraer hasta el } balanceado
            i += 2;
            size_t depth = 1;
            std::string expr_src;
            while (i < raw.size() && depth > 0) {
                if (raw[i] == '{')  ++depth;
                else if (raw[i] == '}') { --depth; if (depth == 0) { ++i; break; } }
                // cadenas dentro de la expresion: copiar opacamente
                if ((raw[i] == '"' || raw[i] == '\'') && depth > 0) {
                    char q = raw[i++];
                    expr_src += q;
                    while (i < raw.size() && raw[i] != q) {
                        if (raw[i] == '\\' && i+1 < raw.size()) { expr_src += raw[i++]; }
                        expr_src += raw[i++];
                    }
                    if (i < raw.size()) { expr_src += raw[i++]; } // cierre comilla
                    continue;
                }
                expr_src += raw[i++];
            }
            // sub-lexer sobre la expresion
            VshLexer sub(expr_src, filename_);
            while (!sub.at_end()) {
                Token st = sub.next();
                if (st.kind == TK::END_OF_FILE) break;
                st.line = base_line; st.col = base_col;
                buf_.push_back(std::move(st));
            }
            // token ISTR_EXPR_END
            Token te; te.kind = TK::ISTR_EXPR_END;
            te.line = base_line; te.col = base_col; buf_.push_back(te);
            continue;
        }
        text += raw[i++];
    }
    flush_text();
    // token ISTR_END
    Token tend; tend.kind = TK::ISTR_END;
    tend.line = base_line; tend.col = base_col; buf_.push_back(tend);
}

Token VshLexer::read_string_or_interp() {
    int sline = line_, scol = col_;
    char quote = advance(); // consume '"' o '\''
    std::string raw;
    bool has_interp = false;
    while (pos_ < src_.size() && cur() != quote) {
        if (cur() == '\\') {
            raw += advance();
            if (pos_ < src_.size()) raw += advance();
            continue;
        }
        if (cur() == '$' && peek1() == '{') has_interp = true;
        raw += advance();
    }
    if (pos_ < src_.size()) advance(); // consume cierre de comilla
    if (!has_interp) {
        // string simple: procesar escapes
        std::string out;
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\\' && i+1 < raw.size()) {
                char e = raw[i+1]; ++i;
                switch (e) {
                    case 'n':  out += '\n'; break;
                    case 't':  out += '\t'; break;
                    case 'r':  out += '\r'; break;
                    case '"':  out += '"';  break;
                    case '\'': out += '\''; break;
                    case '\\': out += '\\'; break;
                    default:   out += '\\'; out += e; break;
                }
            } else {
                out += raw[i];
            }
        }
        Token t; t.kind = TK::STRING_LIT; t.lexeme = out; t.line = sline; t.col = scol;
        return t;
    }
    // string interpolada: genera tokens en buf_ y devuelve un marcador especial
    Token marker; marker.kind = TK::ISTR_TEXT; marker.line = sline; marker.col = scol;
    // marcamos con lexeme vacio para que el parser sepa que empieza aqui
    // En realidad el primer token de la interpolacion ya estara en buf_
    push_interp_tokens(raw, sline, scol);
    // El parser vera ISTR_TEXT (o ISTR_EXPR_BEGIN como primer token) seguido de ISTR_END
    // Devolvemos un token ficticio que el parser identifica como inicio de interp
    // Usaremos ISTR_TEXT con lexeme especial "" como "ya esta en buf_"
    // En realidad simplemente devolvemos el primer token del buf_
    Token first = buf_.front(); buf_.pop_front();
    return first;
}

Token VshLexer::read_ident_kw() {
    int sline = line_, scol = col_;
    std::string lex;
    while (pos_ < src_.size() && (std::isalnum(cur()) || cur() == '_')) {
        lex += advance();
    }
    auto &km = kw_map();
    auto it = km.find(lex);
    Token t; t.line = sline; t.col = scol; t.lexeme = lex;
    if (it != km.end()) {
        t.kind = it->second;
        if (t.kind == TK::BOOL_LIT) t.int_val = (lex == "true") ? 1 : 0;
    } else {
        t.kind = TK::IDENT;
    }
    return t;
}

Token VshLexer::read_one_token() {
    skip_ws_and_comments();
    if (pos_ >= src_.size()) return make_tok(TK::END_OF_FILE, "<eof>");

    int sline = line_, scol = col_;
    char c = cur();

    // Newline: terminador de sentencia
    if (c == '\n') { advance(); Token t = make_tok(TK::NEWLINE, "\\n"); t.line=sline; t.col=scol; return t; }

    if (std::isdigit(c)) return read_number();
    if (c == '"' || c == '\'') return read_string_or_interp();
    if (std::isalpha(c) || c == '_') return read_ident_kw();

    advance(); // consumir el caracter
    Token t; t.line = sline; t.col = scol;

    switch (c) {
        case '+': t.kind = match('=') ? TK::PLUS_EQ  : TK::PLUS;     break;
        case '-': t.kind = match('=') ? TK::MINUS_EQ : TK::MINUS;    break;
        case '*':
            if (match('*')) t.kind = TK::STARSTAR;
            else if (match('=')) t.kind = TK::STAR_EQ;
            else t.kind = TK::STAR;
            break;
        case '/': t.kind = match('=') ? TK::SLASH_EQ  : TK::SLASH;   break;
        case '%': t.kind = match('=') ? TK::PERCENT_EQ: TK::PERCENT; break;
        case '=': t.kind = match('=') ? TK::EQ_EQ     : TK::EQ;      break;
        case '!': t.kind = match('=') ? TK::BANG_EQ   : TK::LEX_ERROR; break;
        case '<': t.kind = match('=') ? TK::LT_EQ     : TK::LT;      break;
        case '>': t.kind = match('=') ? TK::GT_EQ     : TK::GT;      break;
        case '(': t.kind = TK::LPAREN;    break;
        case ')': t.kind = TK::RPAREN;    break;
        case '{': t.kind = TK::LBRACE;    break;
        case '}': t.kind = TK::RBRACE;    break;
        case '[': t.kind = TK::LBRACKET;  break;
        case ']': t.kind = TK::RBRACKET;  break;
        case ',': t.kind = TK::COMMA;     break;
        case ':': t.kind = TK::COLON;     break;
        case '.': t.kind = TK::DOT;       break;
        case ';': t.kind = TK::SEMICOLON; break;
        default:
            t.kind = TK::LEX_ERROR;
            t.lexeme = std::string(1, c);
            break;
    }
    t.lexeme = std::string(1, c);
    return t;
}

// ============================================================
// Seccion 4: VshParser
// ============================================================

// Acepta como nombre de tipo: IDENT o ciertas palabras clave (fn, null, bool, ...)
static bool is_type_name_token(TK k) {
    return k == TK::IDENT  || k == TK::KW_FN || k == TK::NULL_LIT ||
           k == TK::BOOL_LIT;
}

VshParser::VshParser(VshLexer &lex) : lex_(lex) {
    cur_  = lex_.next();
    peek_ = lex_.next();
}

Token VshParser::advance() {
    Token t = cur_;
    cur_  = peek_;
    peek_ = lex_.next();
    return t;
}

bool VshParser::check(TK k) const { return cur_.kind == k; }

bool VshParser::match(TK k) {
    if (!check(k)) return false;
    advance(); return true;
}

Token VshParser::expect(TK k, const char *msg) {
    if (!check(k)) {
        throw VshParseError(
            std::string(msg) + " (se obtuvo '" + cur_.lexeme + "')",
            cur_.line, cur_.col);
    }
    return advance();
}

void VshParser::skip_nl() {
    while (check(TK::NEWLINE) || check(TK::SEMICOLON)) advance();
}

void VshParser::end_stmt() {
    // termina sentencia con NEWLINE, ';', o implicitamente antes de '}'
    if (check(TK::NEWLINE) || check(TK::SEMICOLON)) { advance(); return; }
    if (check(TK::RBRACE) || check(TK::END_OF_FILE)) return;
    throw VshParseError("se esperaba fin de sentencia (nueva linea o ';')",
                        cur_.line, cur_.col);
}

AstNodePtr VshParser::parse_program() {
    auto n = make_node(AstKind::Program, cur_.line, cur_.col);
    skip_nl();
    while (!check(TK::END_OF_FILE)) {
        n->args.push_back(stmt());
        skip_nl();
    }
    return n;
}

AstNodePtr VshParser::stmt() {
    skip_nl();
    if (check(TK::KW_LET))      return let_decl();
    if (check(TK::KW_FN))       return fn_decl();
    if (check(TK::KW_IF))       return if_stmt();
    if (check(TK::KW_WHILE))    return while_stmt();
    if (check(TK::KW_FOR))      return for_stmt();
    if (check(TK::KW_TRY))      return try_stmt();
    if (check(TK::KW_RETURN))   return ret_stmt();
    if (check(TK::KW_BREAK))    return brk_stmt();
    if (check(TK::KW_CONTINUE)) return cnt_stmt();
    if (check(TK::KW_IMPORT))   return import_stmt();
    if (check(TK::KW_CLASS))    return class_decl();
    if (check(TK::KW_THROW))    return throw_stmt();
    return expr_stmt();
}

AstNodePtr VshParser::let_decl() {
    auto n = make_node(AstKind::LetDecl, cur_.line, cur_.col);
    advance(); // 'let'
    n->name = expect(TK::IDENT, "se esperaba nombre de variable despues de 'let'").lexeme;
    if (match(TK::COLON)) {
        if (!is_type_name_token(cur_.kind))
            throw VshParseError("se esperaba nombre de tipo despues de ':'", cur_.line, cur_.col);
        n->type_hint = cur_.lexeme; advance();
    }
    if (match(TK::EQ)) {
        n->right = expr();
    }
    end_stmt();
    return n;
}

std::vector<std::string> VshParser::params_list(AstNode *n) {
    std::vector<std::string> p;
    std::vector<std::string> types;
    if (check(TK::RPAREN)) {
        if (n) n->param_types = types;
        return p;
    }
    p.push_back(expect(TK::IDENT, "se esperaba nombre de parametro").lexeme);
    std::string t;
    if (match(TK::COLON)) {
        if (!is_type_name_token(cur_.kind))
            throw VshParseError("se esperaba nombre de tipo despues de ':'", cur_.line, cur_.col);
        t = cur_.lexeme; advance();
    }
    types.push_back(t);
    while (match(TK::COMMA)) {
        if (check(TK::RPAREN)) break;
        p.push_back(expect(TK::IDENT, "se esperaba nombre de parametro").lexeme);
        t = {};
        if (match(TK::COLON)) {
            if (!is_type_name_token(cur_.kind))
                throw VshParseError("se esperaba nombre de tipo despues de ':'", cur_.line, cur_.col);
            t = cur_.lexeme; advance();
        }
        types.push_back(t);
    }
    if (n) n->param_types = types;
    return p;
}

AstNodePtr VshParser::fn_decl() {
    auto n = make_node(AstKind::FnDecl, cur_.line, cur_.col);
    advance(); // 'fn'
    n->name = expect(TK::IDENT, "se esperaba nombre de funcion").lexeme;
    expect(TK::LPAREN, "se esperaba '(' en declaracion de funcion");
    n->params = params_list(n.get());
    expect(TK::RPAREN, "se esperaba ')'");
    n->body = block();
    return n;
}

AstNodePtr VshParser::if_stmt() {
    auto n = make_node(AstKind::IfStmt, cur_.line, cur_.col);
    advance(); // 'if'
    IfBranch main_branch;
    main_branch.cond = expr();
    main_branch.body = block();
    n->branches.push_back(std::move(main_branch));
    while (check(TK::KW_ELIF)) {
        advance();
        IfBranch b; b.cond = expr(); b.body = block();
        n->branches.push_back(std::move(b));
    }
    if (match(TK::KW_ELSE)) {
        IfBranch b; b.cond = nullptr; b.body = block();
        n->branches.push_back(std::move(b));
    }
    return n;
}

AstNodePtr VshParser::while_stmt() {
    auto n = make_node(AstKind::WhileStmt, cur_.line, cur_.col);
    advance(); // 'while'
    n->cond = expr();
    n->body = block();
    return n;
}

AstNodePtr VshParser::for_stmt() {
    auto n = make_node(AstKind::ForInStmt, cur_.line, cur_.col);
    advance(); // 'for'
    n->name = expect(TK::IDENT, "se esperaba variable de iteracion").lexeme;
    expect(TK::KW_IN, "se esperaba 'in'");
    n->cond = expr(); // expresion iterable (reutiliza campo cond)
    n->body = block();
    return n;
}

AstNodePtr VshParser::try_stmt() {
    auto n = make_node(AstKind::TryCatch, cur_.line, cur_.col);
    advance(); // 'try'
    n->try_body = block();
    expect(TK::KW_CATCH, "se esperaba 'catch' despues del bloque 'try'");
    n->name = expect(TK::IDENT, "se esperaba nombre de variable de error en catch").lexeme;
    if (match(TK::COLON)) {
        n->catch_type_hint = expect(TK::IDENT, "se esperaba nombre de tipo en catch tipado").lexeme;
    }
    n->catch_body = block();
    return n;
}

AstNodePtr VshParser::ret_stmt() {
    auto n = make_node(AstKind::Return, cur_.line, cur_.col);
    advance(); // 'return'
    if (!check(TK::NEWLINE) && !check(TK::SEMICOLON) && !check(TK::RBRACE) && !check(TK::END_OF_FILE))
        n->right = expr();
    end_stmt();
    return n;
}

AstNodePtr VshParser::brk_stmt() {
    auto n = make_node(AstKind::Break, cur_.line, cur_.col);
    advance(); end_stmt(); return n;
}

AstNodePtr VshParser::cnt_stmt() {
    auto n = make_node(AstKind::Continue, cur_.line, cur_.col);
    advance(); end_stmt(); return n;
}

AstNodePtr VshParser::import_stmt() {
    auto n = make_node(AstKind::Import, cur_.line, cur_.col);
    advance(); // 'import'
    auto tok = expect(TK::STRING_LIT, "se esperaba ruta de fichero en 'import'");
    n->name = tok.lexeme;
    end_stmt();
    return n;
}

AstNodePtr VshParser::class_decl() {
    auto n = make_node(AstKind::ClassDecl, cur_.line, cur_.col);
    advance(); // 'class'
    n->name = expect(TK::IDENT, "se esperaba nombre de clase").lexeme;
    if (match(TK::COLON)) {
        n->parent_class = expect(TK::IDENT, "se esperaba nombre de clase padre").lexeme;
    }
    skip_nl();
    expect(TK::LBRACE, "se esperaba '{' en cuerpo de clase");
    skip_nl();
    while (!check(TK::RBRACE) && !check(TK::END_OF_FILE)) {
        if (check(TK::KW_FN)) {
            n->args.push_back(fn_decl());
        } else if (check(TK::STRING_LIT) || check(TK::ISTR_TEXT) || check(TK::ISTR_EXPR_BEGIN)) {
            // docstring de clase: primer elemento string literal
            n->args.push_back(expr_stmt());
        } else {
            throw VshParseError("se esperaba 'fn' o docstring dentro del cuerpo de clase",
                                cur_.line, cur_.col);
        }
        skip_nl();
    }
    expect(TK::RBRACE, "se esperaba '}' para cerrar cuerpo de clase");
    return n;
}

AstNodePtr VshParser::throw_stmt() {
    auto n = make_node(AstKind::ThrowStmt, cur_.line, cur_.col);
    advance(); // 'throw'
    n->right = expr();
    end_stmt();
    return n;
}

AstNodePtr VshParser::block() {
    skip_nl();
    auto n = make_node(AstKind::Block, cur_.line, cur_.col);
    expect(TK::LBRACE, "se esperaba '{' para abrir bloque");
    skip_nl();
    while (!check(TK::RBRACE) && !check(TK::END_OF_FILE)) {
        n->args.push_back(stmt());
        skip_nl();
    }
    expect(TK::RBRACE, "se esperaba '}' para cerrar bloque");
    return n;
}

AstNodePtr VshParser::expr_stmt() {
    auto n = make_node(AstKind::ExprStmt, cur_.line, cur_.col);
    n->left = expr();
    end_stmt();
    return n;
}

// ---- Expresiones ----

AstNodePtr VshParser::expr()      { return assign_expr(); }
AstNodePtr VshParser::assign_expr() {
    auto lhs = or_expr();
    // asignacion: target = value  o  target OP= value
    static const TK compound[] = {TK::PLUS_EQ,TK::MINUS_EQ,TK::STAR_EQ,TK::SLASH_EQ,TK::PERCENT_EQ};
    for (auto op : compound) {
        if (check(op)) {
            TK op2 = cur_.kind; int ln = cur_.line, co = cur_.col; advance();
            auto val = expr();
            auto n = make_node(AstKind::CompoundAssign, ln, co);
            n->op = op2; n->left = lhs; n->right = val;
            return n;
        }
    }
    if (check(TK::EQ)) {
        int ln = cur_.line, co = cur_.col; advance();
        auto val = expr();
        auto n = make_node(AstKind::Assign, ln, co);
        n->left = lhs; n->right = val;
        return n;
    }
    return lhs;
}

AstNodePtr VshParser::or_expr() {
    auto lhs = and_expr();
    while (check(TK::KW_OR)) {
        TK op = cur_.kind; int ln = cur_.line, co = cur_.col; advance();
        auto rhs = and_expr();
        auto n = make_node(AstKind::BinOp, ln, co);
        n->op = op; n->left = lhs; n->right = rhs; lhs = n;
    }
    return lhs;
}

AstNodePtr VshParser::and_expr() {
    auto lhs = not_expr();
    while (check(TK::KW_AND)) {
        TK op = cur_.kind; int ln = cur_.line, co = cur_.col; advance();
        auto rhs = not_expr();
        auto n = make_node(AstKind::BinOp, ln, co);
        n->op = op; n->left = lhs; n->right = rhs; lhs = n;
    }
    return lhs;
}

AstNodePtr VshParser::not_expr() {
    if (check(TK::KW_NOT)) {
        int ln = cur_.line, co = cur_.col; TK op = cur_.kind; advance();
        auto n = make_node(AstKind::UnaryOp, ln, co);
        n->op = op; n->left = not_expr(); return n;
    }
    return cmp_expr();
}

AstNodePtr VshParser::cmp_expr() {
    auto lhs = add_expr();
    while (check(TK::EQ_EQ)||check(TK::BANG_EQ)||check(TK::LT)||check(TK::GT)||
           check(TK::LT_EQ)||check(TK::GT_EQ)) {
        TK op = cur_.kind; int ln = cur_.line, co = cur_.col; advance();
        auto rhs = add_expr();
        auto n = make_node(AstKind::BinOp, ln, co);
        n->op = op; n->left = lhs; n->right = rhs; lhs = n;
    }
    return lhs;
}

AstNodePtr VshParser::add_expr() {
    auto lhs = mul_expr();
    while (check(TK::PLUS) || check(TK::MINUS)) {
        TK op = cur_.kind; int ln = cur_.line, co = cur_.col; advance();
        auto rhs = mul_expr();
        auto n = make_node(AstKind::BinOp, ln, co);
        n->op = op; n->left = lhs; n->right = rhs; lhs = n;
    }
    return lhs;
}

AstNodePtr VshParser::mul_expr() {
    auto lhs = pow_expr();
    while (check(TK::STAR)||check(TK::SLASH)||check(TK::PERCENT)) {
        TK op = cur_.kind; int ln = cur_.line, co = cur_.col; advance();
        auto rhs = pow_expr();
        auto n = make_node(AstKind::BinOp, ln, co);
        n->op = op; n->left = lhs; n->right = rhs; lhs = n;
    }
    return lhs;
}

AstNodePtr VshParser::pow_expr() {
    auto lhs = unary_expr();
    if (check(TK::STARSTAR)) {
        TK op = cur_.kind; int ln = cur_.line, co = cur_.col; advance();
        auto rhs = pow_expr(); // asociatividad derecha: recursion
        auto n = make_node(AstKind::BinOp, ln, co);
        n->op = op; n->left = lhs; n->right = rhs; return n;
    }
    return lhs;
}

AstNodePtr VshParser::unary_expr() {
    if (check(TK::MINUS) || check(TK::KW_NOT)) {
        TK op = cur_.kind; int ln = cur_.line, co = cur_.col; advance();
        auto n = make_node(AstKind::UnaryOp, ln, co);
        n->op = op; n->left = unary_expr(); return n;
    }
    return postfix_expr();
}

std::vector<AstNodePtr> VshParser::args_list() {
    std::vector<AstNodePtr> v;
    if (check(TK::RPAREN)) return v;
    v.push_back(expr());
    while (match(TK::COMMA)) {
        if (check(TK::RPAREN)) break; // trailing comma
        v.push_back(expr());
    }
    return v;
}

AstNodePtr VshParser::postfix_expr() {
    auto lhs = primary();
    for (;;) {
        if (check(TK::LPAREN)) {
            int ln = cur_.line, co = cur_.col; advance();
            auto n = make_node(AstKind::Call, ln, co);
            n->callee = lhs;
            n->args   = args_list();
            expect(TK::RPAREN, "se esperaba ')' en llamada a funcion");
            lhs = n;
        } else if (check(TK::LBRACKET)) {
            int ln = cur_.line, co = cur_.col; advance();
            auto n = make_node(AstKind::Index, ln, co);
            n->left  = lhs;
            n->right = expr();
            expect(TK::RBRACKET, "se esperaba ']' en indexado");
            lhs = n;
        } else if (check(TK::DOT)) {
            int ln = cur_.line, co = cur_.col; advance();
            std::string field = expect(TK::IDENT, "se esperaba nombre de campo despues de '.'").lexeme;
            auto n = make_node(AstKind::FieldAccess, ln, co);
            n->left = lhs;
            n->name = field;
            lhs = n;
        } else {
            break;
        }
    }
    return lhs;
}

AstNodePtr VshParser::fn_expr() {
    auto n = make_node(AstKind::FnExpr, cur_.line, cur_.col);
    advance(); // 'fn'
    expect(TK::LPAREN, "se esperaba '('");
    n->params = params_list(n.get());
    expect(TK::RPAREN, "se esperaba ')'");
    n->body = block();
    return n;
}

AstNodePtr VshParser::list_lit() {
    auto n = make_node(AstKind::ListLit, cur_.line, cur_.col);
    advance(); // '['
    skip_nl();
    if (!check(TK::RBRACKET)) {
        n->args.push_back(expr());
        skip_nl();
        while (match(TK::COMMA)) {
            skip_nl();
            if (check(TK::RBRACKET)) break;
            n->args.push_back(expr());
            skip_nl();
        }
    }
    expect(TK::RBRACKET, "se esperaba ']' al cerrar lista");
    return n;
}

AstNodePtr VshParser::map_lit() {
    auto n = make_node(AstKind::MapLit, cur_.line, cur_.col);
    advance(); // '{'
    skip_nl();
    if (!check(TK::RBRACE)) {
        // pares key: valor; la key puede ser STRING_LIT o IDENT
        auto read_key = [&]() -> AstNodePtr {
            if (check(TK::STRING_LIT)) {
                auto kn = make_node(AstKind::Literal, cur_.line, cur_.col);
                kn->lit_val = Value(cur_.lexeme); advance(); return kn;
            } else if (check(TK::IDENT)) {
                auto kn = make_node(AstKind::Literal, cur_.line, cur_.col);
                kn->lit_val = Value(cur_.lexeme); advance(); return kn;
            }
            throw VshParseError("se esperaba clave de mapa (string o identificador)",
                                 cur_.line, cur_.col);
        };
        n->args.push_back(read_key());
        expect(TK::COLON, "se esperaba ':' despues de clave de mapa");
        n->args.push_back(expr());
        skip_nl();
        while (match(TK::COMMA)) {
            skip_nl();
            if (check(TK::RBRACE)) break;
            n->args.push_back(read_key());
            expect(TK::COLON, "se esperaba ':' despues de clave de mapa");
            n->args.push_back(expr());
            skip_nl();
        }
    }
    expect(TK::RBRACE, "se esperaba '}' al cerrar mapa");
    return n;
}

AstNodePtr VshParser::interp_string() {
    // El primer token ya esta en cur_ y es ISTR_TEXT o ISTR_EXPR_BEGIN o ISTR_END
    // Construimos un nodo InterpString cuyas partes son:
    //   - Literal(string) para ISTR_TEXT
    //   - sub-expresiones entre ISTR_EXPR_BEGIN y ISTR_EXPR_END
    auto n = make_node(AstKind::InterpString, cur_.line, cur_.col);
    while (!check(TK::ISTR_END) && !check(TK::END_OF_FILE)) {
        if (check(TK::ISTR_TEXT)) {
            auto part = make_node(AstKind::Literal, cur_.line, cur_.col);
            part->lit_val = Value(cur_.lexeme);
            advance();
            n->args.push_back(part);
        } else if (check(TK::ISTR_EXPR_BEGIN)) {
            advance(); // consume ISTR_EXPR_BEGIN
            n->args.push_back(expr());
            expect(TK::ISTR_EXPR_END, "se esperaba '}' al cerrar expresion embebida en string");
        } else {
            break;
        }
    }
    if (check(TK::ISTR_END)) advance();
    return n;
}

AstNodePtr VshParser::primary() {
    // Literales escalares
    if (check(TK::INT_LIT)) {
        auto n = make_node(AstKind::Literal, cur_.line, cur_.col);
        n->lit_val = Value(cur_.int_val); advance(); return n;
    }
    if (check(TK::FLOAT_LIT)) {
        auto n = make_node(AstKind::Literal, cur_.line, cur_.col);
        n->lit_val = Value(cur_.flt_val); advance(); return n;
    }
    if (check(TK::STRING_LIT)) {
        auto n = make_node(AstKind::Literal, cur_.line, cur_.col);
        n->lit_val = Value(cur_.lexeme); advance(); return n;
    }
    if (check(TK::BOOL_LIT)) {
        auto n = make_node(AstKind::Literal, cur_.line, cur_.col);
        n->lit_val = Value(bool(cur_.int_val != 0)); advance(); return n;
    }
    if (check(TK::NULL_LIT)) {
        auto n = make_node(AstKind::Literal, cur_.line, cur_.col);
        advance(); return n; // lit_val queda Null por defecto
    }
    // String interpolada: el lexer ya emitio los sub-tokens en buf_
    if (check(TK::ISTR_TEXT) || check(TK::ISTR_EXPR_BEGIN)) return interp_string();
    // Solo ISTR_END (string interpolada vacia "")
    if (check(TK::ISTR_END)) {
        auto n = make_node(AstKind::Literal, cur_.line, cur_.col);
        n->lit_val = Value(std::string{}); advance(); return n;
    }
    // Identificador o 'super'
    if (check(TK::IDENT) || check(TK::KW_SUPER)) {
        auto n = make_node(AstKind::Ident, cur_.line, cur_.col);
        n->name = cur_.lexeme; advance(); return n;
    }
    // Funcion anonima
    if (check(TK::KW_FN)) return fn_expr();
    // Lista
    if (check(TK::LBRACKET)) return list_lit();
    // Mapa: '{' puede ser bloque o mapa; si le sigue STRING/IDENT ':' es mapa.
    // Salta NEWLINEs en el lookahead para soportar mapas multi-linea:
    //   { \n  "key": val, ... }
    if (check(TK::LBRACE)) {
        bool is_map = false;
        // t1: primer token no-NEWLINE tras '{'; busca en peek_ y luego en lex_.peek(n)
        TK t1  = peek_.kind;
        int off = 0; // indice en lex_.peek() a usar si peek_ es NEWLINE
        while (t1 == TK::NEWLINE || t1 == TK::SEMICOLON)
            t1 = lex_.peek(off++).kind;
        if (t1 == TK::STRING_LIT || t1 == TK::IDENT) {
            // t2: token que sigue a la clave (debe ser ':' para que sea mapa)
            TK t2 = lex_.peek(off).kind;
            while (t2 == TK::NEWLINE || t2 == TK::SEMICOLON)
                t2 = lex_.peek(++off).kind;
            if (t2 == TK::COLON) is_map = true;
        }
        if (t1 == TK::RBRACE) is_map = true; // mapa vacio {}
        if (is_map) return map_lit();
        // bloque en posicion de expresion: no permitido
        throw VshParseError("'{ }' inesperado en expresion; usa map() o lista de sentencias",
                             cur_.line, cur_.col);
    }
    // Parentesis agrupador
    if (check(TK::LPAREN)) {
        advance();
        auto n = expr();
        expect(TK::RPAREN, "se esperaba ')' para cerrar expresion entre parentesis");
        return n;
    }
    throw VshParseError("expresion inesperada: '" + cur_.lexeme + "'", cur_.line, cur_.col);
}

// ============================================================
// Seccion 5: VshInterpreter
// ============================================================

VshInterpreter::VshInterpreter(ReplDispatch dispatch)
    : global_(std::make_shared<VshEnv>()), repl_dispatch_(std::move(dispatch)) {
    register_builtins();
}

void VshInterpreter::register_builtin(const std::string &name, NativeFn fn) {
    builtins_[name] = std::move(fn);
}

void VshInterpreter::exec_file(const std::string &path) {
    std::ifstream f(path);
    if (!f) throw VshRuntimeError("no se puede abrir el fichero: " + path);
    std::string src((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    exec_string(src, path);
}

void VshInterpreter::exec_string(const std::string &src, const std::string &name) {
    VshLexer  lex(src, name);
    VshParser par(lex);
    auto ast = par.parse_program();
    exec(ast, global_);
}

/** @brief Extrae el docstring de un cuerpo de funcion (primera sentencia string literal). */
static std::string extract_docstring(const std::shared_ptr<AstNode> &body) {
    if (!body || body->args.empty()) return {};
    const auto &first = body->args[0];
    // sentencia de expresion cuyo unico contenido es un literal string
    if (first->kind == AstKind::ExprStmt && first->left &&
        first->left->kind == AstKind::Literal &&
        first->left->lit_val.is_string()) {
        return first->left->lit_val.as_string();
    }
    return {};
}

// ---- dispatch principal ----

Value VshInterpreter::eval(const AstNodePtr &n, std::shared_ptr<VshEnv> env) {
    switch (n->kind) {
        case AstKind::Literal:        return eval_literal(*n);
        case AstKind::InterpString:   return eval_interp_str(*n, env);
        case AstKind::Ident:          return eval_ident(*n, env);
        case AstKind::BinOp:          return eval_binop(*n, env);
        case AstKind::UnaryOp:        return eval_unary(*n, env);
        case AstKind::Assign:         return eval_assign(*n, env);
        case AstKind::CompoundAssign: return eval_compound(*n, env);
        case AstKind::Call:           return eval_call(*n, env);
        case AstKind::Index:          return eval_index(*n, env);
        case AstKind::ListLit:        return eval_list_lit(*n, env);
        case AstKind::MapLit:         return eval_map_lit(*n, env);
        case AstKind::FieldAccess:    return eval_field(*n, env);
        case AstKind::FnExpr: {
            auto fn = std::make_shared<VshFunction>();
            fn->params      = n->params;
            fn->param_types = n->param_types;
            fn->body        = n->body;
            fn->closure_env = env;
            fn->doc = extract_docstring(n->body);
            return Value(fn);
        }
        default:
            throw VshRuntimeError("eval() llamado con nodo no-expresion", n->line, n->col);
    }
}

void VshInterpreter::exec(const AstNodePtr &n, std::shared_ptr<VshEnv> env) {
    switch (n->kind) {
        case AstKind::Program:
        case AstKind::Block:      exec_block(*n, env);    break;
        case AstKind::ExprStmt:   eval(n->left, env);     break;
        case AstKind::LetDecl:    exec_let(*n, env);       break;
        case AstKind::FnDecl:     exec_fn_decl(*n, env);  break;
        case AstKind::ClassDecl:  exec_class_decl(*n, env); break;
        case AstKind::ThrowStmt: {
            Value thrown = n->right ? eval(n->right, env) : Value{};
            if (thrown.is_instance()) {
                auto inst = thrown.as_instance();
                std::string msg = inst->klass->name;
                auto it = inst->attrs.find("message");
                if (it != inst->attrs.end()) msg = it->second.to_string();
                throw VshInstanceError(msg, thrown, n->line, n->col);
            }
            throw VshRuntimeError(thrown.to_string(), n->line, n->col);
        }
        case AstKind::IfStmt:     exec_if(*n, env);       break;
        case AstKind::WhileStmt:  exec_while(*n, env);    break;
        case AstKind::ForInStmt:  exec_for_in(*n, env);   break;
        case AstKind::TryCatch:   exec_try(*n, env);      break;
        case AstKind::Import:     exec_import(*n, env);   break;
        case AstKind::Return: {
            Value v = n->right ? eval(n->right, env) : Value{};
            throw ReturnSignal{std::move(v)};
        }
        case AstKind::Break:    throw BreakSignal{n->line};
        case AstKind::Continue: throw ContinueSignal{n->line};
        default:
            // sentencias que son expresiones (assign, call como stmt...)
            eval(n, env);
            break;
    }
}

// ---- evaluadores de expresiones ----

Value VshInterpreter::eval_literal(const AstNode &n) {
    return n.lit_val;
}

Value VshInterpreter::eval_interp_str(const AstNode &n, std::shared_ptr<VshEnv> env) {
    std::string result;
    for (auto &part : n.args) {
        result += eval(part, env).to_string();
    }
    return Value(result);
}

Value VshInterpreter::eval_ident(const AstNode &n, std::shared_ptr<VshEnv> env) {
    return env->get(n.name);
}

Value VshInterpreter::eval_binop(const AstNode &n, std::shared_ptr<VshEnv> env) {
    // cortocircuito para and/or (evalua derecho solo si necesario)
    if (n.op == TK::KW_OR) {
        auto lv = eval(n.left, env);
        return lv.truthy() ? lv : eval(n.right, env);
    }
    if (n.op == TK::KW_AND) {
        auto lv = eval(n.left, env);
        return !lv.truthy() ? lv : eval(n.right, env);
    }
    Value lv = eval(n.left, env);
    Value rv = eval(n.right, env);
    switch (n.op) {
        case TK::PLUS:
            if (lv.is_string() || rv.is_string())
                return Value(lv.to_string() + rv.to_string());
            if (lv.is_int()   && rv.is_int())   return Value(lv.as_int()   + rv.as_int());
            if (lv.is_float() || rv.is_float())  return Value(lv.as_number() + rv.as_number());
            if (lv.is_list()) {
                auto lst = std::make_shared<std::vector<Value>>(*lv.as_list());
                for (auto &e : *rv.as_list()) lst->push_back(e);
                return Value(lst);
            }
            throw VshRuntimeError("operador '+' no soportado para " + lv.to_string(), n.line);
        case TK::MINUS:
            if (lv.is_int()   && rv.is_int())   return Value(lv.as_int() - rv.as_int());
            return Value(lv.as_number() - rv.as_number());
        case TK::STAR:
            if (lv.is_int()   && rv.is_int())   return Value(lv.as_int() * rv.as_int());
            if (lv.is_string() && rv.is_int()) {
                std::string r; for (int64_t i=0; i<rv.as_int(); ++i) r+=lv.as_string();
                return Value(r);
            }
            return Value(lv.as_number() * rv.as_number());
        case TK::SLASH:
            if (rv.is_int() && rv.as_int()==0) throw VshRuntimeError("division por cero",n.line);
            if (rv.is_float() && rv.as_float()==0.0) throw VshRuntimeError("division por cero",n.line);
            if (lv.is_int() && rv.is_int()) return Value(lv.as_int() / rv.as_int());
            return Value(lv.as_number() / rv.as_number());
        case TK::PERCENT:
            if (lv.is_int() && rv.is_int()) return Value(lv.as_int() % rv.as_int());
            return Value(std::fmod(lv.as_number(), rv.as_number()));
        case TK::STARSTAR:
            return Value(std::pow(lv.as_number(), rv.as_number()));
        case TK::EQ_EQ:   return Value(lv == rv);
        case TK::BANG_EQ: return Value(lv != rv);
        case TK::LT:      return Value(lv <  rv);
        case TK::GT:      return Value(lv >  rv);
        case TK::LT_EQ:   return Value(lv <= rv);
        case TK::GT_EQ:   return Value(lv >= rv);
        default:
            throw VshRuntimeError("operador binario desconocido", n.line);
    }
}

Value VshInterpreter::eval_unary(const AstNode &n, std::shared_ptr<VshEnv> env) {
    Value v = eval(n.left, env);
    if (n.op == TK::MINUS) {
        if (v.is_int())   return Value(-v.as_int());
        if (v.is_float()) return Value(-v.as_float());
        throw VshRuntimeError("'-' unario no aplicable a " + v.to_string(), n.line);
    }
    if (n.op == TK::KW_NOT) return Value(!v.truthy());
    throw VshRuntimeError("operador unario desconocido", n.line);
}

Value VshInterpreter::eval_assign(const AstNode &n, std::shared_ptr<VshEnv> env) {
    Value val = eval(n.right, env);
    if (n.left->kind == AstKind::Ident) {
        env->assign(n.left->name, val);
        return val;
    }
    if (n.left->kind == AstKind::FieldAccess) {
        Value obj = eval(n.left->left, env);
        const std::string &field = n.left->name;
        if (!obj.is_instance())
            throw VshRuntimeError("asignacion de campo '.' solo para instancias", n.line);
        obj.as_instance()->attrs[field] = val;
        return val;
    }
    if (n.left->kind == AstKind::Index) {
        Value container = eval(n.left->left, env);
        Value key       = eval(n.left->right, env);
        if (container.is_list()) {
            if (!key.is_int()) throw VshRuntimeError("indice de lista debe ser entero", n.line);
            auto &lst = *container.as_list();
            int64_t idx = key.as_int();
            if (idx < 0) idx += (int64_t)lst.size();
            if (idx < 0 || (size_t)idx >= lst.size())
                throw VshRuntimeError("indice fuera de rango", n.line);
            lst[idx] = val;
            return val;
        }
        if (container.is_map()) {
            (*container.as_map())[key.to_string()] = val;
            return val;
        }
        throw VshRuntimeError("asignacion por indice solo para listas y mapas", n.line);
    }
    throw VshRuntimeError("el lado izquierdo de '=' no es asignable", n.line);
}

Value VshInterpreter::eval_compound(const AstNode &n, std::shared_ptr<VshEnv> env) {
    // Construir BinOp sintetico y luego asignar
    AstNode binop_node;
    binop_node.kind = AstKind::BinOp;
    binop_node.line = n.line; binop_node.col = n.col;
    binop_node.left = n.left; binop_node.right = n.right;
    switch (n.op) {
        case TK::PLUS_EQ:    binop_node.op = TK::PLUS;    break;
        case TK::MINUS_EQ:   binop_node.op = TK::MINUS;   break;
        case TK::STAR_EQ:    binop_node.op = TK::STAR;    break;
        case TK::SLASH_EQ:   binop_node.op = TK::SLASH;   break;
        case TK::PERCENT_EQ: binop_node.op = TK::PERCENT; break;
        default: throw VshRuntimeError("operador compuesto desconocido", n.line);
    }
    auto bp = std::make_shared<AstNode>(binop_node);
    Value result = eval_binop(binop_node, env);
    if (n.left->kind == AstKind::Ident) {
        env->assign(n.left->name, result);
    }
    return result;
}

Value VshInterpreter::eval_call(const AstNode &n, std::shared_ptr<VshEnv> env) {
    // ---- caso especial: obj.method(args) ----
    if (n.callee->kind == AstKind::FieldAccess) {
        Value obj = eval(n.callee->left, env);
        const std::string &method_name = n.callee->name;
        std::vector<Value> extra_args;
        for (auto &a : n.args) extra_args.push_back(eval(a, env));
        if (obj.is_instance()) {
            auto inst = obj.as_instance();
            // atributo callable (fn guardado en instancia)
            auto it = inst->attrs.find(method_name);
            if (it != inst->attrs.end()) {
                if (it->second.is_function())
                    return call_method_impl(it->second.as_fn(), obj, std::move(extra_args), n.line);
                throw VshRuntimeError("atributo '" + method_name + "' no es callable", n.line);
            }
            // metodo en la clase
            auto method = find_method(inst->klass, method_name);
            if (method) return call_method_impl(method, obj, std::move(extra_args), n.line);
            throw VshRuntimeError("metodo no encontrado: '" + method_name +
                                  "' en " + inst->klass->name, n.line);
        }
        if (obj.is_class()) {
            // metodo de clase sin instancia (llamada estatica)
            auto method = find_method(obj.as_class(), method_name);
            if (method) return call_fn(Value(method), std::move(extra_args), n.line);
            throw VshRuntimeError("metodo no encontrado: '" + method_name +
                                  "' en clase " + obj.as_class()->name, n.line);
        }
        throw VshRuntimeError("acceso '.' solo para instancias y clases", n.line);
    }

    bool is_ident = (n.callee->kind == AstKind::Ident);
    std::string fn_name = is_ident ? n.callee->name : "";

    // evaluar argumentos
    std::vector<Value> args;
    for (auto &a : n.args) args.push_back(eval(a, env));

    // 1. buscar en el scope del script
    if (is_ident && env->has(fn_name)) {
        Value callee = env->get(fn_name);
        if (callee.is_function()) return call_fn(callee, std::move(args), n.line);
        if (callee.is_class()) {
            // constructor: crear instancia y llamar __init__ si existe
            auto klass = callee.as_class();
            auto inst = std::make_shared<VshInstance>();
            inst->klass = klass;
            Value inst_val = make_instance_val(inst);
            auto init = find_method(klass, "__init__");
            if (init) call_method_impl(init, inst_val, std::move(args), n.line);
            return inst_val;
        }
        throw VshRuntimeError("'" + fn_name + "' no es una funcion ni clase", n.line);
    }

    // 2. buscar en built-ins
    if (is_ident) {
        auto bit = builtins_.find(fn_name);
        if (bit != builtins_.end()) {
            try { return bit->second(std::move(args)); }
            catch (VshRuntimeError &e) {
                if (e.line == 0) throw VshRuntimeError(e.vsh_msg, n.line, n.col);
                throw;
            }
        }
    }

    // 3. si el callee es una expresion evaluable (no ident)
    if (!is_ident) {
        Value callee = eval(n.callee, env);
        if (callee.is_function()) return call_fn(callee, std::move(args), n.line);
        if (callee.is_class()) {
            auto klass = callee.as_class();
            auto inst = std::make_shared<VshInstance>();
            inst->klass = klass;
            Value inst_val = make_instance_val(inst);
            auto init = find_method(klass, "__init__");
            if (init) call_method_impl(init, inst_val, std::move(args), n.line);
            return inst_val;
        }
        throw VshRuntimeError("no es una funcion ni clase llamable", n.line);
    }

    // 4. fallback: despachar como comando del REPL
    if (repl_dispatch_) {
        std::string cmd_line = fn_name;
        for (auto &v : args) {
            cmd_line += ' ';
            std::string s = v.to_string();
            // citar con espacios
            if (s.find(' ') != std::string::npos || s.find('"') != std::string::npos) {
                std::string esc;
                for (char c : s) { if (c=='"') esc += "\\\""; else esc += c; }
                cmd_line += '"' + esc + '"';
            } else {
                cmd_line += s;
            }
        }
        repl_dispatch_(cmd_line);
        return Value{}; // los comandos del REPL devuelven null al script
    }

    throw VshRuntimeError("funcion desconocida: '" + fn_name + "'", n.line, n.col);
}

Value VshInterpreter::eval_index(const AstNode &n, std::shared_ptr<VshEnv> env) {
    Value container = eval(n.left, env);
    Value key       = eval(n.right, env);
    if (container.is_list()) {
        if (!key.is_int())
            throw VshRuntimeError("indice de lista debe ser entero, se obtuvo: " + key.to_string(), n.line);
        auto &lst = *container.as_list();
        int64_t idx = key.as_int();
        if (idx < 0) idx += (int64_t)lst.size();
        if (idx < 0 || (size_t)idx >= lst.size())
            throw VshRuntimeError("indice fuera de rango: " + std::to_string(key.as_int()), n.line);
        return lst[idx];
    }
    if (container.is_map()) {
        std::string k = key.to_string();
        auto &m = *container.as_map();
        auto it = m.find(k);
        if (it == m.end())
            throw VshRuntimeError("clave no encontrada en mapa: '" + k + "'", n.line);
        return it->second;
    }
    if (container.is_string()) {
        if (!key.is_int())
            throw VshRuntimeError("indice de string debe ser entero", n.line);
        const auto &s = container.as_string();
        int64_t idx = key.as_int();
        if (idx < 0) idx += (int64_t)s.size();
        if (idx < 0 || (size_t)idx >= s.size())
            throw VshRuntimeError("indice de string fuera de rango", n.line);
        return Value(std::string(1, s[idx]));
    }
    throw VshRuntimeError("indexado no soportado para " + container.to_string(), n.line);
}

Value VshInterpreter::eval_list_lit(const AstNode &n, std::shared_ptr<VshEnv> env) {
    auto lst = std::make_shared<std::vector<Value>>();
    for (auto &e : n.args) lst->push_back(eval(e, env));
    return Value(lst);
}

Value VshInterpreter::eval_map_lit(const AstNode &n, std::shared_ptr<VshEnv> env) {
    auto mp = std::make_shared<std::unordered_map<std::string, Value>>();
    // args contiene pares (key_node, value_node)
    for (size_t i = 0; i+1 < n.args.size(); i += 2) {
        std::string k = eval(n.args[i], env).to_string();
        (*mp)[k] = eval(n.args[i+1], env);
    }
    return Value(mp);
}

// ---- ejecutores de sentencias ----

void VshInterpreter::exec_block(const AstNode &n, std::shared_ptr<VshEnv> env) {
    // Program ejecuta en el env dado; Block crea un scope hijo
    auto child = (n.kind == AstKind::Block) ? env->make_child() : env;
    for (auto &s : n.args) exec(s, child);
}

void VshInterpreter::exec_let(const AstNode &n, std::shared_ptr<VshEnv> env) {
    Value v = n.right ? eval(n.right, env) : Value{};
    if (!n.type_hint.empty()) {
        check_type_hint(n.type_hint, v, "variable '" + n.name + "'", n.line, env);
    }
    env->define(n.name, std::move(v));
}

void VshInterpreter::exec_fn_decl(const AstNode &n, std::shared_ptr<VshEnv> env) {
    auto fn = std::make_shared<VshFunction>();
    fn->name        = n.name;
    fn->params      = n.params;
    fn->param_types = n.param_types;
    fn->body        = n.body;       // AstNodePtr (Block)
    fn->closure_env = env;
    fn->doc = extract_docstring(std::static_pointer_cast<AstNode>(fn->body));
    env->define(n.name, Value(fn));
}

void VshInterpreter::exec_if(const AstNode &n, std::shared_ptr<VshEnv> env) {
    for (auto &branch : n.branches) {
        if (branch.cond == nullptr || eval(branch.cond, env).truthy()) {
            exec(branch.body, env);
            return;
        }
    }
}

void VshInterpreter::exec_while(const AstNode &n, std::shared_ptr<VshEnv> env) {
    while (eval(n.cond, env).truthy()) {
        try { exec(n.body, env); }
        catch (BreakSignal &)    { break; }
        catch (ContinueSignal &) { continue; }
    }
}

void VshInterpreter::exec_for_in(const AstNode &n, std::shared_ptr<VshEnv> env) {
    Value iter = eval(n.cond, env); // n.cond reutilizado como iterable
    auto  loop_env = env->make_child();

    auto run_body = [&](Value v) {
        loop_env->vars[n.name] = std::move(v); // define/redefine la var del bucle
        try { exec(n.body, loop_env); }
        catch (ContinueSignal &) { /* siguiente iteracion */ }
    };

    try {
        if (iter.is_list()) {
            for (auto &v : *iter.as_list()) run_body(v);
        } else if (iter.is_map()) {
            for (auto &[k, _] : *iter.as_map()) run_body(Value(k));
        } else if (iter.is_string()) {
            for (char c : iter.as_string()) run_body(Value(std::string(1,c)));
        } else {
            throw VshRuntimeError("'for in' requiere un iterable (lista, mapa o string)",
                                   n.line, n.col);
        }
    } catch (BreakSignal &) { /* terminar el bucle */ }
}

void VshInterpreter::exec_try(const AstNode &n, std::shared_ptr<VshEnv> env) {
    try {
        exec(n.try_body, env);
    } catch (VshInstanceError &e) {
        // catch tipado: comprobar jerarquia de clases
        if (!n.catch_type_hint.empty()) {
            bool matches = false;
            if (e.thrown_val.is_instance()) {
                auto klass = e.thrown_val.as_instance()->klass;
                while (klass) {
                    if (klass->name == n.catch_type_hint) { matches = true; break; }
                    klass = klass->parent;
                }
            }
            if (!matches) throw; // tipo no coincide: relanzar
        }
        auto catch_env = env->make_child();
        catch_env->define(n.name, e.thrown_val); // la variable catch recibe la instancia
        exec(n.catch_body, catch_env);
    } catch (VshRuntimeError &e) {
        if (!n.catch_type_hint.empty()) throw; // catch tipado no captura errores de string
        auto catch_env = env->make_child();
        catch_env->define(n.name, Value(std::string(e.vsh_msg)));
        exec(n.catch_body, catch_env);
    }
    // ReturnSignal / BreakSignal / ContinueSignal se propagan sin capturar
}

void VshInterpreter::exec_import(const AstNode &n, std::shared_ptr<VshEnv> /*env*/) {
    std::string path = n.name;
    for (auto &p : import_stack_)
        if (p == path)
            throw VshRuntimeError("importacion circular detectada: " + path, n.line);
    import_stack_.push_back(path);
    try {
        exec_file(path); // ejecuta en el scope global (efecto: define funciones globales)
    } catch (...) {
        import_stack_.pop_back();
        throw;
    }
    import_stack_.pop_back();
}

// ---- llamada a funcion VSH ----

Value VshInterpreter::call_fn(const Value &callee, std::vector<Value> args, int line) {
    auto fn = callee.as_fn();
    // stub de builtin nativo (body == nullptr): despachar directamente
    if (!fn->body) {
        auto bit = builtins_.find(fn->name);
        if (bit != builtins_.end()) {
            try { return bit->second(std::move(args)); }
            catch (VshRuntimeError &e) {
                if (e.line == 0) throw VshRuntimeError(e.vsh_msg, line);
                throw;
            }
        }
        return Value{};
    }
    if (args.size() < fn->params.size())
        throw VshRuntimeError("'" + fn->name + "': se esperaban " +
                              std::to_string(fn->params.size()) + " argumentos, se recibieron " +
                              std::to_string(args.size()), line);
    auto fn_env = fn->closure_env->make_child();
    for (size_t i = 0; i < fn->params.size(); ++i) {
        if (i < fn->param_types.size() && !fn->param_types[i].empty()) {
            check_type_hint(fn->param_types[i], args[i],
                           "parametro '" + fn->params[i] + "'", line, fn->closure_env);
        }
        fn_env->define(fn->params[i], std::move(args[i]));
    }
    auto body = std::static_pointer_cast<AstNode>(fn->body);
    try {
        exec(body, fn_env);
        return Value{}; // funciones sin return explicito devuelven null
    } catch (ReturnSignal &rs) {
        return std::move(rs.val);
    }
}

// ---- nuevos metodos OOP/tipado ----

Value VshInterpreter::eval_field(const AstNode &n, std::shared_ptr<VshEnv> env) {
    Value obj = eval(n.left, env);
    const std::string &field = n.name;
    if (obj.is_instance()) {
        auto inst = obj.as_instance();
        auto it = inst->attrs.find(field);
        if (it != inst->attrs.end()) return it->second;
        // devolver metodo como funcion sin enlazar (se enlaza en eval_call)
        auto method = find_method(inst->klass, field);
        if (method) return Value(method);
        throw VshRuntimeError("atributo no encontrado: '" + field +
                              "' en " + inst->klass->name, n.line);
    }
    if (obj.is_class()) {
        auto method = find_method(obj.as_class(), field);
        if (method) return Value(method);
        throw VshRuntimeError("metodo no encontrado: '" + field +
                              "' en clase " + obj.as_class()->name, n.line);
    }
    throw VshRuntimeError("acceso '.' solo para instancias y clases", n.line);
}

void VshInterpreter::exec_class_decl(const AstNode &n, std::shared_ptr<VshEnv> env) {
    auto klass = std::make_shared<VshClass>();
    klass->name = n.name;
    // resolver clase padre
    if (!n.parent_class.empty()) {
        if (!env->has(n.parent_class))
            throw VshRuntimeError("clase padre no definida: '" + n.parent_class + "'", n.line);
        Value parent_val = env->get(n.parent_class);
        if (!parent_val.is_class())
            throw VshRuntimeError("'" + n.parent_class + "' no es una clase", n.line);
        klass->parent = parent_val.as_class();
    }
    // procesar metodos y docstring del cuerpo
    for (auto &child : n.args) {
        if (child->kind == AstKind::FnDecl) {
            auto fn = std::make_shared<VshFunction>();
            fn->name        = child->name;
            fn->params      = child->params;
            fn->param_types = child->param_types;
            fn->body        = child->body;
            fn->closure_env = env;
            fn->doc = extract_docstring(std::static_pointer_cast<AstNode>(fn->body));
            klass->methods[fn->name] = fn;
        } else if (child->kind == AstKind::ExprStmt && child->left &&
                   child->left->kind == AstKind::Literal &&
                   child->left->lit_val.is_string() && klass->doc.empty()) {
            klass->doc = child->left->lit_val.as_string(); // docstring de clase
        }
    }
    env->define(n.name, make_class_val(klass));
}

std::shared_ptr<VshFunction> VshInterpreter::find_method(
    const std::shared_ptr<VshClass> &klass, const std::string &name) {
    if (!klass) return nullptr;
    auto it = klass->methods.find(name);
    if (it != klass->methods.end()) return it->second;
    return find_method(klass->parent, name);
}

Value VshInterpreter::call_method_impl(const std::shared_ptr<VshFunction> &method,
                                        const Value &self_val,
                                        std::vector<Value> extra_args,
                                        int line) {
    if (method->params.empty())
        throw VshRuntimeError("metodo '" + method->name + "' requiere al menos 'self'", line);
    auto fn_env = method->closure_env->make_child();
    fn_env->define(method->params[0], self_val); // primer param = self
    // exponer 'super' si la clase tiene padre
    if (self_val.is_instance()) {
        auto inst = self_val.as_instance();
        if (inst->klass->parent)
            fn_env->define("super", make_class_val(inst->klass->parent));
    }
    if (extra_args.size() < method->params.size() - 1)
        throw VshRuntimeError("metodo '" + method->name + "': argumentos insuficientes", line);
    for (size_t i = 1; i < method->params.size(); ++i) {
        size_t ai = i - 1;
        if (i < method->param_types.size() && !method->param_types[i].empty()) {
            check_type_hint(method->param_types[i], extra_args[ai],
                           "parametro '" + method->params[i] + "'", line, method->closure_env);
        }
        fn_env->define(method->params[i], std::move(extra_args[ai]));
    }
    auto body = std::static_pointer_cast<AstNode>(method->body);
    try {
        exec(body, fn_env);
        return Value{};
    } catch (ReturnSignal &rs) {
        return std::move(rs.val);
    }
}

void VshInterpreter::check_type_hint(const std::string &hint, const Value &val,
                                      const std::string &ctx, int line,
                                      std::shared_ptr<VshEnv> /*env*/) {
    if (hint.empty()) return;
    bool ok = false;
    if      (hint == "null")                   ok = val.is_null();
    else if (hint == "bool")                   ok = val.is_bool();
    else if (hint == "int")                    ok = val.is_int();
    else if (hint == "float")                  ok = val.is_float();
    else if (hint == "str" || hint == "string") ok = val.is_string();
    else if (hint == "list")                   ok = val.is_list();
    else if (hint == "map")                    ok = val.is_map();
    else if (hint == "fn" || hint == "function") ok = val.is_function();
    else if (hint == "class")                  ok = val.is_class();
    else if (hint == "instance")               ok = val.is_instance();
    else {
        // nombre de clase: recorrer jerarquia de herencia
        if (val.is_instance()) {
            auto klass = val.as_instance()->klass;
            while (klass) {
                if (klass->name == hint) { ok = true; break; }
                klass = klass->parent;
            }
        }
    }
    if (!ok) {
        throw VshRuntimeError(ctx + ": se esperaba tipo '" + hint +
                              "', se obtuvo '" + val.to_string() + "'", line);
    }
}

void VshInterpreter::run_interactive(ReadlineFn readline_fn) {
    std::string buffer;
    int depth = 0;

    auto read_line = [&](const std::string &prompt) -> std::string {
        if (readline_fn) return readline_fn(prompt);
        std::cout << prompt << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) return "\x04"; // EOF
        return line;
    };

    std::cout << "VestaShell REPL interactivo - escribe 'exit' o 'quit' para salir\n";

    for (;;) {
        std::string prompt = buffer.empty() ? ">>> " : "... ";
        std::string line = read_line(prompt);

        if (line == "\x04") break; // EOF
        if (buffer.empty() && (line == "exit" || line == "quit")) break;

        // contar { } para detectar bloque multilinea
        for (char c : line) {
            if (c == '{') ++depth;
            else if (c == '}' && depth > 0) --depth;
        }

        if (!buffer.empty()) buffer += '\n';
        buffer += line;

        if (depth > 0) continue; // esperar cierre del bloque

        if (buffer.find_first_not_of(" \t\n\r") == std::string::npos) {
            buffer.clear(); depth = 0; continue;
        }

        try {
            VshLexer  lex(buffer, "<repl>");
            VshParser par(lex);
            auto ast = par.parse_program();
            // si es una sola ExprStmt, imprimir resultado (al estilo Python)
            if (ast->args.size() == 1 && ast->args[0]->kind == AstKind::ExprStmt) {
                Value result = eval(ast->args[0]->left, global_);
                // auto-invocar stubs de builtins (ej: 'help' sin () ejecuta help())
                if (result.is_function() && !result.as_fn()->body && !result.as_fn()->name.empty()) {
                    auto bit = builtins_.find(result.as_fn()->name);
                    if (bit != builtins_.end()) { result = bit->second({}); }
                }
                if (!result.is_null()) std::cout << result.to_repr() << '\n';
            } else {
                exec(ast, global_);
            }
        } catch (VshParseError &e) {
            std::cerr << "SyntaxError: " << e.what() << '\n';
        } catch (VshInstanceError &e) {
            std::cerr << e.thrown_val.as_instance()->klass->name << ": " << e.what() << '\n';
        } catch (VshRuntimeError &e) {
            std::cerr << "RuntimeError: " << e.what() << '\n';
        } catch (std::exception &e) {
            std::cerr << "Error: " << e.what() << '\n';
        }

        buffer.clear(); depth = 0;
    }
}

// ============================================================
// Seccion 6: Built-ins
// ============================================================

void VshInterpreter::register_builtins() {
    // ---- salida ----
    builtins_["echo"] = [](std::vector<Value> args) -> Value {
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) std::cout << ' ';
            std::cout << args[i].to_string();
        }
        std::cout << '\n'; return {};
    };
    builtins_["print"] = [](std::vector<Value> args) -> Value {
        for (auto &a : args) std::cout << a.to_string();
        return {};
    };
    builtins_["println"] = [](std::vector<Value> args) -> Value {
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) std::cout << ' ';
            std::cout << args[i].to_string();
        }
        std::cout << '\n'; return {};
    };

    // ---- conversiones de tipo ----
    builtins_["str"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) return Value(std::string{});
        return Value(args[0].to_string());
    };
    builtins_["int"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) return Value(int64_t(0));
        if (args[0].is_int())   return args[0];
        if (args[0].is_float()) return Value(int64_t(args[0].as_float()));
        if (args[0].is_bool())  return Value(int64_t(args[0].as_bool() ? 1 : 0));
        if (args[0].is_string()) {
            try { return Value(int64_t(std::stoll(args[0].as_string()))); }
            catch (...) { throw VshRuntimeError("int(): no se puede convertir '" + args[0].as_string() + "'"); }
        }
        throw VshRuntimeError("int(): tipo no convertible");
    };
    builtins_["float"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) return Value(0.0);
        if (args[0].is_float()) return args[0];
        if (args[0].is_int())   return Value(double(args[0].as_int()));
        if (args[0].is_bool())  return Value(double(args[0].as_bool() ? 1 : 0));
        if (args[0].is_string()) {
            try { return Value(std::stod(args[0].as_string())); }
            catch (...) { throw VshRuntimeError("float(): no se puede convertir"); }
        }
        throw VshRuntimeError("float(): tipo no convertible");
    };
    builtins_["bool"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) return Value(false);
        return Value(args[0].truthy());
    };
    builtins_["type"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) return Value(std::string("null"));
        static const char* names[] = {
            "null","bool","int","float","string","list","map","function","class","instance"};
        return Value(std::string(names[int(args[0].type())]));
    };

    // ---- colecciones ----
    builtins_["len"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw VshRuntimeError("len() requiere un argumento");
        if (args[0].is_string()) return Value(int64_t(args[0].as_string().size()));
        if (args[0].is_list())   return Value(int64_t(args[0].as_list()->size()));
        if (args[0].is_map())    return Value(int64_t(args[0].as_map()->size()));
        throw VshRuntimeError("len() no soportado para " + args[0].to_string());
    };
    builtins_["append"] = [](std::vector<Value> args) -> Value {
        if (args.size() < 2 || !args[0].is_list())
            throw VshRuntimeError("Uso: append(lista, valor)");
        args[0].as_list()->push_back(args[1]);
        return {};
    };
    builtins_["pop"] = [](std::vector<Value> args) -> Value {
        if (args.empty() || !args[0].is_list())
            throw VshRuntimeError("Uso: pop(lista)");
        auto &v = *args[0].as_list();
        if (v.empty()) throw VshRuntimeError("pop(): lista vacia");
        Value last = v.back(); v.pop_back(); return last;
    };
    builtins_["keys"] = [](std::vector<Value> args) -> Value {
        if (args.empty() || !args[0].is_map())
            throw VshRuntimeError("Uso: keys(mapa)");
        auto lst = std::make_shared<std::vector<Value>>();
        for (auto &[k,_] : *args[0].as_map()) lst->push_back(Value(k));
        return Value(lst);
    };
    builtins_["values"] = [](std::vector<Value> args) -> Value {
        if (args.empty() || !args[0].is_map())
            throw VshRuntimeError("Uso: values(mapa)");
        auto lst = std::make_shared<std::vector<Value>>();
        for (auto &[_,v] : *args[0].as_map()) lst->push_back(v);
        return Value(lst);
    };
    builtins_["contains"] = [](std::vector<Value> args) -> Value {
        if (args.size() < 2) throw VshRuntimeError("Uso: contains(coleccion, valor)");
        if (args[0].is_list()) {
            for (auto &v : *args[0].as_list()) if (v == args[1]) return Value(true);
            return Value(false);
        }
        if (args[0].is_map()) {
            return Value(args[0].as_map()->count(args[1].to_string()) > 0);
        }
        if (args[0].is_string()) {
            return Value(args[0].as_string().find(args[1].to_string()) != std::string::npos);
        }
        throw VshRuntimeError("contains() no soportado para " + args[0].to_string());
    };

    // ---- strings ----
    builtins_["split"] = [](std::vector<Value> args) -> Value {
        if (args.size() < 2 || !args[0].is_string())
            throw VshRuntimeError("Uso: split(string, separador)");
        std::string s   = args[0].as_string();
        std::string sep = args[1].as_string();
        auto lst = std::make_shared<std::vector<Value>>();
        if (sep.empty()) { for (char c:s) lst->push_back(Value(std::string(1,c))); return Value(lst); }
        size_t pos=0, found;
        while ((found=s.find(sep,pos)) != std::string::npos) {
            lst->push_back(Value(s.substr(pos,found-pos))); pos=found+sep.size();
        }
        lst->push_back(Value(s.substr(pos)));
        return Value(lst);
    };
    builtins_["join"] = [](std::vector<Value> args) -> Value {
        if (args.size() < 2 || !args[0].is_list())
            throw VshRuntimeError("Uso: join(lista, separador)");
        std::string sep = args[1].is_string() ? args[1].as_string() : "";
        std::string r;
        auto &v = *args[0].as_list();
        for (size_t i=0; i<v.size(); ++i) { if(i) r+=sep; r+=v[i].to_string(); }
        return Value(r);
    };
    builtins_["trim"] = [](std::vector<Value> args) -> Value {
        if (args.empty() || !args[0].is_string()) throw VshRuntimeError("Uso: trim(string)");
        std::string s = args[0].as_string();
        auto l = s.find_first_not_of(" \t\r\n");
        auto r = s.find_last_not_of(" \t\r\n");
        return Value(l==std::string::npos ? std::string{} : s.substr(l, r-l+1));
    };
    builtins_["upper"] = [](std::vector<Value> args) -> Value {
        if (args.empty()||!args[0].is_string()) throw VshRuntimeError("Uso: upper(string)");
        std::string s = args[0].as_string();
        std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return std::toupper(c);});
        return Value(s);
    };
    builtins_["lower"] = [](std::vector<Value> args) -> Value {
        if (args.empty()||!args[0].is_string()) throw VshRuntimeError("Uso: lower(string)");
        std::string s = args[0].as_string();
        std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return std::tolower(c);});
        return Value(s);
    };
    builtins_["starts_with"] = [](std::vector<Value> args) -> Value {
        if (args.size()<2||!args[0].is_string()) throw VshRuntimeError("Uso: starts_with(s, prefijo)");
        return Value(args[0].as_string().rfind(args[1].as_string(),0)==0);
    };
    builtins_["ends_with"] = [](std::vector<Value> args) -> Value {
        if (args.size()<2||!args[0].is_string()) throw VshRuntimeError("Uso: ends_with(s, sufijo)");
        auto &s=args[0].as_string(); auto &p=args[1].as_string();
        return Value(s.size()>=p.size() && s.substr(s.size()-p.size())==p);
    };
    builtins_["replace"] = [](std::vector<Value> args) -> Value {
        if (args.size()<3||!args[0].is_string()) throw VshRuntimeError("Uso: replace(s, viejo, nuevo)");
        std::string s=args[0].as_string(), viejo=args[1].as_string(), nuevo=args[2].as_string();
        size_t pos=0;
        while ((pos=s.find(viejo,pos))!=std::string::npos) { s.replace(pos,viejo.size(),nuevo); pos+=nuevo.size(); }
        return Value(s);
    };
    builtins_["substr"] = [](std::vector<Value> args) -> Value {
        if (args.size()<2||!args[0].is_string()) throw VshRuntimeError("Uso: substr(s, inicio [, len])");
        const auto &s=args[0].as_string();
        int64_t start=args[1].as_int();
        if (start<0) start+=(int64_t)s.size();
        size_t len = args.size()>=3 ? (size_t)args[2].as_int() : std::string::npos;
        return Value(s.substr((size_t)start, len));
    };

    // ---- iteracion ----
    builtins_["range"] = [](std::vector<Value> args) -> Value {
        int64_t start=0, end=0, step=1;
        if (args.size()==1) { end=args[0].is_int()?args[0].as_int():(int64_t)args[0].as_float(); }
        else if (args.size()>=2) { start=args[0].is_int()?args[0].as_int():(int64_t)args[0].as_float();
                                   end=args[1].is_int()?args[1].as_int():(int64_t)args[1].as_float(); }
        if (args.size()>=3) step=args[2].is_int()?args[2].as_int():(int64_t)args[2].as_float();
        if (step==0) throw VshRuntimeError("range(): step no puede ser 0");
        auto lst = std::make_shared<std::vector<Value>>();
        for (int64_t i=start; (step>0?i<end:i>end); i+=step) lst->push_back(Value(i));
        return Value(lst);
    };

    // ---- sistema de ficheros ----
    builtins_["exists"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw VshRuntimeError("Uso: exists(ruta)");
        std::error_code ec;
        return Value(std::filesystem::exists(args[0].as_string(), ec));
    };
    builtins_["is_dir"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw VshRuntimeError("Uso: is_dir(ruta)");
        std::error_code ec;
        return Value(std::filesystem::is_directory(args[0].as_string(), ec));
    };
    builtins_["is_file"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw VshRuntimeError("Uso: is_file(ruta)");
        std::error_code ec;
        return Value(std::filesystem::is_regular_file(args[0].as_string(), ec));
    };
    builtins_["basename"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) return Value(std::string{});
        return Value(std::filesystem::path(args[0].as_string()).filename().string());
    };
    builtins_["dirname"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) return Value(std::string{});
        auto p = std::filesystem::path(args[0].as_string()).parent_path();
        return Value(p.empty() ? std::string(".") : p.string());
    };
    builtins_["stem"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) return Value(std::string{});
        return Value(std::filesystem::path(args[0].as_string()).stem().string());
    };
    builtins_["extension"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) return Value(std::string{});
        return Value(std::filesystem::path(args[0].as_string()).extension().string());
    };
    builtins_["glob"] = [](std::vector<Value> args) -> Value {
        auto lst = std::make_shared<std::vector<Value>>();
        if (args.empty()) return Value(lst);
        std::string pattern = args[0].as_string();
        namespace fs = std::filesystem;
        try {
            fs::path base  = fs::path(pattern).parent_path();
            std::string pat = fs::path(pattern).filename().string();
            if (base.empty()) base = ".";
            // matching simple: * ? como wildcards
            auto match_pat = [](const std::string &name, const std::string &p) -> bool {
                size_t ni=0, pi=0;
                size_t last_star_pi = std::string::npos, last_star_ni = 0;
                while (ni < name.size()) {
                    if (pi < p.size() && (p[pi] == '?' || p[pi] == name[ni])) { ++ni; ++pi; }
                    else if (pi < p.size() && p[pi] == '*') {
                        last_star_pi = pi++; last_star_ni = ni;
                    } else if (last_star_pi != std::string::npos) {
                        pi = last_star_pi + 1; ni = ++last_star_ni;
                    } else return false;
                }
                while (pi < p.size() && p[pi] == '*') ++pi;
                return pi == p.size();
            };
            std::error_code ec;
            for (auto &e : fs::directory_iterator(base, ec)) {
                std::string fname = e.path().filename().string();
                if (pat == "*" || match_pat(fname, pat))
                    lst->push_back(Value(e.path().string()));
            }
            // ordenar para resultados deterministas
            std::sort(lst->begin(), lst->end(), [](const Value &a, const Value &b){
                return a.as_string() < b.as_string();
            });
        } catch (...) {}
        return Value(lst);
    };
    builtins_["read_file"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw VshRuntimeError("Uso: read_file(ruta)");
        std::ifstream f(args[0].as_string());
        if (!f) throw VshRuntimeError("read_file(): no se puede abrir: " + args[0].as_string());
        std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        return Value(s);
    };
    builtins_["write_file"] = [](std::vector<Value> args) -> Value {
        if (args.size() < 2) throw VshRuntimeError("Uso: write_file(ruta, contenido)");
        std::ofstream f(args[0].as_string());
        if (!f) throw VshRuntimeError("write_file(): no se puede abrir: " + args[0].as_string());
        f << args[1].to_string();
        return {};
    };

    // ---- shell y misc ----
    builtins_["shell"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) return Value(std::string{});
        std::string cmd = args[0].as_string();
        std::string out;
#if defined(_WIN32)
        FILE *f = _popen(("cmd.exe /C \"" + cmd + "\" 2>&1").c_str(), "r");
#else
        FILE *f = popen((cmd + " 2>&1").c_str(), "r");
#endif
        if (!f) throw VshRuntimeError("shell(): no se pudo ejecutar: " + cmd);
        char buf[256];
        while (fgets(buf, sizeof(buf), f)) out += buf;
#if defined(_WIN32)
        _pclose(f);
#else
        pclose(f);
#endif
        if (!out.empty() && out.back()=='\n') out.pop_back();
        return Value(out);
    };
    builtins_["sleep"] = [](std::vector<Value> args) -> Value {
        int64_t ms = 0;
        if (!args.empty()) ms = args[0].is_int() ? args[0].as_int() : (int64_t)(args[0].as_number());
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        return {};
    };
    builtins_["input"] = [](std::vector<Value> args) -> Value {
        if (!args.empty()) std::cout << args[0].to_string() << std::flush;
        std::string line;
        std::getline(std::cin, line);
        return Value(line);
    };
    builtins_["exit"] = [](std::vector<Value> args) -> Value {
        int code = args.empty() ? 0 : (int)(args[0].is_int() ? args[0].as_int() : (int64_t)args[0].as_number());
        std::exit(code);
        return {};
    };
    builtins_["assert"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw VshRuntimeError("assert(): se requiere condicion");
        if (!args[0].truthy()) {
            std::string msg = args.size() >= 2 ? args[1].to_string() : "asercion fallida";
            throw VshRuntimeError(msg);
        }
        return {};
    };
    builtins_["error"] = [](std::vector<Value> args) -> Value {
        std::string msg = args.empty() ? "error en el script" : args[0].to_string();
        throw VshRuntimeError(msg);
        return {};
    };

    // ---- matematicas ----
    builtins_["abs"]   = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw VshRuntimeError("abs() requiere argumento");
        if (args[0].is_int())   return Value(std::abs(args[0].as_int()));
        return Value(std::abs(args[0].as_float()));
    };
    builtins_["min"]   = [](std::vector<Value> args) -> Value {
        if (args.size()<2) throw VshRuntimeError("min() requiere 2 argumentos");
        return args[0] < args[1] ? args[0] : args[1];
    };
    builtins_["max"]   = [](std::vector<Value> args) -> Value {
        if (args.size()<2) throw VshRuntimeError("max() requiere 2 argumentos");
        return args[0] > args[1] ? args[0] : args[1];
    };
    builtins_["floor"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw VshRuntimeError("floor() requiere argumento");
        return Value(int64_t(std::floor(args[0].as_number())));
    };
    builtins_["ceil"]  = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw VshRuntimeError("ceil() requiere argumento");
        return Value(int64_t(std::ceil(args[0].as_number())));
    };
    builtins_["round"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw VshRuntimeError("round() requiere argumento");
        return Value(int64_t(std::round(args[0].as_number())));
    };
    builtins_["sqrt"]  = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw VshRuntimeError("sqrt() requiere argumento");
        return Value(std::sqrt(args[0].as_number()));
    };
    builtins_["pow"]   = [](std::vector<Value> args) -> Value {
        if (args.size()<2) throw VshRuntimeError("pow() requiere 2 argumentos");
        return Value(std::pow(args[0].as_number(), args[1].as_number()));
    };
    builtins_["log"]   = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw VshRuntimeError("log() requiere argumento");
        return Value(std::log(args[0].as_number()));
    };

    // ---- utilidades de tipo ----
    builtins_["is_null"]  = [](std::vector<Value> args) -> Value { return Value(args.empty()||args[0].is_null()); };
    builtins_["is_bool"]  = [](std::vector<Value> args) -> Value { return Value(!args.empty()&&args[0].is_bool()); };
    builtins_["is_int"]   = [](std::vector<Value> args) -> Value { return Value(!args.empty()&&args[0].is_int()); };
    builtins_["is_float"] = [](std::vector<Value> args) -> Value { return Value(!args.empty()&&args[0].is_float()); };
    builtins_["is_str"]   = [](std::vector<Value> args) -> Value { return Value(!args.empty()&&args[0].is_string()); };
    builtins_["is_list"]  = [](std::vector<Value> args) -> Value { return Value(!args.empty()&&args[0].is_list()); };
    builtins_["is_map"]   = [](std::vector<Value> args) -> Value { return Value(!args.empty()&&args[0].is_map()); };
    builtins_["is_fn"]       = [](std::vector<Value> args) -> Value { return Value(!args.empty()&&args[0].is_function()); };
    builtins_["is_class"]    = [](std::vector<Value> args) -> Value { return Value(!args.empty()&&args[0].is_class()); };
    builtins_["is_instance"] = [](std::vector<Value> args) -> Value { return Value(!args.empty()&&args[0].is_instance()); };

    // ---- OOP ----
    builtins_["classname"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw VshRuntimeError("classname() requiere argumento");
        if (args[0].is_instance()) return Value(args[0].as_instance()->klass->name);
        if (args[0].is_class())    return Value(args[0].as_class()->name);
        throw VshRuntimeError("classname(): se esperaba instancia o clase");
    };
    builtins_["isinstance"] = [](std::vector<Value> args) -> Value {
        if (args.size() < 2) throw VshRuntimeError("Uso: isinstance(obj, clase)");
        if (!args[0].is_instance()) return Value(false);
        if (!args[1].is_class()) throw VshRuntimeError("isinstance(): segundo arg debe ser clase");
        const std::string &target = args[1].as_class()->name;
        auto klass = args[0].as_instance()->klass;
        while (klass) {
            if (klass->name == target) return Value(true);
            klass = klass->parent;
        }
        return Value(false);
    };

    // ---- documentacion (docstrings) ----
    builtins_["doc"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) return Value(std::string{});
        if (args[0].is_function()) return Value(args[0].as_fn()->doc);
        if (args[0].is_class())    return Value(args[0].as_class()->doc);
        return Value(std::string{});
    };
    builtins_["help"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) {
            std::cout << "Uso: help(funcion_o_clase)  ->  muestra firma y docstring\n";
            return Value{};
        }
        if (args[0].is_function()) {
            auto fn = args[0].as_fn();
            std::string sig = fn->name.empty() ? "<lambda>" : fn->name;
            sig += "(";
            for (size_t i = 0; i < fn->params.size(); ++i) {
                if (i) sig += ", ";
                sig += fn->params[i];
                if (i < fn->param_types.size() && !fn->param_types[i].empty())
                    sig += ": " + fn->param_types[i];
            }
            sig += ")";
            std::string out = sig + "\n";
            out += fn->doc.empty() ? "\n(sin documentacion)\n" : "\n" + fn->doc + "\n";
            std::cout << out;
            return Value(out);
        }
        if (args[0].is_class()) {
            auto klass = args[0].as_class();
            std::string out = "class " + klass->name;
            if (klass->parent) out += " : " + klass->parent->name;
            out += "\n";
            if (!klass->doc.empty()) out += "\n" + klass->doc + "\n";
            out += "\nMetodos:\n";
            for (auto &[mname, fn] : klass->methods) {
                out += "  " + mname + "(";
                for (size_t i = 0; i < fn->params.size(); ++i) {
                    if (i) out += ", ";
                    out += fn->params[i];
                    if (i < fn->param_types.size() && !fn->param_types[i].empty())
                        out += ": " + fn->param_types[i];
                }
                out += ")\n";
                if (!fn->doc.empty()) out += "    " + fn->doc + "\n";
            }
            std::cout << out;
            return Value(out);
        }
        throw VshRuntimeError("help() requiere funcion o clase");
    };

    // ---- sockets de red ----
    builtins_["tcp_connect"] = [](std::vector<Value> args) -> Value {
        if (args.size() < 2) throw VshRuntimeError("Uso: tcp_connect(host, port)");
        ensure_wsa();
        std::string host = args[0].as_string();
        uint16_t port = (uint16_t)args[1].as_int();
        struct sockaddr_in addr{};
        vsh_resolve(host, port, addr);
        vsh_sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
        if (!vsh_sock_valid(fd)) throw VshRuntimeError("tcp_connect: error creando socket");
        if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
            vsh_sock_close(fd);
            throw VshRuntimeError("tcp_connect: no se pudo conectar a " + host + ":" + std::to_string(port));
        }
        return Value(vsh_alloc_sock(fd, false));
    };
    builtins_["tcp_listen"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw VshRuntimeError("Uso: tcp_listen(port [, backlog])");
        ensure_wsa();
        uint16_t port = (uint16_t)args[0].as_int();
        int backlog = args.size() >= 2 ? (int)args[1].as_int() : 10;
        vsh_sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
        if (!vsh_sock_valid(fd)) throw VshRuntimeError("tcp_listen: error creando socket");
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
        struct sockaddr_in addr{};
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);
        if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
            vsh_sock_close(fd);
            throw VshRuntimeError("tcp_listen: bind fallido en puerto " + std::to_string(port));
        }
        listen(fd, backlog);
        return Value(vsh_alloc_sock(fd, false));
    };
    builtins_["tcp_accept"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw VshRuntimeError("Uso: tcp_accept(handle)");
        int64_t id = args[0].as_int();
        auto *s = vsh_get_sock(id);
        if (!s) throw VshRuntimeError("tcp_accept: handle invalido");
        struct sockaddr_in remote{};
        socklen_t len = sizeof(remote);
        vsh_sock_t client = accept(s->fd, reinterpret_cast<struct sockaddr*>(&remote), &len);
        if (!vsh_sock_valid(client)) throw VshRuntimeError("tcp_accept: error al aceptar conexion");
        return Value(vsh_alloc_sock(client, false));
    };
    builtins_["socket_send"] = [](std::vector<Value> args) -> Value {
        if (args.size() < 2) throw VshRuntimeError("Uso: socket_send(handle, datos)");
        int64_t id = args[0].as_int();
        auto *s = vsh_get_sock(id);
        if (!s) throw VshRuntimeError("socket_send: handle invalido");
        std::string data = args[1].to_string();
        int sent;
        if (s->ssl) sent = SSL_write(s->ssl, data.c_str(), (int)data.size());
        else        sent = (int)send(s->fd, data.c_str(), (int)data.size(), 0);
        if (sent < 0) throw VshRuntimeError("socket_send: error al enviar datos");
        return Value(int64_t(sent));
    };
    builtins_["socket_recv"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw VshRuntimeError("Uso: socket_recv(handle [, maxlen])");
        int64_t id = args[0].as_int();
        auto *s = vsh_get_sock(id);
        if (!s) throw VshRuntimeError("socket_recv: handle invalido");
        int maxlen = args.size() >= 2 ? (int)args[1].as_int() : 4096;
        std::string buf(maxlen, '\0');
        int n;
        if (s->ssl) n = SSL_read(s->ssl, &buf[0], maxlen);
        else        n = (int)recv(s->fd, &buf[0], (size_t)maxlen, 0);
        if (n < 0) throw VshRuntimeError("socket_recv: error al recibir datos");
        buf.resize(n);
        return Value(buf);
    };
    builtins_["socket_close"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw VshRuntimeError("Uso: socket_close(handle)");
        vsh_free_sock(args[0].as_int());
        return {};
    };
    builtins_["udp_socket"] = [](std::vector<Value> args) -> Value {
        (void)args;
        ensure_wsa();
        vsh_sock_t fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (!vsh_sock_valid(fd)) throw VshRuntimeError("udp_socket: error creando socket UDP");
        return Value(vsh_alloc_sock(fd, true));
    };
    builtins_["udp_bind"] = [](std::vector<Value> args) -> Value {
        if (args.size() < 2) throw VshRuntimeError("Uso: udp_bind(handle, port)");
        int64_t id = args[0].as_int();
        auto *s = vsh_get_sock(id);
        if (!s || !s->is_udp) throw VshRuntimeError("udp_bind: handle invalido o no es UDP");
        uint16_t port = (uint16_t)args[1].as_int();
        struct sockaddr_in addr{};
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);
        if (bind(s->fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0)
            throw VshRuntimeError("udp_bind: bind fallido en puerto " + std::to_string(port));
        return {};
    };
    builtins_["udp_sendto"] = [](std::vector<Value> args) -> Value {
        if (args.size() < 4) throw VshRuntimeError("Uso: udp_sendto(handle, datos, host, port)");
        int64_t id = args[0].as_int();
        auto *s = vsh_get_sock(id);
        if (!s || !s->is_udp) throw VshRuntimeError("udp_sendto: handle invalido o no es UDP");
        std::string data = args[1].to_string();
        struct sockaddr_in dest{};
        vsh_resolve(args[2].as_string(), (uint16_t)args[3].as_int(), dest);
        int n = (int)sendto(s->fd, data.c_str(), (int)data.size(), 0,
                            reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));
        if (n < 0) throw VshRuntimeError("udp_sendto: error al enviar");
        return Value(int64_t(n));
    };
    builtins_["udp_recvfrom"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw VshRuntimeError("Uso: udp_recvfrom(handle [, maxlen])");
        int64_t id = args[0].as_int();
        auto *s = vsh_get_sock(id);
        if (!s || !s->is_udp) throw VshRuntimeError("udp_recvfrom: handle invalido o no es UDP");
        int maxlen = args.size() >= 2 ? (int)args[1].as_int() : 65536;
        std::string buf(maxlen, '\0');
        struct sockaddr_in from{};
        socklen_t fromlen = sizeof(from);
        int n = (int)recvfrom(s->fd, &buf[0], (size_t)maxlen, 0,
                              reinterpret_cast<struct sockaddr*>(&from), &fromlen);
        if (n < 0) throw VshRuntimeError("udp_recvfrom: error al recibir");
        buf.resize(n);
        char ip_buf[INET_ADDRSTRLEN] = {};
        // inet_ntoa pasa el valor por copia (no puntero) asi que es seguro
        const char *ipstr = inet_ntoa(from.sin_addr);
        if (ipstr) strncpy(ip_buf, ipstr, sizeof(ip_buf)-1);
        auto lst = std::make_shared<std::vector<Value>>();
        lst->push_back(Value(buf));
        lst->push_back(Value(std::string(ip_buf)));
        lst->push_back(Value(int64_t(ntohs(from.sin_port))));
        return Value(lst);
    };
    builtins_["tls_connect"] = [](std::vector<Value> args) -> Value {
        if (args.size() < 2) throw VshRuntimeError("Uso: tls_connect(host, port)");
        ensure_wsa();
        ensure_ssl_ctx();
        std::string host = args[0].as_string();
        uint16_t port = (uint16_t)args[1].as_int();
        struct sockaddr_in addr{};
        vsh_resolve(host, port, addr);
        vsh_sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
        if (!vsh_sock_valid(fd)) throw VshRuntimeError("tls_connect: error creando socket");
        if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
            vsh_sock_close(fd);
            throw VshRuntimeError("tls_connect: no se pudo conectar a " + host);
        }
        SSL *ssl = SSL_new(g_ssl_ctx);
        SSL_set_fd(ssl, (int)fd);
        SSL_set_tlsext_host_name(ssl, host.c_str());
        if (SSL_connect(ssl) <= 0) {
            SSL_free(ssl); vsh_sock_close(fd);
            throw VshRuntimeError("tls_connect: fallo TLS handshake con " + host);
        }
        return Value(vsh_alloc_sock(fd, false, ssl));
    };
    builtins_["socket_recv_all"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw VshRuntimeError("Uso: socket_recv_all(handle)");
        int64_t id = args[0].as_int();
        auto *s = vsh_get_sock(id);
        if (!s) throw VshRuntimeError("socket_recv_all: handle invalido");
        std::string result;
        char buf[4096];
        for (;;) {
            int n = s->ssl ? SSL_read(s->ssl, buf, sizeof(buf))
                           : (int)recv(s->fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            result.append(buf, (size_t)n);
        }
        return Value(result);
    };
    builtins_["http_get"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw VshRuntimeError("Uso: http_get(url)");
        bool use_tls; std::string path;
        auto [host, port] = vsh_parse_url(args[0].as_string(), use_tls, path);
        return Value(vsh_http_request("GET", host, port, path, false).body);
    };
    builtins_["https_get"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw VshRuntimeError("Uso: https_get(url)");
        bool use_tls; std::string path;
        auto [host, port] = vsh_parse_url(args[0].as_string(), use_tls, path);
        return Value(vsh_http_request("GET", host, port, path, true).body);
    };
    builtins_["http_post"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw VshRuntimeError("Uso: http_post(url, body [, content_type])");
        bool use_tls; std::string path;
        auto [host, port] = vsh_parse_url(args[0].as_string(), use_tls, path);
        std::string body = args.size() >= 2 ? args[1].to_string() : "";
        std::string ct   = args.size() >= 3 ? args[2].as_string() : "application/x-www-form-urlencoded";
        return Value(vsh_http_request("POST", host, port, path, false, body, ct).body);
    };
    builtins_["https_post"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw VshRuntimeError("Uso: https_post(url, body [, content_type])");
        bool use_tls; std::string path;
        auto [host, port] = vsh_parse_url(args[0].as_string(), use_tls, path);
        std::string body = args.size() >= 2 ? args[1].to_string() : "";
        std::string ct   = args.size() >= 3 ? args[2].as_string() : "application/x-www-form-urlencoded";
        return Value(vsh_http_request("POST", host, port, path, true, body, ct).body);
    };
    builtins_["http_put"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw VshRuntimeError("Uso: http_put(url, body [, content_type])");
        bool use_tls; std::string path;
        auto [host, port] = vsh_parse_url(args[0].as_string(), use_tls, path);
        std::string body = args.size() >= 2 ? args[1].to_string() : "";
        std::string ct   = args.size() >= 3 ? args[2].as_string() : "application/x-www-form-urlencoded";
        return Value(vsh_http_request("PUT", host, port, path, false, body, ct).body);
    };
    builtins_["https_put"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw VshRuntimeError("Uso: https_put(url, body [, content_type])");
        bool use_tls; std::string path;
        auto [host, port] = vsh_parse_url(args[0].as_string(), use_tls, path);
        std::string body = args.size() >= 2 ? args[1].to_string() : "";
        std::string ct   = args.size() >= 3 ? args[2].as_string() : "application/x-www-form-urlencoded";
        return Value(vsh_http_request("PUT", host, port, path, true, body, ct).body);
    };
    builtins_["http_delete"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw VshRuntimeError("Uso: http_delete(url)");
        bool use_tls; std::string path;
        auto [host, port] = vsh_parse_url(args[0].as_string(), use_tls, path);
        return Value(vsh_http_request("DELETE", host, port, path, false).body);
    };
    builtins_["https_delete"] = [](std::vector<Value> args) -> Value {
        if (args.empty()) throw VshRuntimeError("Uso: https_delete(url)");
        bool use_tls; std::string path;
        auto [host, port] = vsh_parse_url(args[0].as_string(), use_tls, path);
        return Value(vsh_http_request("DELETE", host, port, path, true).body);
    };
    // http_request(method, url [, body [, content_type]]) -> map {status, body}
    builtins_["http_request"] = [](std::vector<Value> args) -> Value {
        if (args.size() < 2) throw VshRuntimeError("Uso: http_request(method, url [, body [, content_type]])");
        std::string method = args[0].as_string();
        bool use_tls; std::string path;
        auto [host, port] = vsh_parse_url(args[1].as_string(), use_tls, path);
        std::string body = args.size() >= 3 ? args[2].to_string() : "";
        std::string ct   = args.size() >= 4 ? args[3].as_string() : "application/json";
        auto r = vsh_http_request(method, host, port, path, use_tls, body, ct);
        auto m = std::make_shared<std::unordered_map<std::string, Value>>();
        (*m)["status"]  = Value(int64_t(r.status));
        (*m)["headers"] = Value(r.headers);
        (*m)["body"]    = Value(r.body);
        return Value(m);
    };

    // ---- exponer todos los builtins en el scope global como stubs (estilo Python) ----
    // Esto permite usar 'help' sin parentesis igual que en Python
    for (auto &kv : builtins_) {
        const std::string &bname = kv.first;
        auto stub = std::make_shared<VshFunction>();
        stub->name = bname;
        stub->closure_env = global_;
        // body queda nullptr -> call_fn lo despachara al builtin nativo
        global_->define(bname, Value(stub));
    }

    // ---- clase Error incorporada (base para errores personalizados) ----
    exec_string(
        "class Error {\n"
        "    fn __init__(self, message) {\n"
        "        self.message = message\n"
        "    }\n"
        "    fn __str__(self) {\n"
        "        return self.message\n"
        "    }\n"
        "}\n",
        "<builtin>"
    );
}

} // namespace vsh
