/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file annotate.cpp
 * @brief Que PAPEL juega cada token ambiguo.
 *
 * En Vesta -- como en C, Java o C# -- hay simbolos que significan cosas
 * distintas segun donde esten, y el espaciado correcto depende de cual sea:
 *
 *     a * b     producto, con espacios      i64* p    puntero, pegado
 *     a & b     bits, con espacios          &x        direccion, pegado
 *     a < b     comparacion, con espacios   Caja<i64> tipo, pegado
 *     c ? a : b ternario, con espacios      Animal?   anulable, pegado
 *
 * Mirando solo el token de al lado no se puede saber: `IDENT * IDENT` es lo
 * mismo en `foo(a * b)` que en `f(Punto* p)`.  Hace falta CONTEXTO.
 *
 * Y no hace falta el arbol.  Este pase lleva el estado justo -- si se esta al
 * principio de una sentencia, si lo que se lee es una declaracion, si un `<`
 * cierra como lista de tipos -- y con eso resuelve el papel de cada simbolo en
 * una sola pasada.  Es el mismo reparto que hace clang-format: un pase anota,
 * otro formatea.  Asi el formateador sigue funcionando con codigo a medio
 * escribir, que era la razon de no depender del parser.
 *
 * Lo que de verdad no se pueda decidir se queda en @c Role::Unknown, y quien
 * formatea conserva lo que habia.  Pero eso tiene que ser la excepcion rara, no
 * la respuesta por defecto.
 */

#include "vx/fmt/fmt_internal.h"
#include <set>

#include "vx/parser.h" // la lista de builtins con argumentos de tipo
#include "vx/token.h"

namespace vx {
namespace fmt {
namespace {

/**
 * @brief Indica si el token puede TERMINAR un valor.
 *
 * Es la pregunta que separa un operador binario de uno unario: solo se
 * multiplica algo que ya es un valor.
 *
 * @param k Categoria del token.
 * @return Cierto si lo de detras es un valor completo.
 */
bool ends_value(TokenKind k) {
    switch (k) {
    case TokenKind::IDENTIFIER:
    case TokenKind::INT_LIT:
    case TokenKind::FLOAT_LIT:
    case TokenKind::CHAR_LIT:
    case TokenKind::STRING_LIT:
    case TokenKind::RAW_STRING_LIT:
    case TokenKind::TRUE_KW:
    case TokenKind::FALSE_KW:
    case TokenKind::NULL_KW:
    case TokenKind::RPAREN:
    case TokenKind::RBRACKET:
    case TokenKind::PLUS_PLUS:
    case TokenKind::MINUS_MINUS:
    case TokenKind::KW_THIS:
    case TokenKind::KW_SUPER: return true;
    default: return false;
    }
}

/**
 * @brief Indica si el token puede aparecer DENTRO de un tipo.
 *
 * Es lo que decide si un `<` abre una lista de argumentos de tipo: se mira
 * hacia adelante y, si hasta el `>` que cierra solo hay cosas que caben en un
 * tipo, era un generico.  En cuanto aparece algo que no cabe -- un literal, un
 * `+`, un `;` -- era una comparacion.
 *
 * @param k Categoria del token.
 * @return Cierto si cabe dentro de un tipo.
 */
bool fits_in_type(TokenKind k) {
    if (is_type_keyword(k)) return true;
    switch (k) {
    case TokenKind::IDENTIFIER:
    case TokenKind::COMMA:
    case TokenKind::STAR:
    case TokenKind::AMP:
    case TokenKind::LBRACKET:
    case TokenKind::RBRACKET:
    case TokenKind::QUESTION:
    case TokenKind::DOT:
    case TokenKind::LT:
    case TokenKind::GT:
    case TokenKind::SHR: // `>>` cerrando dos genericos anidados
    /* Un tipo FUNCION tambien es un tipo, y puede ir de argumento:
     * `type.result<cfn(string) -> u64>()`.  Sin sus tres piezas -- la palabra,
     * la flecha y los parentesis de mas abajo -- los angulos se leian como una
     * comparacion y el formateador escribia `type.result < ... > ()`, que ya no
     * es la llamada que habia. */
    case TokenKind::KW_FN:
    case TokenKind::KW_CFN:
    case TokenKind::ARROW:
    case TokenKind::INT_LIT: return true; // `u8[16]`
    default: return false;
    }
}

/**
 * @brief Indica si el token abre una sentencia nueva.
 * @param k Categoria del token.
 * @return Cierto si lo que sigue empieza de cero.
 */
bool starts_statement(TokenKind k) {
    return k == TokenKind::SEMICOLON || k == TokenKind::LBRACE ||
           k == TokenKind::RBRACE || k == TokenKind::COLON;
}

/**
 * @brief Busca el `>` que cierra un `<` que empieza en @p open.
 *
 * @param pieces Todas las piezas.
 * @param open   Indice del `<`.
 * @param close  [out] indice del `>` que cierra, si lo hay.
 * @return Cierto si el `<` abre una lista de argumentos de tipo.
 */
/**
 * @brief Si lo que precede al `<` es un builtin con nombre en ARBOL.
 *
 * Los builtins se llaman `type.size`, `field.count`, `scoped.method.result`, y
 * un argumento de tipo suyo puede ser OTRA LLAMADA -- un tipo calculado, como
 * `type.size<field.type_at<Punto>(0)>()` --.  Ahi dentro hay parentesis, que
 * en un tipo corriente no caben.
 *
 * Preguntarlo es lo que permite aceptarlos SOLO aqui: admitirlos siempre haria
 * que `a < f(x) > b` se leyera como un generico.  Y el modo de fallar de no
 * preguntarlo era de los peores: el formateador partia
 * `type.size<...>()` en `type.size < ... > ()`, o sea que `vm fmt` ROMPIA un
 * programa correcto.
 *
 * @param pieces Las piezas del fichero.
 * @param open   Indice del `<`.
 * @return Cierto si el nombre de delante es un builtin conocido.
 */
/**
 * @brief Si el token puede ser un SEGMENTO de un nombre en arbol.
 *
 * `get` y `set` son palabras clave del lenguaje -- la forma property de una
 * clase --, pero tras un punto son nombres corrientes: `field.get`,
 * `field.set`.  El parser ya lo sabe (@c Parser::is_name_token) y el
 * formateador no, asi que `field.get<Punto>(p, "x")` no se reconocia como
 * argumentos de tipo y salia `field.get < Punto > (p, "x")`, que ya no parsea:
 * `vm fmt` ROMPIA un programa correcto.
 *
 * @param k Categoria del token.
 * @return Cierto si puede ser el nombre, o un trozo de el.
 */
bool is_name_piece(TokenKind k) {
    return k == TokenKind::IDENTIFIER || k == TokenKind::KW_GET ||
           k == TokenKind::KW_SET;
}

bool preceded_by_builtin(const std::vector<Piece> &pieces, size_t open) {
    if (open == 0) return false;
    std::string name;
    size_t i = open; // pieces[i - 1] es el ultimo segmento del nombre
    while (i > 0 && is_name_piece(kind_of(pieces[i - 1]))) {
        name = name.empty() ? std::string(pieces[i - 1].text)
                            : std::string(pieces[i - 1].text) + "." + name;
        if (i >= 2 && kind_of(pieces[i - 2]) == TokenKind::DOT) {
            i -= 2;
            continue;
        }
        break;
    }
    return !name.empty() && is_comptime_builtin_name(name);
}

bool closes_as_type_args(const std::vector<Piece> &pieces, size_t open,
                         size_t &close) {
    /* Antes de un `<` de generico va SIEMPRE un nombre de tipo.  Tras un
     * literal o un `)` solo puede ser una comparacion. */
    if (open == 0) return false;
    const TokenKind before = kind_of(pieces[open - 1]);
    if (!is_name_piece(before) && !is_type_keyword(before)) return false;

    int depth = 0;
    /* Una COTA (`<T: Numerico>`, `<T: A + B>`) tambien va dentro de los
     * angulos, y sus dos tokens -- el `:` y el `+` -- no caben en un tipo.  Sin
     * tratarlos aqui, una cabeza con cota no se reconocia y salia formateada
     * `struct Caja < T : Num >`, con los angulos separados como si compararan.
     *
     * No basta con admitirlos en @c fits_in_type: eso haria que
     * `a < b ? c : d > e` se leyera como un generico.  Lo que distingue a una
     * cota es DONDE esta: justo detras del primer y unico identificador de su
     * grupo, que es lo que aqui se cuenta.  Tras ella solo caben nombres de
     * concepto, sus puntos y los `+` que los suman. */
    unsigned in_group = 0; // tokens vistos desde el ultimo `<` o `,`
    bool after_colon = false;
    /* Un argumento de tipo CALCULADO es una llamada, asi que lleva parentesis
     * y literales dentro.  Solo se admiten cuando quien abre es un builtin
     * (ver @c preceded_by_builtin): en cualquier otro sitio, un parentesis
     * entre angulos es una comparacion. */
    const bool computed_ok = preceded_by_builtin(pieces, open);
    /* Y los parentesis de un tipo FUNCION, que van tras su palabra: `fn(...)`
     * / `cfn(...)`.  Ahi no hay ambiguedad -- `a < fn` no es nada --, asi que
     * se admiten vengan de donde vengan, no solo bajo un builtin. */
    bool fn_type_seen = false;
    // Un tope corto: una lista de tipos larguisima no existe, y sin el una
    // comparacion haria recorrer el fichero entero por cada `<`.
    const size_t limit = open + 64 < pieces.size() ? open + 64 : pieces.size();
    for (size_t i = open; i < limit; ++i) {
        const TokenKind k = kind_of(pieces[i]);
        if (depth == 1 && i > open) {
            if (k == TokenKind::COMMA) {
                in_group = 0;
                after_colon = false;
                continue;
            }
            if (k == TokenKind::COLON && in_group == 1 &&
                kind_of(pieces[i - 1]) == TokenKind::IDENTIFIER) {
                after_colon = true;
                ++in_group;
                continue;
            }
            if (after_colon && (k == TokenKind::PLUS || k == TokenKind::DOT ||
                                k == TokenKind::IDENTIFIER)) {
                ++in_group;
                continue;
            }
            ++in_group;
        }
        if (k == TokenKind::LT) {
            ++depth;
            if (depth == 1) {
                in_group = 0;
                after_colon = false;
            }
            continue;
        }
        if (k == TokenKind::GT) {
            if (--depth == 0) {
                close = i;
                return true;
            }
            continue;
        }
        if (k == TokenKind::SHR) { // `>>` cierra dos de golpe
            depth -= 2;
            if (depth <= 0) {
                close = i;
                return true;
            }
            continue;
        }
        if (k == TokenKind::KW_FN || k == TokenKind::KW_CFN)
            fn_type_seen = true;
        if (i > open && !fits_in_type(k)) {
            if ((computed_ok || fn_type_seen) &&
                (k == TokenKind::LPAREN || k == TokenKind::RPAREN))
                continue;
            if (computed_ok && k == TokenKind::STRING_LIT) continue;
            return false;
        }
    }
    return false;
}

} // namespace

/**
 * @brief Indica si el token nombra un TIPO del lenguaje.
 *
 * Son todas las palabras clave de tipo, y en Vesta esas incluyen las
 * COLECCIONES: `ArrayList`, `HashMap`, `Queue` y compania no son
 * identificadores de una libreria, son tipos primitivos del lenguaje.
 * Tratarlos como nombres corrientes hacia que `HashMap<string, i64>` no se
 * reconociera como generico y saliera formateado `HashMap < string, i64>`.
 *
 * Se usa el RANGO del enum, que va seguido desde `void` hasta `borrow_mut`, en
 * vez de enumerarlas: una lista escrita a mano se queda corta en cuanto el
 * lenguaje gana un tipo, y se queda corta EN SILENCIO.
 *
 * @param k Categoria del token.
 * @return Cierto si nombra un tipo.
 */
bool is_modifier(TokenKind k) {
    switch (k) {
    case TokenKind::KW_PUBLIC:
    case TokenKind::KW_PRIVATE:
    case TokenKind::KW_PROTECTED:
    case TokenKind::KW_STATIC:
    case TokenKind::KW_FINAL:
    case TokenKind::KW_CONST: return true;
    default: return false;
    }
}

bool precedes_type(TokenKind k) {
    if (is_modifier(k)) return true;
    switch (k) {
    case TokenKind::KW_TYPEDEF:
    case TokenKind::KW_USING:
    /* Calificadores que van delante del tipo sin ser modificadores de `R42`:
     * no dicen quien ve la declaracion, dicen algo del valor o de donde vive.
     * Sin saltarlos, `nonnull i64* p` no se leia como declaracion y su `*`
     * salia pegado al nombre. */
    case TokenKind::KW_NONNULL:
    case TokenKind::KW_THREAD_LOCAL: return true;
    default: return false;
    }
}


size_t skip_decl_qualifiers(const std::vector<Piece> &pieces, size_t i) {
    size_t j = i;
    /* EN BUCLE porque los calificadores se combinan: la direccion de un
     * parametro puede llevar un `const` detras (`out const i64** q`), y
     * saltando una sola vez el `const` se quedaba sin saltar -- con lo que
     * `out` pasaba por ser el TIPO y las dos estrellas salian separadas,
     * `i64 * *q`. */
    for (;;) {
        const size_t antes = j;
        while (j < pieces.size() && precedes_type(kind_of(pieces[j])))
            ++j;
        /* Un calificador que el lexer NO entrega como palabra propia del sitio
         * donde esta.
         *
         * `comptime` no tiene token propio, asi que `comptime char* cn` no se
         * leia como declaracion.  Y la direccion de un parametro tiene el mismo
         * problema por partida doble: `out`/`inout` son nombres corrientes --
         * no pueden ser reservados, porque `out` es una instruccion de x86
         * dentro de un bloque `asm` -- e `in` es una palabra reservada, pero de
         * OTRA cosa (el `for (x in col)`).
         *
         * Se reconoce por la FORMA y no por el nombre: algo seguido de un tipo,
         * o de otro calificador, solo puede ser un calificador.  Asi cubre
         * tambien a los que vengan despues sin tener que enumerarlos. */
        if (j + 1 < pieces.size() &&
            (kind_of(pieces[j]) == TokenKind::IDENTIFIER ||
             kind_of(pieces[j]) == TokenKind::KW_IN) &&
            (is_type_keyword(kind_of(pieces[j + 1])) ||
             precedes_type(kind_of(pieces[j + 1]))))
            ++j;
        if (j == antes) break;
    }
    return j;
}

/**
 * @brief Los nombres que ESTE fichero declara como tipos.
 *
 * Hace falta para `R66`: con un tipo integrado, `(i32)n` se reconoce como cast
 * porque `i32` no puede ser otra cosa; con un nombre del usuario no --
 * `(v) - 1` es una resta corriente --, asi que hay que saber cuales de esos
 * nombres son tipos.
 *
 * Un BARRIDO de tokens, no un parseo: se apunta el nombre que va detras de
 * `struct`, `class`, `union`, `interface`, `enum` o `using`, y el que precede
 * al `;` de un `typedef`.  Con eso basta para el caso normal -- un tipo se usa
 * donde se declara --; uno IMPORTADO sigue sin reconocerse, y ahi el
 * formateador deja lo que haya en vez de adivinar.
 *
 * @param pieces Piezas del fuente.
 * @return Los nombres, para preguntar por ellos.
 */
static std::set<std::string_view>
collect_declared_types(const std::vector<Piece> &pieces) {
    std::set<std::string_view> names;
    for (size_t i = 0; i + 1 < pieces.size(); ++i) {
        const TokenKind k = kind_of(pieces[i]);
        if (k == TokenKind::KW_STRUCT || k == TokenKind::KW_CLASS ||
            k == TokenKind::KW_UNION || k == TokenKind::KW_INTERFACE ||
            k == TokenKind::KW_ENUM || k == TokenKind::KW_USING) {
            if (kind_of(pieces[i + 1]) == TokenKind::IDENTIFIER)
                names.insert(pieces[i + 1].text);
            continue;
        }
        /* `typedef u32 Edad new;` y `typedef u32 Edad;`: el nombre es el ultimo
         * IDENTIFICADOR antes del `;`, porque el `new` va detras. */
        if (k != TokenKind::KW_TYPEDEF) continue;
        std::string_view last;
        for (size_t t = i + 1; t < pieces.size(); ++t) {
            const TokenKind tk = kind_of(pieces[t]);
            if (tk == TokenKind::SEMICOLON) break;
            if (tk == TokenKind::IDENTIFIER) last = pieces[t].text;
        }
        if (!last.empty()) names.insert(last);
    }
    return names;
}

std::vector<Role> annotate_roles(const std::vector<Piece> &pieces) {
    std::vector<Role> roles(pieces.size(), Role::Plain);
    // `R66`: que nombres de este fichero son TIPOS, para reconocer sus casts.
    const std::set<std::string_view> tipos = collect_declared_types(pieces);

    /* `decl_until` marca hasta donde llega el TIPO de una declaracion que se
     * esta leyendo.  Mientras se esta dentro, un `*` es un puntero y un `<` una
     * lista de argumentos; fuera, son producto y comparacion. */
    size_t decl_until = 0;
    /* Sitios que vienen justo detras del `)` de un CAST.  Ahi empieza un
     * valor, asi que un `&`, un `*`, un `+` o un `-` son PREFIJOS. */
    std::vector<uint8_t> tras_cast(pieces.size(), 0u);
    /* Profundidad de la lista de PARAMETROS en la que estamos, o -1.
     *
     * Dentro de ella cada elemento es una declaracion, asi que un `*` es un
     * puntero y va pegado al tipo (`R27`).  En una LLAMADA no: ahi `f(a * b)`
     * es una multiplicacion, y por eso no vale con mirar cualquier `(`.  Se
     * distingue por como se llego: el `(` de una declaracion viene justo
     * detras del nombre que `decl_until` acaba de marcar. */
    size_t decl_name = 0; // indice del nombre de la ultima declaracion vista
    int param_paren = -1;
    int depth_paren = 0;
    bool stmt_start = true;
    /* Una ANOTACION es una unidad completa, y lo que viene detras empieza
     * sentencia.  Sin esto, `@alloc(...)` acababa en `)` -- que no abre
     * sentencia -- y la firma de la funcion que decoraba no se leia como
     * declaracion: el `*` de sus parametros salia `i64 *addr` en vez de
     * `i64* addr`, contra la misma regla que la funcion de al lado si cumple.
     *
     * `anot_hasta` es el indice del ultimo token de la anotacion en curso. */
    size_t anot_hasta = 0;

    /* Contexto para los DOS PUNTOS, que en Vesta son seis cosas distintas:
     *
     *     c ? a : b        ternario         -> espacios a los dos lados
     *     class A : B      herencia         -> espacios
     *     for (T x : xs)   recorrido        -> espacios
     *     case Foo:        rama             -> pegado delante
     *     salir:           etiqueta         -> pegado delante
     *     where T: Comp    restriccion      -> pegado delante
     *
     * No se distinguen por lo que tienen al lado, sino por DONDE estan.  Basta
     * con recordar unas pocas cosas mientras se recorre. */
    int ternary_open = 0;     // `?` de ternario esperando su `:`
    int annot_depth = 0;      // profundidad de los parentesis de `@Nombre(`
    bool label_seen = false;  // ya salio el `:` de la etiqueta del argumento
    int paren_depth = 0;      // parentesis abiertos
    bool after_case = false;  // se vio un `case` sin cerrar
    bool after_where = false; // se vio un `where` sin cerrar
    /* Hasta donde llegan unos angulos que son de TIPO.  Lo que hay dentro no
     * son operadores: el `:` de `<T: Numerico>` es el mismo de `where T:
     * Numerico` -- una cota -- y va pegado al nombre, no separado como el de
     * la herencia. */
    size_t type_angle_until = 0;
    bool in_for = false;          // dentro de los parentesis de un `for`
    bool stmt_start_prev = false; // el token anterior abria sentencia
    int for_paren = -1;           // profundidad a la que abrio ese `for`

    for (size_t i = 0; i < pieces.size(); ++i) {
        const TokenKind k = kind_of(pieces[i]);

        /* Al principio de una sentencia, mirar si lo que empieza es una
         * DECLARACION: `[modificadores] TIPO [*|[]|<>] NOMBRE` seguido de `=`,
         * `;`, `,`, `)` o `(`.  Es el unico sitio donde hace falta mirar
         * adelante, y basta con unos pocos tokens. */
        /* En un alias, lo que hay DETRAS del `=` es un tipo entero.
         *
         * `using MyPtr = i64*;` salia `i64 *` porque pasado el `=` ya se
         * estaba leyendo un valor, y ahi un `*` es producto.  El alias es el
         * unico sitio donde una declaracion sigue nombrando tipo despues del
         * `=`, asi que se marca hasta el `;`. */
        /* Y detras de `->` viene el tipo de RETORNO.
         *
         * `fn memcpy(...) -> u8*;` acababa en `u8 *` porque pasado el
         * parentesis ya no se estaba leyendo ninguna declaracion. */
        if (k == TokenKind::ARROW) {
            size_t t = i + 1;
            while (t < pieces.size()) {
                const TokenKind tk = kind_of(pieces[t]);
                if (is_type_keyword(tk) || tk == TokenKind::IDENTIFIER ||
                    tk == TokenKind::STAR || tk == TokenKind::AMP ||
                    tk == TokenKind::LBRACKET || tk == TokenKind::RBRACKET ||
                    tk == TokenKind::QUESTION || tk == TokenKind::INT_LIT ||
                    tk == TokenKind::DOT) {
                    ++t;
                    continue;
                }
                if (tk == TokenKind::LT) {
                    size_t close = 0;
                    if (!closes_as_type_args(pieces, t, close)) break;
                    t = close + 1;
                    continue;
                }
                break;
            }
            // Una pieza MAS ALLA del ultimo token del tipo: el caso `STAR`
            // pregunta `i < decl_until`, asi que la ultima estrella tiene que
            // quedar dentro.
            if (t > i + 1 && t > decl_until) decl_until = t;
        }

        if (k == TokenKind::KW_USING && stmt_start) {
            size_t t = i + 1;
            while (t < pieces.size() &&
                   kind_of(pieces[t]) != TokenKind::SEMICOLON)
                ++t;
            if (t < pieces.size() && t > decl_until) decl_until = t;
        }

        if (k == TokenKind::AT && i + 1 < pieces.size()) {
            size_t t = i + 1; // el nombre de la anotacion
            if (t + 1 < pieces.size() &&
                kind_of(pieces[t + 1]) == TokenKind::LPAREN) {
                int prof = 0;
                for (size_t j = t + 1; j < pieces.size(); ++j) {
                    const TokenKind jk = kind_of(pieces[j]);
                    if (jk == TokenKind::LPAREN)
                        ++prof;
                    else if (jk == TokenKind::RPAREN && --prof == 0) {
                        t = j;
                        break;
                    }
                }
            }
            anot_hasta = t;
        }

        /* Un CAST tambien nombra un tipo: `(u8*) p`.
         *
         * `decl_until` solo se pone al empezar una sentencia, asi que dentro
         * del parentesis de un cast el `*` no era parte de ningun tipo y salia
         * `(u8 *)`, contra la misma regla que fuera cumple.  Se reconoce por
         * la forma -- `( TIPO *... )` --, y se exige al menos un `*`: sin el,
         * `(x)` es un parentesis normal y no hay nada que decidir. */
        if (k == TokenKind::LPAREN && i + 2 < pieces.size()) {
            /* Lo que va DELANTE del tipo lo salta quien ya sabe cuales son.
             *
             * Aqui se daba por hecho que el tipo empezaba justo tras el `(`, y
             * cualquier calificador rompia el reconocimiento: `(const u8*) p`,
             * `cfn(nonnull i64*)` y `cfn(in i64*)` salian con la estrella
             * suelta (`i64 *`) mientras `cfn(i64*)`, al lado, salia bien.
             * Reusar el mismo salto los arregla los tres a la vez, y a los que
             * se anadan despues. */
            const size_t ini = skip_decl_qualifiers(pieces, i + 1);
            const TokenKind tipo =
                ini < pieces.size() ? kind_of(pieces[ini]) : TokenKind::RPAREN;
            if (is_type_keyword(tipo) || tipo == TokenKind::IDENTIFIER) {
                /* Un TIPO con nombre propio ya delata el cast sin necesitar
                 * estrella: `(i64) - 1` es negar, no restar.  Con un
                 * IDENTIFICADOR no se puede decir lo mismo -- `(v) - 1` es una
                 * resta corriente --, y por eso ahi se sigue exigiendo al
                 * menos un `*`. */
                /* Y un nombre que este fichero declara como tipo cuenta igual
                 * que uno integrado: `(Edad)n` es un cast, `(v) - 1` no.  Sin
                 * esto el formateador dejaba las dos formas del primero. */
                const bool tipo_nombrado = is_type_keyword(tipo) ||
                                           (tipo == TokenKind::IDENTIFIER &&
                                            tipos.count(pieces[ini].text) != 0);
                size_t t = ini + 1;
                size_t estrellas = 0;
                while (t < pieces.size()) {
                    const TokenKind tk = kind_of(pieces[t]);
                    if (tk == TokenKind::STAR) {
                        ++estrellas;
                        ++t;
                        continue;
                    }
                    if (tk == TokenKind::LBRACKET ||
                        tk == TokenKind::RBRACKET) {
                        ++t;
                        continue;
                    }
                    break;
                }
                const bool es_cast = (estrellas > 0 || tipo_nombrado) &&
                                     t < pieces.size() &&
                                     kind_of(pieces[t]) == TokenKind::RPAREN;
                if (es_cast) {
                    if (estrellas > 0 && t > decl_until) decl_until = t;
                    /* Tras el `)` de un cast viene un VALOR, asi que lo que
                     * haya ahi es un PREFIJO y no un operador binario:
                     * `(u64)&v`, `(i64)*p`, `(i64)-1`.  Se apunta el sitio
                     * porque quien lo decide solo mira el token de al lado, y
                     * desde ahi un `)` de cast y uno de agrupacion se ven
                     * igual -- `(v) - 1` SI es una resta --. */
                    /* Salvo que lo que siga sea un `->`: entonces esos
                     * parentesis son los PARAMETROS de un tipo funcion
                     * (`fn(i32) -> i32`), que se escribe igual que un cast y no
                     * lo es.  Sin esta salida el `->` se quedaba pegado. */
                    if (t + 1 < pieces.size() &&
                        kind_of(pieces[t + 1]) != TokenKind::ARROW)
                        tras_cast[t + 1] = 1u;
                }
            }
        }

        if (stmt_start && i >= decl_until) {
            size_t j = skip_decl_qualifiers(pieces, i);

            /* Una clase de almacenamiento con argumento -- `register("rsi")`,
             * y cualquiera que venga despues con la misma forma -- tambien va
             * delante del tipo.  Se reconoce por la FORMA: un nombre, sus
             * parentesis, y detras un tipo. */
            if (j + 1 < pieces.size() &&
                kind_of(pieces[j]) == TokenKind::IDENTIFIER &&
                kind_of(pieces[j + 1]) == TokenKind::LPAREN) {
                int prof = 0;
                size_t t = j + 1;
                for (; t < pieces.size(); ++t) {
                    const TokenKind tk = kind_of(pieces[t]);
                    if (tk == TokenKind::LPAREN)
                        ++prof;
                    else if (tk == TokenKind::RPAREN && --prof == 0)
                        break;
                }
                if (t + 1 < pieces.size() &&
                    (is_type_keyword(kind_of(pieces[t + 1])) ||
                     precedes_type(kind_of(pieces[t + 1]))))
                    j = t + 1;
            }
            /* `fn` esta fuera del rango de los tipos porque nombra una
             * FORMA de tipo y no un tipo, pero en una declaracion ocupa su
             * sitio: `fn memcpy(u8* dst, ...)`.  Sin contarlo, ninguna
             * declaracion de un bloque `extern` se leia como tal y sus
             * punteros salian pegados al nombre. */
            const bool named_type =
                j < pieces.size() &&
                (is_type_keyword(kind_of(pieces[j])) ||
                 kind_of(pieces[j]) == TokenKind::KW_FN ||
                 kind_of(pieces[j]) == TokenKind::IDENTIFIER);
            if (named_type) {
                /* Un CONSTRUCTOR no tiene tipo de retorno: su nombre ocupa el
                 * sitio donde los demas ponen el tipo, asi que el patron
                 * `TIPO nombre (` no lo reconoce y los punteros de sus
                 * parametros salian pegados al nombre.
                 *
                 * Se distingue de una LLAMADA por lo que hay tras el `)`: un
                 * cuerpo (`{`) o una expresion (`=>`).  Una llamada lleva ahi
                 * un `;` o un operador. */
                if (j + 1 < pieces.size() &&
                    kind_of(pieces[j + 1]) == TokenKind::LPAREN) {
                    int prof = 0;
                    size_t c = j + 1;
                    for (; c < pieces.size(); ++c) {
                        const TokenKind ck = kind_of(pieces[c]);
                        if (ck == TokenKind::LPAREN)
                            ++prof;
                        else if (ck == TokenKind::RPAREN && --prof == 0)
                            break;
                    }
                    if (c + 1 < pieces.size() &&
                        (kind_of(pieces[c + 1]) == TokenKind::LBRACE ||
                         kind_of(pieces[c + 1]) == TokenKind::FAT_ARROW))
                        decl_name = j;
                }

                size_t t = j + 1;
                // Saltar lo que decora un tipo: punteros, arrays, genericos.
                while (t < pieces.size()) {
                    const TokenKind tk = kind_of(pieces[t]);
                    if (tk == TokenKind::STAR || tk == TokenKind::AMP ||
                        tk == TokenKind::LBRACKET ||
                        tk == TokenKind::RBRACKET ||
                        tk == TokenKind::QUESTION || tk == TokenKind::INT_LIT ||
                        /* `i32 *const q`: el `const` de un puntero constante
                         * va DETRAS de la estrella y sigue siendo tipo. */
                        is_modifier(tk)) {
                        ++t;
                        continue;
                    }
                    if (tk == TokenKind::LT) {
                        size_t close = 0;
                        if (!closes_as_type_args(pieces, t, close)) break;
                        t = close + 1;
                        continue;
                    }
                    break;
                }
                // Tras el tipo tiene que venir un nombre, y tras el nombre algo
                // que solo aparece en una declaracion.
                /* El nombre puede ser una palabra clave SENSIBLE AL CONTEXTO.
                 *
                 * `set` y `get` solo son palabras clave donde declaran una
                 * propiedad; en cualquier otro sitio son nombres corrientes, y
                 * uno de la stdlib se llama asi (`rt_sigprocmask(..., void*
                 * set, void* oset, ...)`).  Pidiendo un IDENTIFIER, esa
                 * declaracion no se reconocia y su `*` salia pegado al nombre
                 * -- con el detalle delator de que en la MISMA linea `oset` si
                 * salia bien --.
                 *
                 * Se acepta cualquier palabra que no nombre un tipo ni
                 * preceda a uno: lo que va delante del `,` o del `)` de un
                 * parametro solo puede ser su nombre. */
                const TokenKind nk =
                    t < pieces.size() ? kind_of(pieces[t]) : TokenKind::LPAREN;
                const bool nombrable =
                    nk == TokenKind::IDENTIFIER ||
                    (nk > TokenKind::IDENTIFIER && !is_type_keyword(nk) &&
                     !precedes_type(nk) && nk != TokenKind::KW_FN);
                const bool has_name = t < pieces.size() && nombrable && t > j;
                if (has_name && t + 1 < pieces.size()) {
                    switch (kind_of(pieces[t + 1])) {
                    case TokenKind::ASSIGN:
                    case TokenKind::SEMICOLON:
                    case TokenKind::COMMA:
                    case TokenKind::RPAREN:
                    case TokenKind::LPAREN:
                        decl_until = t;
                        // `decl_until` se pone a cero en cuanto se alcanza, y
                        // para entonces ya se paso el `(`.  El nombre se
                        // guarda aparte para poder reconocerlo alli.
                        decl_name = t;
                        break;
                    default: break;
                    }
                }
            }
        }

        /* Detras del `)` de un cast empieza un VALOR, asi que lo de antes no
         * "termina un valor" por mucho que sea un `)`.  Sin esto, `(u64)&v`
         * salia `(u64) & v` -- un `y` logico donde hay una direccion --, y lo
         * mismo `(i64)*p` y `(i64)-1`. */
        const bool sigue_a_valor =
            i > 0 && ends_value(kind_of(pieces[i - 1])) && !tras_cast[i];
        switch (k) {
        case TokenKind::STAR:
            if (tras_cast[i]) {
                roles[i] = Role::TightBoth; // `(i64)*p`
            } else if (i < decl_until) {
                roles[i] = Role::TightLeft; // `i64* p`
            } else if (sigue_a_valor) {
                roles[i] = Role::Binary; // `a * b`
            } else {
                roles[i] = Role::TightRight; // `*p`
            }
            break;
        case TokenKind::AMP:
            if (tras_cast[i]) {
                roles[i] = Role::TightBoth; // `(u64)&v`
            } else if (i < decl_until) {
                roles[i] = Role::TightLeft;
            } else if (sigue_a_valor) {
                roles[i] = Role::Binary; // `a & b`
            } else {
                roles[i] = Role::TightRight; // `&x`
            }
            break;
        case TokenKind::PLUS:
        case TokenKind::MINUS:
            /* Normalmente lo decide el espaciador, que con mirar el token de
             * al lado le basta.  Aqui NO: desde ahi un `)` de cast y uno de
             * agrupacion se ven igual, y solo este pase tiene delante el
             * fichero entero para distinguirlos. */
            if (tras_cast[i]) roles[i] = Role::TightBoth; // `(i64)-1`
            break;
        case TokenKind::LT: {
            size_t close = 0;
            if (closes_as_type_args(pieces, i, close)) {
                roles[i] = Role::TightBoth;
                roles[close] = Role::TightLeft;
                // Lo de dentro es un tipo: sus `*` van pegados.
                if (close > decl_until) decl_until = close;
                if (close > type_angle_until) type_angle_until = close;
            } else if (roles[i] == Role::Plain) {
                roles[i] = Role::Binary; // `a < b`
            }
            break;
        }
        case TokenKind::GT:
            // Si no lo marco ya el `<` que abria, es una comparacion.
            if (roles[i] == Role::Plain) roles[i] = Role::Binary;
            break;
        case TokenKind::QUESTION:
            /* `Animal?` va pegado al tipo; `c ? a : b` lleva espacios.  Dentro
             * de una declaracion es anulable; fuera, ternario -- y entonces hay
             * que recordar que su `:` viene despues. */
            if (i < decl_until) {
                roles[i] = Role::TightLeft;
            } else {
                roles[i] = Role::Binary;
                ++ternary_open;
            }
            break;
        case TokenKind::DOTDOTDOT:
            /* `R37`: el variadico va pegado al TIPO y separado del nombre,
             * igual que el `*` de un puntero: `i64... xs`.  Es parte del
             * tipo, no un operador entre dos cosas. */
            roles[i] = Role::TightLeft;
            break;
        case TokenKind::CARET:
            /* Dentro de una anotacion, `^` es un EXPONENTE y no un xor:
             * `O(n^k)` es una formula, y separarla la convierte en una
             * expresion que no lo es.  Fuera, el espaciado normal. */
            if (annot_depth > 0) roles[i] = Role::TightBoth;
            break;
        case TokenKind::COLON:
            if (annot_depth > 0) {
                /* Dentro de una anotacion, `nombre: valor`.
                 *
                 * El PRIMER `:` de cada argumento es el de su etiqueta y va
                 * pegado al nombre, como en un mapa.  Los siguientes son parte
                 * del VALOR y van pegados por los dos lados, porque ahi
                 * `clave:valor` es una sola cosa:
                 *
                 *     @stack(partial: 0, total: 32, when: arch:arm64)
                 *                                         ^^^^ un solo atomo
                 *
                 * Se distinguen por el orden dentro del argumento, que es lo
                 * unico que hace falta: la cuenta se reinicia en cada coma. */
                roles[i] = label_seen ? Role::TightBoth : Role::TightLeft;
                label_seen = true;
            } else if (i < type_angle_until) {
                /* Dentro de unos angulos de tipo solo puede ser una COTA
                 * (`<T: Numerico>`), que es la misma que la clausula `where` y
                 * se escribe igual: pegada al nombre. */
                roles[i] = Role::TightLeft;
            } else if (ternary_open > 0) {
                roles[i] = Role::Binary; // cierra un ternario
                --ternary_open;
            } else if (after_case || after_where) {
                roles[i] = Role::TightLeft; // `case Foo:`, `where T: Comp`
                after_case = false;
                after_where = false;
            } else if (in_for) {
                roles[i] = Role::Binary; // `for (T x : xs)`
            } else if (stmt_start_prev) {
                /* Al principio de una sentencia y sin nada mas: es una
                 * etiqueta de `goto`.  `salir:` va pegado. */
                roles[i] = Role::TightLeft;
            } else {
                roles[i] = Role::Binary; // herencia: `class A : B`
            }
            break;
        default: break;
        }

        // --- Contabilidad del contexto para la proxima vuelta. ---
        if (k == TokenKind::LPAREN) {
            ++depth_paren;
            // El `(` que sigue al nombre de una declaracion abre parametros.
            if (param_paren < 0 && i > 0 && decl_name == i - 1 &&
                kind_of(pieces[i - 1]) == TokenKind::IDENTIFIER)
                param_paren = depth_paren;

            /* Y el de un TIPO funcion: `fn(...) -> R` / `cfn(...) -> R`.
             *
             * Lo de dentro son parametros igual que en cualquier otro sitio, y
             * sin decirlo se quedaban sin el tratamiento comun -- la direccion
             * (`in`/`out`) no se reconocia como calificador y `cfn(in i64*)`
             * salia `cfn(in i64 *)`, con la estrella suelta --.  Se marca aqui
             * en vez de anadir una regla propia: la lista de parametros de un
             * tipo no es un caso distinto. */
            if (param_paren < 0 && i > 0 &&
                (kind_of(pieces[i - 1]) == TokenKind::KW_FN ||
                 kind_of(pieces[i - 1]) == TokenKind::KW_CFN))
                param_paren = depth_paren;

            /* Y el de una LAMBDA, que no lleva nombre delante.
             *
             * `(i64* p) => { ... }` empieza por el parentesis, asi que no hay
             * declaracion que lo anuncie y sus punteros salian pegados al
             * nombre.  Lo que lo delata es el `=>` que viene tras el cierre:
             * ningun otro parentesis lo tiene detras salvo el patron de un
             * `case`, y ahi dentro no hay `TIPO nombre` que confundir. */
            if (param_paren < 0) {
                int prof = 0;
                size_t c = i;
                for (; c < pieces.size(); ++c) {
                    const TokenKind ck = kind_of(pieces[c]);
                    if (ck == TokenKind::LPAREN)
                        ++prof;
                    else if (ck == TokenKind::RPAREN && --prof == 0)
                        break;
                }
                if (c + 1 < pieces.size() &&
                    kind_of(pieces[c + 1]) == TokenKind::FAT_ARROW)
                    param_paren = depth_paren;
            }
            ++paren_depth;
            if (i > 0 && kind_of(pieces[i - 1]) == TokenKind::KW_FOR) {
                in_for = true;
                for_paren = paren_depth;
            }
            /* `@Nombre(`: los parentesis de una anotacion.  Se anota su
             * profundidad para saber cuando se cierra, porque dentro puede
             * haber otros parentesis que no son suyos (`O(n)`). */
            if (i >= 2 && kind_of(pieces[i - 1]) == TokenKind::IDENTIFIER &&
                kind_of(pieces[i - 2]) == TokenKind::AT) {
                annot_depth = paren_depth;
                label_seen = false;
            }
        } else if (k == TokenKind::RPAREN) {
            if (param_paren == depth_paren) param_paren = -1;
            if (depth_paren > 0) --depth_paren;
            if (annot_depth > 0 && paren_depth == annot_depth) {
                annot_depth = 0;
                label_seen = false;
            }
            if (in_for && paren_depth == for_paren) {
                in_for = false;
                for_paren = -1;
            }
            if (paren_depth > 0) --paren_depth;
        }
        // Cada argumento de la anotacion tiene su propia etiqueta.
        if (k == TokenKind::COMMA && annot_depth > 0) label_seen = false;
        if (k == TokenKind::KW_CASE) after_case = true;
        if (k == TokenKind::IDENTIFIER && pieces[i].text == "where")
            after_where = true;
        if (k == TokenKind::SEMICOLON || k == TokenKind::LBRACE) {
            // Una sentencia nueva no hereda ternarios ni restricciones a
            // medias.
            ternary_open = 0;
            after_case = false;
            after_where = false;
        }

        /* `R66`: un cast va PEGADO a lo que convierte.  La marca de "esto va
         * detras del `)` de un cast" ya estaba -- se usaba para no leer
         * `(u64)&v` como un `y` logico --, pero solo para los tokens que se
         * podian confundir con un operador.  Para un nombre o un numero no se
         * decidia nada y el espaciador metia el espacio que pone tras cualquier
         * `)`, asi que `(i32)largo` y `(i32) largo` convivian: dos formas para
         * el mismo programa, que es lo que `P1` prohibe. */
        if (tras_cast[i] && roles[i] == Role::Plain) {
            /* Pegado DELANTE, que es lo que dice la regla.  Y si lo que sigue
             * abre un grupo, pegado por los dos lados: detras de un `(` no va
             * espacio de todas formas, y con `TightLeft` se colaba uno dentro
             * -- `(i32)( largo + 1)` --. */
            roles[i] = (k == TokenKind::LPAREN || k == TokenKind::LBRACKET)
                           ? Role::TightBoth
                           : Role::TightLeft;
        }

        // Una etiqueta es un NOMBRE al principio de sentencia, y nada mas.
        stmt_start_prev = stmt_start && k == TokenKind::IDENTIFIER;
        stmt_start = starts_statement(k) ||
                     (anot_hasta != 0 && i == anot_hasta) ||
                     (param_paren >= 0 &&
                      (k == TokenKind::LPAREN || k == TokenKind::COMMA));
        if (i >= decl_until) decl_until = 0;
    }
    return roles;
}

} // namespace fmt
} // namespace vx
