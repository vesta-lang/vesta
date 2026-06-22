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
 * @file semantic_tokens.cpp
 * @brief Implementacion del resaltado semantico del LSP de Vex.
 *
 * Estrategia general:
 *  1. LEXAR el fuente con el lexer de Vex y, por cada token, emitir un
 *     "token absoluto" (linea 0-based, caracter UTF-16 0-based, longitud
 *     UTF-16, tipo, modificadores).  Los identificadores se enriquecen
 *     consultando los sets de nombres declarados del @c DocAnalysis.
 *  2. ESCANEAR aparte los comentarios (el lexer los descarta) con un
 *     scanner ligero que respeta strings y chars, y emitir un token
 *     @c Comment por cada uno, PARTIENDO los comentarios de bloque en un
 *     token por linea cubierta (el protocolo no admite tokens multilinea).
 *  3. ORDENAR todos los tokens absolutos por (linea, caracter) y
 *     CODIFICARLOS en el array plano de quintetos con deltas relativos.
 *
 * La posicion en unidades UTF-16 se calcula con un indice byte->(linea,
 * columna UTF-16) construido en una sola pasada sobre el fuente.
 */

#include "lsp/semantic_tokens.h"

#include <algorithm>
#include <exception>
#include <unordered_set>

#include "lsp/analysis_engine.h"
#include "vex/diagnostic.h"
#include "vex/lexer.h"
#include "vex/token.h"

namespace lsp {

namespace {

/// Representacion intermedia de un token ya resuelto a coordenadas LSP
/// (lineas/caracteres UTF-16 absolutos), antes de codificar a deltas.
struct AbsToken {
    uint32_t line = 0;       ///< Linea 0-based.
    uint32_t start = 0;      ///< Caracter inicial 0-based en UTF-16.
    uint32_t length = 0;     ///< Longitud en unidades UTF-16.
    uint32_t type = 0;       ///< Indice en la leyenda de tokenTypes.
    uint32_t modifiers = 0;  ///< Mascara de modificadores.
};

/**
 * @brief Indice de posiciones: traduce un offset de byte del fuente a su
 *        (linea 0-based, columna 0-based en unidades UTF-16).
 *
 * Se construye en una sola pasada decodificando UTF-8.  Mantiene, por cada
 * byte de inicio de un code point, el par (linea, columna UTF-16) en el que
 * ese code point comienza.  Los bytes de continuacion comparten la entrada
 * del code point al que pertenecen (no se consultan de forma directa).
 */
class PositionIndex {
  public:
    explicit PositionIndex(const std::string &src) { build(src); }

    /**
     * @brief Devuelve la posicion LSP del code point que empieza en
     *        @p byte_offset.  Si el offset cae dentro de una secuencia o
     *        fuera de rango, devuelve la posicion del final del fuente.
     */
    void at(size_t byte_offset, uint32_t &out_line, uint32_t &out_col) const {
        if (byte_offset >= line_.size()) {
            out_line = end_line_;
            out_col = end_col_;
            return;
        }
        out_line = line_[byte_offset];
        out_col = col_[byte_offset];
    }

    /// Columna UTF-16 al final del fuente (para clamping).
    uint32_t end_line() const { return end_line_; }
    uint32_t end_col() const { return end_col_; }

  private:
    void build(const std::string &src) {
        const size_t n = src.size();
        line_.assign(n, 0);
        col_.assign(n, 0);
        uint32_t line = 0;       // linea 0-based.
        uint32_t col = 0;        // columna 0-based en UTF-16.
        size_t i = 0;
        while (i < n) {
            const unsigned char b = static_cast<unsigned char>(src[i]);
            // Longitud de la secuencia UTF-8 segun el byte lider.
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
                seq = 1; // byte invalido: 1 unidad.
            }
            // Validar continuaciones; si no caben o son invalidas, degradar.
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
            // Anotar la posicion de inicio del code point en TODOS sus bytes
            // (asi at() funciona aunque el offset apunte a una continuacion).
            for (size_t k = 0; k < seq && (i + k) < n; ++k) {
                line_[i + k] = line;
                col_[i + k] = col;
            }
            // Avanzar la posicion logica.
            if (seq == 1 && b == '\n') {
                ++line;
                col = 0;
            } else {
                // Unidades UTF-16 del code point: 2 si es plano astral.
                col += (seq == 4 && cp >= 0x10000) ? 2u : 1u;
            }
            i += seq;
        }
        end_line_ = line;
        end_col_ = col;
    }

    std::vector<uint32_t> line_; ///< byte_offset -> linea de su code point.
    std::vector<uint32_t> col_;  ///< byte_offset -> columna UTF-16.
    uint32_t end_line_ = 0;
    uint32_t end_col_ = 0;
};

/**
 * @brief Indica si un TokenKind es un tipo primitivo de Vex.
 *
 * Cubre i8..i64, u8..u64, los alias int*_t/uint*_t, f32/f64/float/double,
 * bool/char/void/string.  Las colecciones builtin (ArrayList, HashMap, ...)
 * y los smart pointers (unique/shared/borrow) tambien se resaltan como
 * tipo por ergonomia.
 */
bool is_primitive_type_kw(vex::TokenKind k) {
    using TK = vex::TokenKind;
    switch (k) {
    case TK::KW_VOID:
    case TK::KW_BOOL:
    case TK::KW_CHAR:
    case TK::KW_INT8:
    case TK::KW_INT16:
    case TK::KW_INT32:
    case TK::KW_INT64:
    case TK::KW_UINT8:
    case TK::KW_UINT16:
    case TK::KW_UINT32:
    case TK::KW_UINT64:
    case TK::KW_INT8_T:
    case TK::KW_INT16_T:
    case TK::KW_INT32_T:
    case TK::KW_INT64_T:
    case TK::KW_UINT8_T:
    case TK::KW_UINT16_T:
    case TK::KW_UINT32_T:
    case TK::KW_UINT64_T:
    case TK::KW_F32:
    case TK::KW_F64:
    case TK::KW_FLOAT:
    case TK::KW_DOUBLE:
    case TK::KW_STRING:
    case TK::KW_ARRAYLIST:
    case TK::KW_HASHMAP:
    case TK::KW_HASHSET:
    case TK::KW_QUEUE:
    case TK::KW_DEQUE:
    case TK::KW_TREEMAP:
    case TK::KW_TREESET:
    case TK::KW_STACK:
    case TK::KW_UNIQUE:
    case TK::KW_SHARED:
    case TK::KW_BORROW:
    case TK::KW_BORROW_MUT:
        return true;
    default:
        return false;
    }
}

/**
 * @brief Indica si un TokenKind es una palabra reservada (cualquier KW_*).
 *
 * El enum agrupa los keywords entre @c KW_VOID y @c KW_CASE (categorias
 * 4..7).  Se comprueba por rango ordinal para no enumerar cada uno.
 */
bool is_keyword(vex::TokenKind k) {
    using TK = vex::TokenKind;
    // El primer keyword es KW_VOID; el ultimo de la zona de keywords es
    // KW_CASE.  TRUE_KW/FALSE_KW/NULL_KW se tratan aparte (literales).
    return k >= TK::KW_VOID && k <= TK::KW_CASE;
}

/**
 * @brief Indica si un TokenKind es un operador (no delimitador).
 *
 * Los delimitadores (parentesis, llaves, corchetes, coma, punto y coma,
 * dos puntos, punto, @) NO se emiten: el editor los deja en su color por
 * defecto.  Solo se resaltan operadores reales.
 */
bool is_operator(vex::TokenKind k) {
    using TK = vex::TokenKind;
    switch (k) {
    case TK::PLUS:
    case TK::MINUS:
    case TK::STAR:
    case TK::SLASH:
    case TK::PERCENT:
    case TK::ASSIGN:
    case TK::PLUS_ASSIGN:
    case TK::MINUS_ASSIGN:
    case TK::STAR_ASSIGN:
    case TK::SLASH_ASSIGN:
    case TK::PERCENT_ASSIGN:
    case TK::AMP_ASSIGN:
    case TK::PIPE_ASSIGN:
    case TK::CARET_ASSIGN:
    case TK::SHL_ASSIGN:
    case TK::SHR_ASSIGN:
    case TK::EQ:
    case TK::NEQ:
    case TK::LT:
    case TK::LE:
    case TK::GT:
    case TK::GE:
    case TK::AND_AND:
    case TK::OR_OR:
    case TK::BANG:
    case TK::BANG_BANG:
    case TK::AMP:
    case TK::PIPE:
    case TK::CARET:
    case TK::TILDE:
    case TK::SHL:
    case TK::SHR:
    case TK::PLUS_PLUS:
    case TK::MINUS_MINUS:
    case TK::ARROW:
    case TK::FAT_ARROW:
    case TK::QUESTION:
        return true;
    default:
        return false;
    }
}

/**
 * @brief Indica si @p name es un keyword CONTEXTUAL de Vex.
 *
 * El lexer emite estos nombres como IDENTIFIER (no hay un KW_* dedicado)
 * porque su caracter de palabra reservada depende del contexto
 * sintactico.  El parser los reconoce comparando el lexema (ver
 * @c src/vex/parser.cpp).  Para el resaltado los tratamos siempre como
 * keyword: es lo que el usuario espera ver coloreado.
 *
 * Lista derivada de las comparaciones @c current_.lexeme == "..." del
 * parser.  Se EXCLUYEN los que ya son KW_* del lexer (import, extern,
 * await, spawn, match, namespace, ...): esos llegan por su TokenKind y
 * no como IDENTIFIER.  Tampoco se incluyen los nombres de anotaciones
 * (Aspect, Async, Macro, ...): aparecen tras @c @ y se clasifican por su
 * contexto en una fase futura.
 */
bool is_contextual_keyword(const std::string &name) {
    static const std::unordered_set<std::string> kContextualKw = {
        // Declaracion / inferencia de tipo.
        "comptime", "auto", "var",
        // Construcciones comptime / metaprogramacion.
        "foreach", "lazy",
        // Inline asm.
        "register", "clobbers",
        // Imports selectivos y typedef explicit.
        "as", "only", "from",
        // Concurrencia (hints de spawn).
        "here", "on",
        // Datos crudos estilo NASM en bloques bytes.
        "times", "bits",
        // Captura raw de expresion en macros.
        "expr",
    };
    return kContextualKw.count(name) != 0;
}

/**
 * @brief Indica si @p name es un builtin/intrinseco de Vex.
 *
 * Los builtins son IDENTIFIER que el frontend reconoce por nombre: o bien
 * registrados en el type checker (@c reg_builtin en
 * @c src/vex/type_checker.cpp), o evaluados como intrinsecos comptime
 * (@c src/vex/comptime_introspect.cpp) o bajados especialmente en el
 * lowering (@c src/vex/lowering.cpp: reflexion + introspeccion).  Los
 * clasificamos como @c Function para que tengan el color de funcion.
 */
bool is_builtin_name(const std::string &name) {
    static const std::unordered_set<std::string> kBuiltins = {
        // --- I/O (vesta_io) ---
        "print", "println", "echo", "flush", "print_int", "print_uint",
        "print_hex", "print_float", "print_bool", "print_char", "print_color",
        "print_cstr", "print_bin", "print_oct", "print_ptr", "print_gchandle",
        "print_pad", "fopen", "fwrite", "fclose",
        // --- Memoria cruda ---
        "malloc", "free",
        // --- CPU dispatch ---
        "cpu_features",
        // --- Strings (StringObject) ---
        "str_length", "str_bytes", "str_cstr", "str_wstr", "str_hash",
        "str_intern", "str_concat", "str_equals", "str_make", "str_convert",
        // --- Excepciones / secciones / RAII ---
        "panic", "section_start", "section_end", "section_size", "dispose",
        // --- FFI runtime dinamico ---
        "ffi_open", "ffi_sym", "ffi_call",
        // --- Math (vesta_math) ---
        "sqrt", "pow", "fabs", "floor", "ceil", "round", "fmin", "fmax", "log",
        "log2", "log10", "sin", "cos", "tan", "abs", "imin", "imax", "clamp",
        "trunc", "iminu", "imaxu", "ilog2", "popcount", "clz", "ctz", "bswap",
        "rotl", "rotr",
        // --- Callbacks nativos ---
        "as_native_callback",
        // --- Reflexion runtime ---
        "forName", "getClass", "getField", "getMethod", "newInstance",
        "getMethods", "invoke",
        // --- Introspeccion comptime ---
        "static_assert", "sizeof", "alignof", "typename", "type_id", "kind",
        "field_count", "method_count", "is_class", "is_struct", "is_primitive",
        "is_newtype", "offsetof", "has_field", "has_method", "is_subtype",
        "is_same", "comptime_type", "parent_class", "element_type",
        "error_type", "method_name", "method_return_type", "field_name",
        "field_type", "field_type_at",
        // --- Builtins comptime de string + utilidades ---
        "comptime_concat", "comptime_streq", "comptime_strlen", "comptime_chr",
        "comptime_ord", "comptime_substr", "comptime_repeat",
        "comptime_replace", "comptime_contains", "gensym", "comptime_compile",
        "comptime_to_str", "comptime_print",
        // --- Smart pointers / borrow ---
        "unique_box", "shared_box", "unique_with", "shared_with", "move",
        "ptr_of", "use_count", "lend", "lend_mut", "read_borrow",
        "write_borrow",
        // --- Memoria compartida (Phase Z) ---
        "share", "unshare", "is_shared", "shared_malloc", "shared_free",
        "atomic_load_i64", "atomic_store_i64", "atomic_cas_i64",
        "atomic_add_i64",
        // --- Colecciones constructoras ---
        "arraylist", "hashmap", "hashset", "queue", "deque", "treemap",
        "treeset",
        // --- Carga dinamica de modulos ---
        "loadmodule", "unloadmodule",
    };
    return kBuiltins.count(name) != 0;
}

/**
 * @brief Clasifica un IDENTIFIER consultando los sets del analisis.
 *
 * Prioridad (de mayor a menor):
 *   1. keyword contextual (comptime, as, only, ...)  -> Keyword.
 *   2. builtin/intrinseco (static_assert, sizeof, ...) -> Function.
 *   3. nombre declarado (class/struct/enum/type/function) -> su tipo.
 *   4. parametro de plantilla (T en class Box<T>)     -> TypeParameter.
 *   5. resto                                          -> Variable.
 *
 * Un builtin no debe quedar tapado por un nombre declarado homonimo, de
 * ahi que vaya antes que los sets del analisis.  Sin analisis, solo se
 * aplican las reglas 1 y 2 (listas fijas).
 */
SemTokenType classify_identifier(const std::string &name,
                                 const DocAnalysis *an) {
    // 1) Keywords contextuales (lista fija, no depende del analisis).
    if (is_contextual_keyword(name))
        return SemTokenType::Keyword;
    // 2) Builtins/intrinsecos (lista fija, no depende del analisis).
    if (is_builtin_name(name))
        return SemTokenType::Function;
    if (an != nullptr) {
        // 3) Nombres declarados top-level en el documento.
        if (an->class_names.count(name))
            return SemTokenType::Class;
        if (an->struct_names.count(name))
            return SemTokenType::Struct;
        if (an->enum_names.count(name))
            return SemTokenType::Enum;
        if (an->type_names.count(name))
            return SemTokenType::Type;
        if (an->function_names.count(name))
            return SemTokenType::Function;
        // 4) Parametros de plantilla genericos.
        if (an->type_params.count(name))
            return SemTokenType::TypeParameter;
    }
    return SemTokenType::Variable;
}

/**
 * @brief Emite uno o varios AbsToken para un span de bytes [b0, b1) con un
 *        tipo dado, partiendo el span por lineas si cruza saltos.
 *
 * Necesario para comentarios de bloque multilinea: el protocolo no admite
 * un token que cruce lineas, asi que se genera un token por cada linea que
 * el span cubre, usando el indice de posiciones para las coordenadas.
 *
 * @param src   Fuente completo (para detectar fronteras de linea por byte).
 * @param idx   Indice de posiciones byte->(linea, col UTF-16).
 * @param b0    Offset de byte inicial (inclusive).
 * @param b1    Offset de byte final (exclusive).
 * @param type  Tipo de token a emitir.
 * @param out   Destino de los AbsToken generados.
 */
void emit_span_by_lines(const std::string &src, const PositionIndex &idx,
                        size_t b0, size_t b1, SemTokenType type,
                        std::vector<AbsToken> &out) {
    const size_t n = src.size();
    if (b1 > n)
        b1 = n;
    if (b0 >= b1)
        return;
    // Recorrer el span byte a byte agrupando por linea: cuando cambia la
    // linea (o termina el span) cerramos el segmento actual.
    size_t seg_start = b0;
    uint32_t seg_line = 0, seg_col = 0;
    idx.at(b0, seg_line, seg_col);
    for (size_t i = b0; i < b1; ++i) {
        if (src[i] == '\n') {
            // Cerrar el segmento que termina justo antes del salto.
            if (i > seg_start) {
                uint32_t l0, c0, lEnd, cEnd;
                idx.at(seg_start, l0, c0);
                idx.at(i, lEnd, cEnd); // i es la posicion del salto.
                // Si el salto esta en la misma linea (lo normal), la longitud
                // es cEnd - c0.  cEnd corresponde a la columna del '\n'.
                AbsToken t;
                t.line = l0;
                t.start = c0;
                t.length = (lEnd == l0 && cEnd >= c0) ? (cEnd - c0) : 0;
                t.type = static_cast<uint32_t>(type);
                if (t.length > 0)
                    out.push_back(t);
            }
            // El siguiente segmento empieza despues del salto.
            seg_start = i + 1;
        }
    }
    // Cerrar el ultimo segmento (desde seg_start hasta b1).
    if (b1 > seg_start) {
        uint32_t l0, c0, lEnd, cEnd;
        idx.at(seg_start, l0, c0);
        idx.at(b1, lEnd, cEnd);
        AbsToken t;
        t.line = l0;
        t.start = c0;
        // Si b1 cae en otra linea (no deberia, ya cortamos por '\n'),
        // usar el final de la linea via end_col seria mas complejo; aqui
        // b1 esta en la misma linea por construccion.
        t.length = (lEnd == l0 && cEnd >= c0) ? (cEnd - c0)
                                              : (idx.end_col() - c0);
        t.type = static_cast<uint32_t>(type);
        if (t.length > 0)
            out.push_back(t);
    }
}

/**
 * @brief Escanea el fuente y emite tokens @c Comment para // y bloque.
 *
 * Scanner ligero, robusto, que respeta el contexto de string/char para no
 * confundir un // dentro de una cadena con un comentario.  No intenta
 * interpretar la semantica de Vex; solo reconoce:
 *   - @c "..." y @c '...' (con escape @c \\) para SALTARLOS.
 *   - @c r"..." se trata como string normal a efectos de salto (el prefijo
 *     @c r se consume como identificador antes; aqui solo importa la comilla).
 *   - @c //...  hasta fin de linea.
 *   - @c /* ... *\/ posiblemente multilinea (se parte por lineas al emitir).
 *
 * Las cadenas triple-quoted @c """...""" se cubren por el manejo de comillas
 * (se entra y sale del modo string en cada @c "), lo que es conservador pero
 * suficiente para NO marcar como comentario nada que este dentro de ellas en
 * la practica de codigo bien formado.
 *
 * @param src Fuente completo.
 * @param idx Indice de posiciones.
 * @param out Destino de los AbsToken de comentario.
 */
void scan_comments(const std::string &src, const PositionIndex &idx,
                   std::vector<AbsToken> &out) {
    const size_t n = src.size();
    size_t i = 0;
    while (i < n) {
        const char c = src[i];
        // --- Cadenas: saltarlas para no detectar // o /* dentro ---
        if (c == '"' || c == '\'') {
            const char quote = c;
            ++i; // consumir comilla de apertura.
            while (i < n) {
                if (src[i] == '\\') {
                    i += 2; // escape: saltar el caracter escapado.
                    continue;
                }
                if (src[i] == quote) {
                    ++i; // comilla de cierre.
                    break;
                }
                ++i;
            }
            continue;
        }
        // --- Comentario de linea // ---
        if (c == '/' && i + 1 < n && src[i + 1] == '/') {
            size_t start = i;
            i += 2;
            while (i < n && src[i] != '\n')
                ++i;
            emit_span_by_lines(src, idx, start, i, SemTokenType::Comment, out);
            continue;
        }
        // --- Comentario de bloque /* ... */ ---
        if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            size_t start = i;
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/'))
                ++i;
            // Avanzar tras el cierre si existe; si no, hasta el final.
            size_t end = (i + 1 < n) ? (i + 2) : n;
            emit_span_by_lines(src, idx, start, end, SemTokenType::Comment, out);
            i = end;
            continue;
        }
        ++i;
    }
}

} // namespace

const std::vector<std::string> &semantic_token_types() {
    // Orden ESTABLE: cada indice debe corresponder al valor de SemTokenType.
    static const std::vector<std::string> kTypes = {
        "namespace", "type",       "class",    "enum",     "interface",
        "struct",    "typeParameter", "parameter", "variable", "property",
        "enumMember", "function",  "method",   "keyword",  "modifier",
        "comment",   "string",     "number",   "operator", "macro"};
    return kTypes;
}

const std::vector<std::string> &semantic_token_modifiers() {
    // El indice de cada modificador = su numero de bit en SemTokenModifier.
    static const std::vector<std::string> kMods = {
        "declaration", "definition", "readonly",
        "static",      "abstract",   "deprecated"};
    return kMods;
}

std::vector<uint32_t> compute_semantic_tokens(const std::string &text,
                                              const std::string &filename,
                                              const DocAnalysis *analysis) {
    std::vector<AbsToken> abs;
    try {
        PositionIndex idx(text);

        // 1) Lexar y clasificar cada token.  Los comentarios NO los emite el
        //    lexer (los descarta); los anyadimos despues con scan_comments.
        vex::Diagnostics diags; // descartables: solo queremos los tokens.
        vex::Lexer lex(text, filename, diags);
        // Limite defensivo de tokens para no colgar ante una entrada
        // patologica: documentos enormes producen como mucho ~1 token por
        // byte; el tope holgado evita bucles infinitos por un bug del lexer.
        const size_t kMaxTokens = text.size() + 1024;
        size_t produced = 0;
        for (;;) {
            vex::Token tok = lex.next();
            if (tok.kind == vex::TokenKind::END_OF_FILE)
                break;
            if (++produced > kMaxTokens)
                break;

            // Resolver el tipo de token semantico.
            SemTokenType type;
            using TK = vex::TokenKind;
            switch (tok.kind) {
            case TK::STRING_LIT:
            case TK::RAW_STRING_LIT:
            case TK::CHAR_LIT:
            case TK::ISTR_TEXT:
            case TK::ISTR_EXPR_FMT:
                type = SemTokenType::String;
                break;
            case TK::INT_LIT:
            case TK::FLOAT_LIT:
                type = SemTokenType::Number;
                break;
            case TK::TRUE_KW:
            case TK::FALSE_KW:
            case TK::NULL_KW:
                // Literales con apariencia de palabra clave.
                type = SemTokenType::Keyword;
                break;
            case TK::IDENTIFIER:
                type = classify_identifier(tok.lexeme, analysis);
                break;
            default:
                if (is_primitive_type_kw(tok.kind)) {
                    type = SemTokenType::Type;
                } else if (is_keyword(tok.kind)) {
                    type = SemTokenType::Keyword;
                } else if (is_operator(tok.kind)) {
                    type = SemTokenType::Operator;
                } else {
                    // Delimitadores y tokens de control: no emitir.
                    continue;
                }
                break;
            }

            // El span del token puede no tener longitud (sentinelas de string
            // interpolado como ISTR_BEGIN/END): se omitieron arriba por kind.
            // Para los emitidos, length en bytes viene en loc.length.
            if (tok.loc.length == 0)
                continue;

            // Calcular coordenadas LSP del inicio y del final del span.  El
            // lexer da line/column 1-based en bytes; convertimos via el
            // indice de posiciones usando el offset de byte del token.
            const size_t b0 = tok.loc.offset;
            const size_t b1 = b0 + tok.loc.length;
            // Partir por lineas (un token de string triple-quoted podria
            // cruzar lineas).  emit_span_by_lines clampa al final del fuente.
            emit_span_by_lines(text, idx, b0, b1, type, abs);
        }

        // 2) Anyadir los comentarios.
        scan_comments(text, idx, abs);
    } catch (...) {
        // Robustez: devolver lo acumulado (posiblemente vacio) sin lanzar.
    }

    // 3) Ordenar por (linea, caracter).  Estable para deterministico.
    std::stable_sort(abs.begin(), abs.end(),
                     [](const AbsToken &a, const AbsToken &b) {
                         if (a.line != b.line)
                             return a.line < b.line;
                         return a.start < b.start;
                     });

    // 4) Codificar a quintetos con deltas relativos al token anterior.
    std::vector<uint32_t> data;
    data.reserve(abs.size() * 5);
    uint32_t prev_line = 0;
    uint32_t prev_start = 0;
    for (const AbsToken &t : abs) {
        if (t.length == 0)
            continue; // por si acaso: no emitir tokens de longitud cero.
        uint32_t delta_line = t.line - prev_line;
        uint32_t delta_start =
            (delta_line == 0) ? (t.start - prev_start) : t.start;
        data.push_back(delta_line);
        data.push_back(delta_start);
        data.push_back(t.length);
        data.push_back(t.type);
        data.push_back(t.modifiers);
        prev_line = t.line;
        prev_start = t.start;
    }
    return data;
}

} // namespace lsp
