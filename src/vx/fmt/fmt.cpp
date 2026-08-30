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
 * @file fmt.cpp
 * @brief Orquestacion del formateador y reglas de espacio en blanco.
 *
 * Aqui se decide el formato; recuperar la trivia es cosa de `trivia.cpp`.  Ver
 * `include/vx/fmt/fmt.h` para las decisiones de fondo y
 * `doc/VMdoc/Vesta/EstiloYFormato.md` para las reglas.
 *
 * ORDEN DE CONSTRUCCION, y es deliberado: primero la RED -- las dos propiedades
 * comprobadas sobre el corpus entero -- y luego las reglas.  Al reves, cada
 * regla nueva seria un salto sin red y el primer fallo se descubriria en el
 * fichero de alguien.  Por eso esta fase reconstruye el texto sin reformatear
 * casi nada: lo que aporta es la garantia, no el aspecto.
 */

#include "vx/fmt/fmt_internal.h"

#include "vx/diagnostic.h"
#include "vx/lexer.h"

namespace vx {
namespace fmt {
namespace {

/**
 * @brief Indica si el texto trae retornos de carro.
 *
 * Se pregunta antes de copiar: la inmensa mayoria de los ficheros no los tiene,
 * y en ese caso no hay que duplicar el fuente en memoria para nada.
 *
 * @param source Texto a mirar.
 * @return Cierto si hay algun `\r`.
 */
bool has_carriage_returns(std::string_view source) {
    return source.find('\r') != std::string_view::npos;
}

/**
 * @brief Copia un texto quitando los retornos de carro (`R11`).
 *
 * Se hace sobre el fuente ENTERO antes de tokenizar, no linea a linea: un `\r`
 * dentro de una cadena tambien es un fin de linea mal puesto, y dejarlo ahi
 * haria que el fichero se viera distinto segun el sistema.
 *
 * @param source Texto de entrada.
 * @return El mismo texto sin `\r`.
 */
std::string strip_carriage_returns(std::string_view source) {
    std::string out;
    out.reserve(source.size());
    for (const char c : source)
        if (c != '\r') out.push_back(c);
    return out;
}

/**
 * @brief Anade un tramo de texto quitando el blanco al final de cada linea.
 *
 * Los blancos se retienen hasta saber que viene detras: si es un salto de
 * linea, sobraban; si es cualquier otra cosa, se reponen TAL CUAL.  Hay que
 * guardarlos y no contarlos, porque un tabulador y un espacio no son
 * intercambiables -- `R1` y `R2` los usan para cosas distintas.
 *
 * El estado va fuera para poder llamar a esto tramo a tramo mientras se
 * reconstruye, y asi no hacer una segunda pasada sobre todo el fichero.
 *
 * @param chunk   Tramo a anadir.
 * @param out     [in,out] salida que se va construyendo.
 * @param pending [in,out] blancos retenidos entre llamadas.
 */
void append_trimming_eol(std::string_view chunk, std::string &out,
                         std::string &pending) {
    for (const char c : chunk) {
        if (c == ' ' || c == '\t') {
            pending.push_back(c);
            continue;
        }
        if (c != '\n') out += pending;
        pending.clear();
        out.push_back(c);
    }
}

} // namespace

/**
 * @brief Indica si dos textos son el MISMO programa escrito distinto.
 *
 * Compara la tira de tokens: si coincide, lo unico que se ha movido es espacio
 * en blanco, que es lo que el formateador tiene permitido tocar (`P2`).  La
 * posicion se ignora a proposito -- es justo lo que cambia --.
 *
 * No construye ninguna lista: recorre los dos a la vez y para en cuanto
 * discrepan, que en el caso bueno es el final y en el malo, enseguida.
 *
 * @param before Texto original.
 * @param after  Texto formateado.
 * @return Cierto si los dos dan la misma tira de tokens.
 */
bool same_program(std::string_view before, std::string_view after,
                  const std::vector<Rewrite> &rewrites) {
    /* Se leen las DOS tiras de tokens enteras y luego se recorren en paralelo.
     * Leerlas antes cuesta un poco de memoria y ahorra mucho lio: al comparar
     * hace falta mirar hacia adelante -- un `>>` que sustituye a dos `>`, unos
     * parentesis que ya no estan --, y con un lexer que solo va hacia adelante
     * eso no se puede. */
    const auto leer = [](std::string_view src, const char *nombre) {
        Diagnostics d;
        Lexer lx(std::string(src), nombre, d);
        std::vector<Token> out;
        for (;;) {
            Token t = lx.next();
            const bool fin = t.kind == TokenKind::END_OF_FILE;
            out.push_back(std::move(t));
            if (fin) break;
        }
        return out;
    };
    const std::vector<Token> A = leer(before, "<antes>");
    const std::vector<Token> B = leer(after, "<despues>");

    /// Dos tokens son el mismo si coinciden en clase y en lo que valen.
    const auto igual = [](const Token &a, const Token &b) {
        if (a.kind != b.kind) return false;
        /* En un literal numerico se compara el VALOR y no el lexema: `R106`
         * reescribe `0xf` como `0x0F`, que es el mismo numero escrito con la
         * anchura de su tipo.  Comparar el valor es ademas mas estricto que
         * comparar el texto, porque es lo que el programa acaba usando. */
        const bool numeric =
            a.kind == TokenKind::INT_LIT || a.kind == TokenKind::FLOAT_LIT;
        if (!numeric && a.lexeme != b.lexeme) return false;
        if (numeric && a.suffix != b.suffix) return false;
        if (numeric && a.flt_val != b.flt_val) return false;
        // El valor ya resuelto tambien: dos cadenas pueden tener el mismo
        // lexema y distinto contenido si un escape se colo por medio.
        if (a.str_val != b.str_val) return false;
        if (a.int_val != b.int_val) return false;
        return true;
    };

    /* La reescritura declarada para el token @p k del original, si queda
     * alguna sin usar.
     *
     * Se marcan al consumirlas porque hay reescrituras que NO avanzan en el
     * original -- `AddBraces` anade una llave que alli no existe --, y sin la
     * marca la misma se aplicaba una y otra vez, comiendose el resto del
     * fichero token a token. */
    std::vector<bool> usada(rewrites.size(), false);
    const auto declarada = [&rewrites, &usada](size_t k, RewriteKind *kind,
                                               size_t *cual) {
        for (size_t r = 0; r < rewrites.size(); ++r)
            if (!usada[r] && rewrites[r].at == k) {
                *kind = rewrites[r].kind;
                *cual = r;
                return true;
            }
        return false;
    };

    size_t ia = 0, ib = 0;
    while (ia < A.size() && ib < B.size()) {
        /* Una reescritura declarada manda sobre la igualdad.
         *
         * Hace falta porque dos tokens IGUALES pueden ser distintos: la `}`
         * que `R6` anade y la que ya cerraba la funcion son las dos un
         * `RBRACE`, y compararlas por igualdad las apareaba entre si -- con lo
         * que al final sobraba una y el fichero se rechazaba entero. */
        RewriteKind kind;
        size_t cual = 0;
        const bool hay = declarada(ia, &kind, &cual);
        if (!hay && igual(A[ia], B[ib])) {
            if (A[ia].kind == TokenKind::END_OF_FILE) return true;
            ++ia;
            ++ib;
            continue;
        }
        /* Difieren.  Solo se acepta si el formateador DECLARO haber hecho ahi
         * esa transformacion, y si lo que se ve encaja con lo que esa
         * transformacion produce.  Cualquier otra diferencia es un fallo. */
        if (!hay) return false;
        usada[cual] = true;
        switch (kind) {
        case RewriteKind::GlueGenericClose:
            // `R29`: dos `>` seguidos pasan a ser un `>>`.
            if (ia + 1 >= A.size()) return false;
            if (A[ia].kind != TokenKind::GT || A[ia + 1].kind != TokenKind::GT)
                return false;
            if (B[ib].kind != TokenKind::SHR) return false;
            ia += 2;
            ib += 1;
            break;
        case RewriteKind::DropEmptyParens:
            // `R74`: `@X()` pierde su `(` y su `)`, que no llevaban nada.
            if (ia + 1 >= A.size()) return false;
            if (A[ia].kind != TokenKind::LPAREN ||
                A[ia + 1].kind != TokenKind::RPAREN)
                return false;
            ia += 2; // en el formateado no hay nada que consumir
            break;
        case RewriteKind::SwapModifiers:
            // `R42`: dos modificadores cambian de orden entre si.
            if (ia + 1 >= A.size() || ib + 1 >= B.size()) return false;
            if (!igual(A[ia], B[ib + 1]) || !igual(A[ia + 1], B[ib]))
                return false;
            ia += 2;
            ib += 2;
            break;
        case RewriteKind::AddBraces:
            // `R6`: aparece una llave que en el original no estaba.
            if (B[ib].kind != TokenKind::LBRACE &&
                B[ib].kind != TokenKind::RBRACE)
                return false;
            ib += 1; // en el original no hay nada que consumir
            break;
        }
    }
    // Las dos tiras tienen que acabar a la vez.
    return ia == A.size() && ib == B.size();
}

FormatResult format(const std::string &source, const std::string &filename,
                    const FormatOptions &options) {
    FormatResult r;
    r.text = source;

    /* `R11`: los fines de linea se normalizan antes de nada, para que el resto
     * del formateador no tenga que pensar en ellos.  Si no hay ninguno -- el
     * caso normal -- no se copia el fuente. */
    std::string normalized;
    std::string_view text = source;
    if (has_carriage_returns(source)) {
        normalized = strip_carriage_returns(source);
        text = normalized;
    }

    std::string_view tail;
    std::string code;
    std::vector<Piece> pieces = scan_pieces(text, filename, tail, code);
    if (!code.empty()) {
        // Ante la duda, el fichero del usuario se devuelve intacto.
        r.ok = false;
        r.code = code;
        return r;
    }

    /* `R110`: el interior de las llamadas que capturan el texto de su
     * argumento se marca ANTES que nada, para que ni la forma canonica de los
     * literales ni la indentacion lo toquen. */
    mark_verbatim_calls(pieces, options.raw_capture_names);

    /* `R29`, `R42`, `R74`: las reglas que cambian TOKENS.
     *
     * Cada una declara lo que hace, y la comprobacion de mas abajo exige que
     * la diferencia entre el antes y el despues sea exactamente esa.  Van aqui
     * -- antes de medir -- porque cambian la anchura de la linea, y de eso
     * dependen el reparto y las columnas. */
    std::vector<Rewrite> rewrites = apply_token_rules(pieces);
    /* Las llaves que `R6` anade se apuntan en la emision FINAL: hasta que no
     * se reparte no se sabe si el cuerpo cupo en la linea de su cabecera, y de
     * eso depende si las necesita. */
    std::vector<Rewrite> llaves;

    /* `R106`, `R107`: la forma canonica de los literales numericos.
     *
     * Va antes de medir porque cambia la ANCHURA (`0xF` pasa a `0x0F`), y de
     * esa anchura dependen el reparto de lineas largas y las columnas de la
     * alineacion.  Los textos nuevos viven en `literals`, que se reserva de
     * una vez para que los `string_view` de las piezas no se queden colgando
     * al crecer el vector. */
    std::vector<std::string> literals;
    literals.reserve(pieces.size());
    for (Piece &p : pieces) {
        if (p.kind != (int)TokenKind::INT_LIT &&
            p.kind != (int)TokenKind::FLOAT_LIT)
            continue;
        // Dentro de `${...}` no es una pieza suelta, y dentro de una captura
        // de texto el literal es contenido del autor (`R110`).
        if (p.in_string || p.verbatim) continue;
        std::string canonical = canonical_literal(p.text);
        if (canonical.empty()) continue;
        literals.push_back(std::move(canonical));
        p.text = literals.back();
    }

    /* Indentacion y espaciado (`R1`, `R4`-`R11`, `R33`-`R35`, `R52`, `R61`+),
     * y luego las columnas (`R83`-`R88`).
     *
     * Se emite DOS VECES: la primera para medir en que columna cae cada cosa
     * por si sola, y la segunda ya con el relleno.  Alinear es precisamente
     * decidir en funcion de las vecinas, asi que no hay forma de hacerlo en una
     * sola pasada. */
    const std::vector<Role> roles = annotate_roles(pieces);

    /* 1-2) Medir y repartir, HASTA QUE NO CAMBIE.
     *
     * Una sola vuelta no basta y el motivo se ve enseguida: al repartir una
     * lista, uno de sus elementos puede seguir pasando de 80 el solo -- una
     * llamada anidada, por ejemplo --, y entonces hay que repartir tambien esa.
     * Con una vuelta unica el resultado dependia de cuantas veces se hubiera
     * formateado el fichero, que es exactamente lo que la idempotencia
     * prohibe; se veia como 21 ficheros del corpus que cambiaban al formatear
     * dos veces.
     *
     * El tope existe porque un punto fijo que no llega es un cuelgue: cuatro
     * vueltas cubren cualquier anidamiento real, y si no converge se emite lo
     * que haya, que es una forma valida aunque no la mejor. */
    Layout wrapped;
    std::vector<Break> breaks;
    for (int round = 0; round < 4; ++round) {
        Layout medida;
        reindent(pieces, tail, options, nullptr, &medida,
                 breaks.empty() ? nullptr : &breaks);
        std::vector<Break> next = compute_breaks(pieces, medida, options);
        // Los cortes se ACUMULAN: los de la vuelta anterior siguen valiendo.
        if (next.size() == breaks.size()) {
            bool grew = false;
            for (size_t i = 0; i < next.size(); ++i) {
                if (next[i].before && !breaks[i].before) {
                    breaks[i] = next[i];
                    grew = true;
                }
            }
            wrapped = std::move(medida);
            if (!grew) break; // punto fijo: nadie pidio un corte nuevo
        } else {
            breaks = std::move(next);
            wrapped = std::move(medida);
        }
    }

    // 4) Alinear las columnas de lo que quedo (`R83`-`R88`).
    const std::vector<uint32_t> pad =
        compute_alignment(pieces, roles, wrapped, options);

    /* 5) Alinear los comentarios de fin de linea (`R20`).  Van aparte porque un
     *    comentario no es un token: vive en la trivia y necesita su propio
     *    relleno.  Se mide sobre el layout ya repartido. */
    const std::vector<uint32_t> cpad =
        compute_comment_alignment(pieces, wrapped, options);

    // 6) Y emitir ya con todo.
    const std::string indented =
        reindent(pieces, tail, options, &pad, nullptr, &breaks, &cpad, &llaves);

    /* `R9`: sin blanco al final de ninguna linea.  Va el ultimo a proposito:
     * asi hace de red para cualquier regla anterior que dejara un resto, en vez
     * de tener que acordarse cada una por su cuenta. */
    std::string out;
    out.reserve(indented.size());
    std::string pending;
    append_trimming_eol(indented, out, pending);

    /* SALVAGUARDA: `P2` comprobado AQUI, no solo en el test.
     *
     * El test dice que las reglas de hoy no cambian el programa; esto dice que
     * lo que sale AHORA MISMO no lo cambia, sea cual sea el fichero.  Y hace
     * falta: la primera version de la indentacion metia espacios dentro de las
     * cadenas interpoladas -- `"total: ${n}"` no es un token, es una tira, y lo
     * que hay entre sus piezas ES la cadena --.  Sin esta comprobacion eso
     * llega al fichero de alguien y nadie se entera hasta que el programa hace
     * otra cosa.
     *
     * Un fallo aqui NUNCA es silencioso: se devuelve el fichero INTACTO y un
     * codigo del catalogo que dice exactamente que ha pasado.  Cuesta una
     * tokenizacion mas, que es lineal y barata al lado de escribir el fichero.
     */
    // Las llaves de `R6` cuentan igual que el resto de reescrituras.
    rewrites.insert(rewrites.end(), llaves.begin(), llaves.end());
    if (!same_program(text, out, rewrites)) {
        r.ok = false;
        r.code = "VXF004";
        r.text = source; // intacto: mejor sin formatear que mal formateado
        return r;
    }

    r.text = std::move(out);
    r.changed = (r.text != source);
    return r;
}

} // namespace fmt
} // namespace vx
