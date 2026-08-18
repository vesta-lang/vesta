/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file value_range_domain.cpp
 * @brief Semantica del dominio de intervalos: reticulo, aritmetica,
 * conversiones y restricciones (contrato en value_range.h).
 *
 * SEPARADO del motor de flujo a proposito.  Aqui no hay CFG, ni bloques, ni
 * punto fijo: solo numeros y tipos.  Se puede probar entero sin construir una
 * funcion IR, y esa es justo la propiedad que hace fiable lo que el motor
 * despues compone.
 *
 * METODO de toda la aritmetica, uno solo y aplicado igual en todas partes:
 *
 *   1. DESPLEGAR   los extremos a enteros sin limite, leidos segun el tipo.
 *   2. OPERAR      ahi, donde nada desborda ni hay comportamiento indefinido.
 *   3. PLEGAR      el resultado al tipo, que es donde ENVUELVE.
 *
 * El paso 3 es el que distingue este dominio de uno ingenuo.  Un `u8` con
 * `250 + 10` da 4, no 260: el conjunto exacto tiene UN valor y ese valor cabe,
 * asi que la respuesta es exacta.  Y cuando el conjunto exacto DA LA VUELTA al
 * tipo -- `u8 [250,260]` -> `{250..255} U {0..4}` -- no hay intervalo simple
 * que lo diga y la respuesta es el tipo entero: menos preciso, nunca falso.
 *
 * NUNCA se responde BOTTOM por no caber.  BOTTOM significa "aqui no se llega",
 * y una operacion que envuelve produce valores perfectamente reales.
 */
#include "analysis/facts/value_range.h"

#include <algorithm>

namespace analysis {

namespace {

// ===========================================================================
//  Enteros sin limite (dentro de lo razonable)
//
//  Los extremos vienen de tipos de 64 bits como mucho, asi que 128 bits bastan
//  para sumar, restar y multiplicar dos de ellos sin desbordar jamas.  Donde no
//  hay enteros de 128 bits se opera en 64 con deteccion de desbordamiento y lo
//  que no cabe se responde como "no representable", que el plegado convierte en
//  el tipo entero.
// ===========================================================================
#if defined(__SIZEOF_INT128__)
/* `__extension__` porque el estandar no tiene enteros de 128 bits: es una
 * extension del compilador, y decirlo evita que -Wpedantic la rechace. */
__extension__ typedef __int128 Ancho;
inline bool suma_ancha(Ancho a, Ancho b, Ancho &o) {
    o = a + b;
    return true;
}
inline bool resta_ancha(Ancho a, Ancho b, Ancho &o) {
    o = a - b;
    return true;
}
inline bool mul_ancha(Ancho a, Ancho b, Ancho &o) {
    o = a * b;
    return true;
}
#else
using Ancho = int64_t;
inline bool suma_ancha(Ancho a, Ancho b, Ancho &o) {
#if defined(__GNUC__) || defined(__clang__)
    return !__builtin_add_overflow(a, b, &o);
#else
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b))
        return false;
    o = a + b;
    return true;
#endif
}
inline bool resta_ancha(Ancho a, Ancho b, Ancho &o) {
#if defined(__GNUC__) || defined(__clang__)
    return !__builtin_sub_overflow(a, b, &o);
#else
    if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b))
        return false;
    o = a - b;
    return true;
#endif
}
inline bool mul_ancha(Ancho a, Ancho b, Ancho &o) {
#if defined(__GNUC__) || defined(__clang__)
    return !__builtin_mul_overflow(a, b, &o);
#else
    if (a == 0 || b == 0) {
        o = 0;
        return true;
    }
    if (a > 0) {
        if (b > 0) {
            if (a > INT64_MAX / b) return false;
        } else {
            if (b < INT64_MIN / a) return false;
        }
    } else {
        if (b > 0) {
            if (a < INT64_MIN / b) return false;
        } else {
            if (a < INT64_MAX / b) return false;
        }
    }
    o = a * b;
    return true;
#endif
}
#endif

/**
 * @brief Division en el entero ancho, sin caer en comportamiento indefinido.
 *
 * Dos casos que hay que apartar ANTES de dividir, no despues: el divisor cero y
 * el minimo entre menos uno (cuyo cociente no cabe en el tipo con signo).  Con
 * enteros de 128 bits el segundo si cabe y da el resultado envuelto correcto;
 * sin ellos, no hay nada que calcular.
 */
inline bool div_ancha(Ancho a, Ancho b, Ancho &o) {
    if (b == 0) return false;
    if (sizeof(Ancho) == 8 && b == -1 && a == static_cast<Ancho>(INT64_MIN))
        return false;
    o = a / b;
    return true;
}

/// Valor absoluto sin desbordar: el minimo con signo no tiene opuesto en su
/// propio ancho, asi que ese caso se declara no calculable en vez de negarlo.
inline bool valor_absoluto(Ancho v, Ancho &o) {
    if (v >= 0) {
        o = v;
        return true;
    }
    if (sizeof(Ancho) == 8 && v == static_cast<Ancho>(INT64_MIN)) return false;
    o = -v;
    return true;
}

/// Conjunto exacto de una operacion, antes de plegarlo al tipo.
struct Exacto {
    bool ok = false; ///< false = no cabe ni en el entero ancho: no se afirma
    Ancho lo = 0, hi = 0;
};

/// Los bits de un extremo, leidos como NUMERO segun el tipo.
Ancho desplegar(RangeType t, uint64_t crudo) {
    if (t.sin_signo) return static_cast<Ancho>(t.normalizar(crudo));
    return static_cast<Ancho>(t.hacia_signo(crudo));
}

/**
 * @brief Devuelve el conjunto exacto al tipo, envolviendo como el hardware.
 *
 * Dos motivos para responder el tipo entero, y ninguno de los dos es BOTTOM:
 * que el conjunto tenga mas valores de los que el tipo distingue, o que tras
 * envolver quede partido en dos trozos (el intervalo daria la vuelta).
 */
ValueRange plegar(RangeType t, const Exacto &e) {
    if (!e.ok) return ValueRange::todo(t);
    Ancho anchura = e.hi - e.lo;
    if (anchura < 0)
        return ValueRange::todo(t); // no deberia pasar; no se afirma
    const uint64_t card = t.cardinal();
    if (card == 0) {
        // 64 bits: solo cubre el tipo si la anchura llega a 2^64.
        if (sizeof(Ancho) > 8) {
            const Ancho dos64 = (static_cast<Ancho>(1) << 63) * 2;
            if (anchura >= dos64) return ValueRange::todo(t);
        }
    } else if (anchura >= static_cast<Ancho>(card)) {
        return ValueRange::todo(t);
    }
    // `crudo` normaliza y, si el intervalo quedo invertido, responde el tipo.
    return ValueRange::crudo(t, static_cast<uint64_t>(e.lo),
                             static_cast<uint64_t>(e.hi));
}

/// Las dos operandas comparten tipo y estan acotadas.  Si no, no hay operacion.
bool operables(const ValueRange &a, const ValueRange &b) {
    return a.acotada() && b.acotada() && a.t == b.t;
}

} // namespace

// ===========================================================================
//  Reticulo
// ===========================================================================

ValueRange ValueRange::unir(const ValueRange &o) const {
    if (es_bottom()) return o;
    if (o.es_bottom()) return *this;
    if (es_top() || o.es_top() || t != o.t) return top(t);
    return armar(t, t.menor_de(lo_c, o.lo_c), t.mayor_de(hi_c, o.hi_c));
}

ValueRange ValueRange::cortar(const ValueRange &o) const {
    if (es_bottom() || o.es_bottom()) return bottom(t);
    if (es_top()) return o;
    if (o.es_top()) return *this;
    /* Tipos distintos: mezclar dominios es un fallo del IR o del llamante.  Se
     * ignora la restriccion (se afirma menos) en vez de concluir BOTTOM, que
     * convertiria un fallo nuestro en "este codigo no se ejecuta". */
    if (t != o.t) return *this;
    return corte(t, t.mayor_de(lo_c, o.lo_c), t.menor_de(hi_c, o.hi_c));
}

ValueRange ValueRange::ensanchar(const ValueRange &nuevo) const {
    if (es_bottom()) return nuevo;
    if (!acotada() || !nuevo.acotada() || t != nuevo.t) return nuevo;
    uint64_t l = nuevo.lo_c, h = nuevo.hi_c;
    if (t.menor(l, lo_c)) l = t.min_crudo(); // bajo mas: se suelta por abajo
    if (t.menor(hi_c, h)) h = t.max_crudo(); // subio mas: se suelta por arriba
    return armar(t, l, h);
}

// ===========================================================================
//  Aritmetica
// ===========================================================================

ValueRange ValueRange::sumar(const ValueRange &o) const {
    if (es_bottom() || o.es_bottom()) return bottom(t);
    if (!operables(*this, o)) return top(t);
    Exacto e;
    e.ok = suma_ancha(desplegar(t, lo_c), desplegar(t, o.lo_c), e.lo) &&
           suma_ancha(desplegar(t, hi_c), desplegar(t, o.hi_c), e.hi);
    return plegar(t, e);
}

/// El minimo sale de `a.lo - b.hi` y el maximo de `a.hi - b.lo`: los extremos
/// CRUZADOS.  Restar el minimo al minimo daria un intervalo que no existe.
ValueRange ValueRange::restar(const ValueRange &o) const {
    if (es_bottom() || o.es_bottom()) return bottom(t);
    if (!operables(*this, o)) return top(t);
    Exacto e;
    e.ok = resta_ancha(desplegar(t, lo_c), desplegar(t, o.hi_c), e.lo) &&
           resta_ancha(desplegar(t, hi_c), desplegar(t, o.lo_c), e.hi);
    return plegar(t, e);
}

/// Producto por las CUATRO esquinas: con signos mezclados el minimo no es el
/// producto de los minimos (`[-2,3] * [-5,1]` llega a -15 y a 10).
ValueRange ValueRange::multiplicar(const ValueRange &o) const {
    if (es_bottom() || o.es_bottom()) return bottom(t);
    if (!operables(*this, o)) return top(t);
    const Ancho al = desplegar(t, lo_c), ah = desplegar(t, hi_c);
    const Ancho bl = desplegar(t, o.lo_c), bh = desplegar(t, o.hi_c);
    Ancho p[4];
    if (!mul_ancha(al, bl, p[0]) || !mul_ancha(al, bh, p[1]) ||
        !mul_ancha(ah, bl, p[2]) || !mul_ancha(ah, bh, p[3]))
        return todo(t);
    Exacto e;
    e.ok = true;
    e.lo = *std::min_element(p, p + 4);
    e.hi = *std::max_element(p, p + 4);
    return plegar(t, e);
}

ValueRange ValueRange::negar() const {
    if (es_bottom()) return bottom(t);
    if (!acotada()) return top(t);
    Exacto e;
    e.ok = resta_ancha(0, desplegar(t, hi_c), e.lo) &&
           resta_ancha(0, desplegar(t, lo_c), e.hi);
    return plegar(t, e);
}

/// Un intervalo que contiene el cero.  Es lo que impide dividir.
static bool contiene_cero(const ValueRange &r) {
    const uint64_t z = r.t.desde_signo(0);
    return !r.t.menor(z, r.lo_c) && !r.t.menor(r.hi_c, z);
}

ValueRange ValueRange::dividir(const ValueRange &o) const {
    if (es_bottom() || o.es_bottom()) return bottom(t);
    if (!operables(*this, o)) return top(t);
    /* Un divisor que puede ser cero no permite afirmar nada.  Y ademas hay que
     * apartarlo aqui: calcular primero y mirar despues seria dividir por cero
     * de verdad, en el propio analizador. */
    if (contiene_cero(o)) return todo(t);
    /* Un intervalo que no contiene el cero es entero positivo o entero
     * negativo, asi que la division es monotona en cada argumento y las cuatro
     * esquinas bastan. */
    const Ancho al = desplegar(t, lo_c), ah = desplegar(t, hi_c);
    const Ancho bl = desplegar(t, o.lo_c), bh = desplegar(t, o.hi_c);
    Ancho p[4];
    if (!div_ancha(al, bl, p[0]) || !div_ancha(al, bh, p[1]) ||
        !div_ancha(ah, bl, p[2]) || !div_ancha(ah, bh, p[3]))
        return todo(t);
    Exacto e;
    e.ok = true;
    e.lo = *std::min_element(p, p + 4);
    e.hi = *std::max_element(p, p + 4);
    return plegar(t, e);
}

ValueRange ValueRange::resto(const ValueRange &o) const {
    if (es_bottom() || o.es_bottom()) return bottom(t);
    if (!operables(*this, o)) return top(t);
    if (contiene_cero(o)) return todo(t);
    /* El resto no puede llegar al valor absoluto del divisor, y su signo es el
     * del dividendo.  De ahi salen las dos cotas sin dividir nada. */
    const Ancho bl = desplegar(t, o.lo_c), bh = desplegar(t, o.hi_c);
    /* El valor absoluto del minimo con signo no cabe en su propio tipo: negarlo
     * ahi seria el desbordamiento de siempre.  Con enteros de 128 bits si cabe;
     * sin ellos, no se afirma. */
    Ancho abs_bl = 0, abs_bh = 0;
    if (!valor_absoluto(bl, abs_bl) || !valor_absoluto(bh, abs_bh))
        return todo(t);
    const Ancho tope = (abs_bl > abs_bh ? abs_bl : abs_bh) - 1;
    const Ancho al = desplegar(t, lo_c), ah = desplegar(t, hi_c);
    Exacto e;
    e.ok = true;
    e.lo = (al >= 0) ? 0 : (al > -tope ? al : -tope);
    e.hi = (ah <= 0) ? 0 : (ah < tope ? ah : tope);
    return plegar(t, e);
}

ValueRange ValueRange::conjuncion(const ValueRange &o) const {
    if (es_bottom() || o.es_bottom()) return bottom(t);
    if (!acotada() || !o.acotada() || t != o.t) return top(t);
    /* Una mascara constante no negativa acota el resultado por arriba, y eso
     * vale aunque del otro operando no se sepa nada: `x & 7` esta en [0,7]
     * venga `x` de donde venga. */
    auto mascara_cte = [&](const ValueRange &r, uint64_t &tope) {
        if (!r.es_constante()) return false;
        if (!t.sin_signo && r.lo() < 0) return false;
        tope = r.lo_c;
        return true;
    };
    uint64_t ma = 0, mb = 0;
    const bool ca = mascara_cte(*this, ma), cb = mascara_cte(o, mb);
    if (ca && cb) return constante(t, ma & mb);
    if (ca || cb) return corte(t, 0, ca ? ma : mb);
    /* Sin constantes: con los dos operandos no negativos, el `y` bit a bit no
     * pasa del menor de los dos.  Es poco, pero es cierto. */
    if (!t.sin_signo && (lo() < 0 || o.lo() < 0)) return top(t);
    return corte(t, 0, t.menor_de(hi_c, o.hi_c));
}

/// Tope de bits: el menor `2^n - 1` que llega a @p v.  Es la cota de cualquier
/// operacion bit a bit entre valores no negativos: no se pueden encender bits
/// mas altos que los que ya habia.
static uint64_t tope_por_bits(uint64_t v) {
    uint64_t m = 0;
    while (m < v) {
        if (m == UINT64_MAX) return UINT64_MAX;
        m = (m << 1) | 1;
    }
    return m;
}

/// Las dos operandas son no negativas en el orden de su tipo.
static bool ambas_no_negativas(const ValueRange &a, const ValueRange &b) {
    if (a.t.sin_signo) return true;
    return a.lo() >= 0 && b.lo() >= 0;
}

ValueRange ValueRange::disyuncion(const ValueRange &o) const {
    if (es_bottom() || o.es_bottom()) return bottom(t);
    if (!operables(*this, o)) return top(t);
    if (!ambas_no_negativas(*this, o)) return top(t);
    // Encender bits no baja de ninguno de los dos, y no puede pasar del tope.
    const uint64_t suelo = t.mayor_de(lo_c, o.lo_c);
    const uint64_t techo = tope_por_bits(t.mayor_de(hi_c, o.hi_c));
    // Aqui todo es natural, asi que la comparacion con el tope del tipo
    // tambien.
    if (techo > t.max_crudo()) return todo(t);
    return corte(t, suelo, techo);
}

ValueRange ValueRange::exclusiva(const ValueRange &o) const {
    if (es_bottom() || o.es_bottom()) return bottom(t);
    if (!operables(*this, o)) return top(t);
    if (!ambas_no_negativas(*this, o)) return top(t);
    // El `o exclusivo` tambien APAGA bits, asi que por abajo no se afirma nada.
    const uint64_t techo = tope_por_bits(t.mayor_de(hi_c, o.hi_c));
    if (techo > t.max_crudo()) return todo(t);
    return corte(t, 0, techo);
}

ValueRange ValueRange::complemento() const {
    if (es_bottom()) return bottom(t);
    if (!acotada()) return top(t);
    /* `~x` es una biyeccion del tipo en si mismo que invierte el orden (para un
     * `u8`, 255-x; para un `i8`, -x-1).  Por eso el resultado es exacto y basta
     * con cruzar los extremos. */
    return armar(t, t.normalizar(~hi_c), t.normalizar(~lo_c));
}

/// Un desplazamiento util: constante o no, pero siempre dentro de [0, bits).
/// Fuera de ahi el IR no define el resultado y no hay nada que afirmar.
static bool cuenta_valida(const ValueRange &k, uint8_t bits, uint32_t &lo,
                          uint32_t &hi) {
    if (!k.acotada()) return false;
    const int64_t l = k.t.sin_signo ? static_cast<int64_t>(k.lo_c) : k.lo();
    const int64_t h = k.t.sin_signo ? static_cast<int64_t>(k.hi_c) : k.hi();
    if (l < 0 || h >= static_cast<int64_t>(bits)) return false;
    lo = static_cast<uint32_t>(l);
    hi = static_cast<uint32_t>(h);
    return true;
}

ValueRange ValueRange::desplazar_izq(const ValueRange &o) const {
    if (es_bottom() || o.es_bottom()) return bottom(t);
    if (!acotada() || !o.acotada()) return top(t);
    uint32_t kl = 0, kh = 0;
    if (!cuenta_valida(o, t.bits, kl, kh)) return todo(t);
    /* `x << k` es `x * 2^k`: multiplicar por una potencia positiva es monotono
     * en `x`, y para un `x` fijo subir `k` aleja del cero conservando el signo.
     * Las cuatro esquinas bastan, y el producto detecta lo que no cabe. */
    const Ancho a[2] = {desplegar(t, lo_c), desplegar(t, hi_c)};
    const uint32_t k[2] = {kl, kh};
    Ancho p[4];
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j) {
            if (k[j] >= 63 && sizeof(Ancho) == 8) return todo(t);
            const Ancho factor = static_cast<Ancho>(1) << k[j];
            if (!mul_ancha(a[i], factor, p[i * 2 + j])) return todo(t);
        }
    Exacto e;
    e.ok = true;
    e.lo = *std::min_element(p, p + 4);
    e.hi = *std::max_element(p, p + 4);
    return plegar(t, e);
}

ValueRange ValueRange::desplazar_der_logico(const ValueRange &o) const {
    if (es_bottom() || o.es_bottom()) return bottom(t);
    if (!acotada() || !o.acotada()) return top(t);
    uint32_t kl = 0, kh = 0;
    if (!cuenta_valida(o, t.bits, kl, kh)) return todo(t);
    /* El desplazamiento LOGICO trata los bits como un natural, asi que se
     * razona en el dominio sin signo del mismo ancho y se vuelve al tipo al
     * final.  Ahi la operacion es monotona en los dos argumentos y los extremos
     * bastan. */
    const RangeType tu = RangeType::de(t.bits, true);
    const ValueRange u = reinterpretar(tu);
    if (!u.acotada()) return todo(t);
    return crudo(tu, u.lo_c >> kh, u.hi_c >> kl).reinterpretar(t);
}

ValueRange ValueRange::desplazar_der_aritmetico(const ValueRange &o) const {
    if (es_bottom() || o.es_bottom()) return bottom(t);
    if (!acotada() || !o.acotada()) return top(t);
    uint32_t kl = 0, kh = 0;
    if (!cuenta_valida(o, t.bits, kl, kh)) return todo(t);
    /* Conserva el signo: es una division por `2^k` que redondea HACIA ABAJO, no
     * hacia cero (`-1 >> 1` vale -1, no 0).  Monotona en `x`, y para un `x`
     * fijo subir `k` acerca a 0 o a -1 segun el signo: las esquinas bastan. */
    const Ancho a[2] = {desplegar(t, lo_c), desplegar(t, hi_c)};
    const uint32_t k[2] = {kl, kh};
    Ancho p[4];
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j) {
            if (k[j] >= 63 && sizeof(Ancho) == 8) return todo(t);
            const Ancho div = static_cast<Ancho>(1) << k[j];
            const Ancho v = a[i];
            // Redondeo hacia abajo, escrito sin depender de como divide el host
            // y sin negar el minimo, que es el desbordamiento de siempre.
            if (v >= 0) {
                p[i * 2 + j] = v / div;
            } else {
                Ancho av = 0;
                if (!valor_absoluto(v, av)) return todo(t);
                p[i * 2 + j] = -((av + div - 1) / div);
            }
        }
    Exacto e;
    e.ok = true;
    e.lo = *std::min_element(p, p + 4);
    e.hi = *std::max_element(p, p + 4);
    return plegar(t, e);
}

// ===========================================================================
//  Conversiones entre anchos
// ===========================================================================

ValueRange ValueRange::extender_sin_signo(RangeType destino) const {
    if (es_bottom()) return bottom(destino);
    if (!acotada()) return top(destino);
    /* Se lee el origen SIN signo.  Un `i8` que vale -1 pasa a valer 255: el
     * numero CAMBIA, y por eso esto no es lo mismo que extender con signo. */
    uint64_t ul = lo_c, uh = hi_c;
    if (!t.sin_signo && lo() < 0 && hi() >= 0) {
        // Cruza el cero: leido sin signo el conjunto se parte (los negativos
        // son los mas grandes).  Lo afirmable es todo el ancho del origen.
        ul = 0;
        uh = t.mascara();
    }
    Exacto e;
    e.ok = true;
    e.lo = static_cast<Ancho>(t.normalizar(ul));
    e.hi = static_cast<Ancho>(t.normalizar(uh));
    if (e.hi < e.lo) return todo(destino);
    return plegar(destino, e);
}

ValueRange ValueRange::extender_con_signo(RangeType destino) const {
    if (es_bottom()) return bottom(destino);
    if (!acotada()) return top(destino);
    // El numero no cambia: se despliega en el origen y se pliega en el destino.
    Exacto e;
    e.ok = true;
    e.lo = desplegar(t, lo_c);
    e.hi = desplegar(t, hi_c);
    return plegar(destino, e);
}

ValueRange ValueRange::truncar(RangeType destino) const {
    if (es_bottom()) return bottom(destino);
    if (!acotada()) return top(destino);
    /* Truncar es MODULAR: se quedan los bits bajos.  Si el conjunto tiene mas
     * valores de los que el destino distingue, cubre el destino entero; si no,
     * los extremos se normalizan y `crudo` decide si el resultado quedo partido
     * (y entonces responde el tipo, que es lo unico afirmable). */
    const uint64_t card_origen = cardinal();
    const uint64_t card_destino = destino.cardinal();
    if (card_origen == 0) return todo(destino);
    if (card_destino != 0 && card_origen > card_destino) return todo(destino);
    return crudo(destino, lo_c, hi_c);
}

ValueRange ValueRange::reinterpretar(RangeType destino) const {
    if (es_bottom()) return bottom(destino);
    if (!acotada()) return top(destino);
    if (t == destino) return *this;
    /* Los mismos bits, otra lectura.  Solo tiene sentido a igual ancho; y aun
     * asi la correspondencia NO es monotona -- `u32 [0x7FFFFFF0,0xFFFFFFFF]`
     * leido como `i32` se parte en dos --, que es justo lo que detecta `crudo`
     * al ver el intervalo invertido. */
    if (t.bits != destino.bits) return todo(destino);
    return crudo(destino, lo_c, hi_c);
}

// ===========================================================================
//  Restricciones (lo que afirma una comparacion)
// ===========================================================================

ValueRange ValueRange::restringir_menor(const ValueRange &o) const {
    if (es_bottom() || o.es_bottom()) return bottom(t);
    if (!o.acotada() || (acotada() && t != o.t)) return *this;
    const RangeType d = acotada() ? t : o.t;
    // Nada es menor que el minimo del tipo: la rama no se toma.
    if (o.hi_c == d.min_crudo()) return bottom(d);
    return cortar(corte(d, d.min_crudo(), d.normalizar(o.hi_c - 1)));
}

ValueRange ValueRange::restringir_menor_igual(const ValueRange &o) const {
    if (es_bottom() || o.es_bottom()) return bottom(t);
    if (!o.acotada() || (acotada() && t != o.t)) return *this;
    const RangeType d = acotada() ? t : o.t;
    return cortar(corte(d, d.min_crudo(), o.hi_c));
}

ValueRange ValueRange::restringir_mayor(const ValueRange &o) const {
    if (es_bottom() || o.es_bottom()) return bottom(t);
    if (!o.acotada() || (acotada() && t != o.t)) return *this;
    const RangeType d = acotada() ? t : o.t;
    if (o.lo_c == d.max_crudo()) return bottom(d);
    return cortar(corte(d, d.normalizar(o.lo_c + 1), d.max_crudo()));
}

ValueRange ValueRange::restringir_mayor_igual(const ValueRange &o) const {
    if (es_bottom() || o.es_bottom()) return bottom(t);
    if (!o.acotada() || (acotada() && t != o.t)) return *this;
    const RangeType d = acotada() ? t : o.t;
    return cortar(corte(d, o.lo_c, d.max_crudo()));
}

ValueRange ValueRange::restringir_igual(const ValueRange &o) const {
    return cortar(o);
}

ValueRange ValueRange::restringir_fuera(const ValueRange &o) const {
    if (es_bottom() || o.es_bottom()) return bottom(t);
    if (!acotada() || !o.acotada() || t != o.t) return *this;
    // Nos cubre entero: no queda ningun valor posible.
    if (!t.menor(lo_c, o.lo_c) && !t.menor(o.hi_c, hi_c)) return bottom(t);
    // Muerde por abajo: el intervalo empieza donde acaba el prohibido.
    if (!t.menor(lo_c, o.lo_c) && !t.menor(o.hi_c, lo_c) &&
        o.hi_c != t.max_crudo())
        return corte(t, t.normalizar(o.hi_c + 1), hi_c);
    // Muerde por arriba.
    if (!t.menor(hi_c, o.lo_c) && !t.menor(o.hi_c, hi_c) &&
        o.lo_c != t.min_crudo())
        return corte(t, lo_c, t.normalizar(o.lo_c - 1));
    /* Lo parte por en medio (o no lo toca): quitar un trozo interior dejaria
     * dos intervalos -- `[0,10]` sin el 5 son `[0,4]` y `[6,10]` -- y eso no se
     * representa.  Quedarse como estaba es correcto, solo menos preciso. */
    return *this;
}

ValueRange ValueRange::restringir_distinto(const ValueRange &o) const {
    if (es_bottom() || o.es_bottom()) return bottom(t);
    /* `x != y` con `y` en un rango no afirma NADA sobre x: que x pueda
     * coincidir con algun valor de ese rango no obliga a que coincida con el
     * que toque. */
    if (!o.es_constante()) return *this;
    return restringir_fuera(o);
}

} // namespace analysis
