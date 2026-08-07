/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/facts/value_range.h
 * @brief Entre que dos numeros esta un valor.  Hecho fundacional, sin dueno.
 *
 * Es la pieza que hace decidible lo que si no habria que callar: una region de
 * tamano SIMBOLICO (`malloc(n)`) no se puede comprobar sin saber cuanto puede
 * valer `n`, y un acceso indexado (`buf[i]`) tampoco sin saber cuanto vale `i`.
 *
 * Reticulo de intervalos con TRES estados, no dos:
 *
 *     TOP      no se nada del valor
 *     [lo,hi]  esta entre esos dos
 *     BOTTOM   este punto del programa NO SE ALCANZA
 *
 * BOTTOM no es "no se": es "aqui no se llega".  Confundirlos hace que una rama
 * imposible -- `x = 20; if (x < 10)` -- se trate como una rama de la que no se
 * sabe nada, y peor: permite declarar inalcanzable un punto vivo, que habilita a
 * cualquier consumidor a concluir lo que quiera sobre codigo que si se ejecuta.
 *
 * TOP y `todo(T)` NO son lo mismo, y la diferencia importa:
 *
 *     TOP        ni siquiera se sabe en que dominio se esta hablando
 *     todo(u8)   se sabe que es un `u8`; el valor, no.  Y eso ya acota: [0,255]
 *
 * CADA INTERVALO CONOCE SU TIPO -- ancho en bits e interpretacion --, y esa es
 * la diferencia entre un prototipo y algo sobre lo que se puede demostrar:
 *
 *   - Un `u64` puede valer mas que el mayor `int64_t`.  Representarlo con
 *     enteros con signo obliga a callarse en la mitad del dominio.
 *   - La aritmetica del IR ENVUELVE al ancho: un `u8` con `250 + 10` vale 4, no
 *     260.  Un dominio que calcula 260 no puede hacer nada con ese numero salvo
 *     tirarlo; uno que sabe el ancho responde 4, que es la verdad.
 *
 * POR ESO TODA LA SEMANTICA VIVE AQUI y no en el motor de flujo: envoltura,
 * comparaciones con y sin signo, extensiones y truncados son propiedades del
 * TIPO, no del recorrido del grafo.  El motor decide QUE se compone; el dominio,
 * COMO.  Un dominio que se prueba solo -- sin construir una funcion IR -- es un
 * dominio del que se puede uno fiar.
 *
 * SEPARADO de "que bits estan a uno" a proposito: aqui se habla del VALOR, no
 * de la representacion fisica.
 *
 * INDEPENDIENTE DE ARQUITECTURA: se razona sobre el IR y los anchos de sus
 * tipos.  Ni registros, ni acarreos, ni convenios de llamada.
 */
#ifndef ANALYSIS_FACTS_VALUE_RANGE_H
#define ANALYSIS_FACTS_VALUE_RANGE_H

#include "analysis/facts/ir_facts.h"

#include <cstdint>
#include <vector>

namespace ir {
struct IrFunction;
}

namespace analysis {

/**
 * @brief Tipo numerico sobre el que se razona: cuantos bits y como se leen.
 *
 * Es el UNICO sitio donde se convierte entre "los bits" y "el numero".  Tenerlo
 * repartido es como acaban los analisis afirmando que un `u64` grande es
 * negativo.
 *
 * Se construye SIEMPRE por las factorias, que validan el ancho.  Un ancho fuera
 * de [1,64] no existe en el IR; si llegara, se ensancha a 64 bits, que es una
 * sobre-aproximacion (se afirma menos), nunca una mentira.
 */
struct RangeType {
    uint8_t bits = 64;
    bool    sin_signo = false;

    /// Construccion validada.  Es la unica puerta de entrada.
    static RangeType de(uint8_t bits, bool sin_signo) {
        RangeType t;
        t.bits = (bits >= 1 && bits <= 64) ? bits : 64;
        t.sin_signo = sin_signo;
        return t;
    }
    /// Atajos que se leen como los tipos del lenguaje: `i(32)`, `u(8)`.
    static RangeType i(uint8_t bits) { return de(bits, false); }
    static RangeType u(uint8_t bits) { return de(bits, true); }

    bool valido() const { return bits >= 1 && bits <= 64; }
    bool operator==(const RangeType &o) const {
        return bits == o.bits && sin_signo == o.sin_signo;
    }
    bool operator!=(const RangeType &o) const { return !(*this == o); }

    /// Mascara del ancho: los bits que el tipo realmente tiene.
    uint64_t mascara() const {
        return bits >= 64 ? UINT64_MAX : ((uint64_t(1) << bits) - 1);
    }
    /// Cuantos valores distintos hay.  0 significa "2^64", que no cabe.
    uint64_t cardinal() const { return bits >= 64 ? 0 : (uint64_t(1) << bits); }

    /// Mayor y menor valor representables, EN CRUDO (los bits, no el numero).
    uint64_t max_crudo() const {
        if (bits >= 64)
            return sin_signo ? UINT64_MAX : static_cast<uint64_t>(INT64_MAX);
        const uint64_t m = mascara();
        return sin_signo ? m : (m >> 1);
    }
    uint64_t min_crudo() const {
        if (sin_signo) return 0;
        if (bits >= 64) return static_cast<uint64_t>(INT64_MIN);
        return (uint64_t(1) << (bits - 1)) & mascara();
    }

    /// Deja el valor dentro del ancho (lo que hace el hardware al envolver).
    uint64_t normalizar(uint64_t v) const { return v & mascara(); }

    /// Los bits leidos como NUMERO, segun la interpretacion del tipo.
    int64_t hacia_signo(uint64_t v) const {
        v = normalizar(v);
        if (sin_signo || bits >= 64) return static_cast<int64_t>(v);
        const uint64_t bit = uint64_t(1) << (bits - 1);
        return static_cast<int64_t>((v ^ bit) - bit); // extension de signo
    }
    /// Un numero guardado como bits del tipo (envolviendo si no cabe).
    uint64_t desde_signo(int64_t v) const {
        return normalizar(static_cast<uint64_t>(v));
    }

    /// Orden del tipo: un `u8` compara 200 < 250; un `i8`, -56 < 0.
    bool menor(uint64_t a, uint64_t b) const {
        if (sin_signo) return normalizar(a) < normalizar(b);
        return hacia_signo(a) < hacia_signo(b);
    }
    uint64_t menor_de(uint64_t a, uint64_t b) const { return menor(a, b) ? a : b; }
    uint64_t mayor_de(uint64_t a, uint64_t b) const { return menor(a, b) ? b : a; }
};

enum class RangeKind : uint8_t { Bottom, Bounded, Top };

/**
 * @brief Intervalo cerrado dentro de un tipo, o TOP, o BOTTOM.
 *
 * Los extremos se guardan EN CRUDO (los bits) y se leen segun el tipo.  Asi un
 * `u64` por encima del mayor `int64_t` se representa igual de bien que un `i8`
 * negativo, sin que el analisis tenga que rendirse en medio dominio.
 *
 * INVARIANTE (comprobable con @c valida): si esta acotado, el tipo es valido,
 * los dos extremos pertenecen al tipo y `lo <= hi` EN EL ORDEN DEL TIPO.  Las
 * factorias son las que lo garantizan; construir el struct a mano lo rompe.
 *
 * NO REPRESENTA intervalos CIRCULARES.  Cuando el resultado exacto de una
 * operacion da la vuelta al tipo -- `u8 [250,4]` -- no hay intervalo simple que
 * lo diga, y la respuesta es `todo(u8)`: menos preciso, nunca falso.
 */
struct ValueRange {
    RangeKind kind = RangeKind::Top;
    RangeType t{};
    uint64_t  lo_c = 0; ///< extremo inferior, en crudo
    uint64_t  hi_c = 0; ///< extremo superior, en crudo

    // ----------------------------------------------------------------- crear
    static ValueRange top(RangeType ty = RangeType{}) {
        ValueRange r;
        r.kind = RangeKind::Top;
        r.t = ty;
        return r;
    }
    static ValueRange bottom(RangeType ty = RangeType{}) {
        ValueRange r;
        r.kind = RangeKind::Bottom;
        r.t = ty;
        return r;
    }

    /**
     * @brief Intervalo a partir de VALORES DEL TIPO (se normalizan al ancho).
     *
     * Si tras normalizar el intervalo queda invertido, el conjunto DA LA VUELTA
     * al tipo y no hay intervalo simple que lo represente: se responde
     * `todo(ty)`.  NUNCA BOTTOM -- un conjunto no vacio no puede convertirse en
     * "aqui no se llega" por una limitacion de la representacion.
     */
    static ValueRange crudo(RangeType ty, uint64_t lo, uint64_t hi) {
        lo = ty.normalizar(lo);
        hi = ty.normalizar(hi);
        if (ty.menor(hi, lo)) return todo(ty);
        return armar(ty, lo, hi);
    }

    /**
     * @brief Intervalo procedente de un CORTE: si queda vacio, es BOTTOM.
     *
     * Es la otra mitad de @c crudo, y la distincion es deliberada.  Aqui el
     * intervalo invertido significa "ninguna de las dos afirmaciones puede
     * cumplirse a la vez", que es exactamente inalcanzable.
     */
    static ValueRange corte(RangeType ty, uint64_t lo, uint64_t hi) {
        lo = ty.normalizar(lo);
        hi = ty.normalizar(hi);
        if (ty.menor(hi, lo)) return bottom(ty);
        return armar(ty, lo, hi);
    }

    /// Intervalo desde numeros con signo (el caso comun al escribir codigo).
    static ValueRange de_enteros(RangeType ty, int64_t lo, int64_t hi) {
        return crudo(ty, ty.desde_signo(lo), ty.desde_signo(hi));
    }
    static ValueRange constante(RangeType ty, uint64_t v) {
        return armar(ty, ty.normalizar(v), ty.normalizar(v));
    }
    /// Todo el tipo: se sabe el dominio, no el valor.  Distinto de TOP.
    static ValueRange todo(RangeType ty) {
        return armar(ty, ty.min_crudo(), ty.max_crudo());
    }

    // -------------------------------------------------------------- consultar
    bool es_top() const { return kind == RangeKind::Top; }
    bool es_bottom() const { return kind == RangeKind::Bottom; }
    bool acotada() const { return kind == RangeKind::Bounded; }
    bool es_constante() const { return acotada() && lo_c == hi_c; }
    /// Si cubre el tipo entero: acotado, pero sin informacion util.
    bool es_todo() const {
        return acotada() && lo_c == t.min_crudo() && hi_c == t.max_crudo();
    }

    /// El invariante de la struct.  Se comprueba en las pruebas y en depuracion.
    bool valida() const {
        if (kind != RangeKind::Bounded) return true;
        return t.valido() && !t.menor(hi_c, lo_c) &&
               t.normalizar(lo_c) == lo_c && t.normalizar(hi_c) == hi_c;
    }

    /// Los extremos leidos como NUMEROS del tipo.  Sin sentido si no acotada.
    int64_t lo() const { return t.hacia_signo(lo_c); }
    int64_t hi() const { return t.hacia_signo(hi_c); }

    /// Cuantos valores contiene (0 = "no cabe en `uint64_t`", solo `u64` entero).
    uint64_t cardinal() const {
        if (!acotada()) return 0;
        const uint64_t d = t.normalizar(hi_c - lo_c);
        return (d == UINT64_MAX) ? 0 : d + 1;
    }

    /**
     * @brief Los dos extremos leidos como `int64_t`, si ambos caben ahi.
     *
     * Es una consulta de REPRESENTACION, no una conversion semantica: dice si
     * este intervalo puede entregarse a un consumidor que trabaja con enteros
     * con signo de 64 bits (un desplazamiento, un tamano).  Un `u64` por encima
     * del mayor `int64_t` no cabe, y decirlo es mejor que mentir.
     */
    bool vista_con_signo(int64_t &lo_out, int64_t &hi_out) const {
        if (!acotada()) return false;
        if (t.sin_signo &&
            (lo_c > uint64_t(INT64_MAX) || hi_c > uint64_t(INT64_MAX)))
            return false;
        lo_out = lo();
        hi_out = hi();
        return true;
    }

    bool operator==(const ValueRange &o) const {
        if (kind != o.kind || t != o.t) return false;
        return kind != RangeKind::Bounded || (lo_c == o.lo_c && hi_c == o.hi_c);
    }
    bool operator!=(const ValueRange &o) const { return !(*this == o); }

    // ---------------------------------------------------------------- reticulo
    /// UNION: lo que puede valer si viene por cualquiera de dos caminos.
    /// BOTTOM es el neutro (ese camino no aporta valores); TOP absorbe.
    ValueRange unir(const ValueRange &o) const;

    /**
     * @brief CORTE: lo que cumple las dos afirmaciones sobre el MISMO punto.
     *
     * Si no queda nada, el punto no se alcanza -> BOTTOM (no "no se").
     *
     * Con tipos DISTINTOS no se corta: mezclar dominios es un fallo del IR o del
     * llamante, y la respuesta segura es quedarse con lo que ya se sabia.
     * Devolver BOTTOM ahi convertiria un fallo NUESTRO en "este codigo no se
     * ejecuta", que es la peor conclusion posible.  Quien quiera cruzar dominios
     * reinterpreta primero (@c reinterpretar), que si esta definido.
     */
    ValueRange cortar(const ValueRange &o) const;

    /**
     * @brief ENSANCHAMIENTO: el extremo que crece se suelta hasta el del tipo.
     *
     * Es lo que hace que el analisis TERMINE.  Se suelta SOLO el extremo que se
     * movio: soltar el intervalo entero tira tambien lo que no habia cambiado, y
     * en `for (i = 100; i < 200)` lo que no cambia es la cota inferior, que es
     * justo lo que permite demostrar algo.  Ensanchar no es olvidar.
     */
    ValueRange ensanchar(const ValueRange &nuevo) const;

    // -------------------------------------------------- aritmetica del dominio
    //
    //  Todas ENVUELVEN como el IR.  El conjunto exacto se calcula en enteros sin
    //  limite y luego se pliega al tipo: si cabe, el resultado es exacto (un
    //  `u8` con 250+10 responde 4); si al plegarlo da la vuelta al tipo, la
    //  respuesta es `todo(T)`.
    ValueRange sumar(const ValueRange &o) const;
    ValueRange restar(const ValueRange &o) const;
    ValueRange multiplicar(const ValueRange &o) const;
    ValueRange negar() const;
    /**
     * @brief Division entera.
     *
     * Con un divisor que PUEDE valer cero no se afirma nada.  Descartar el cero
     * "porque dividir por cero corta la ejecucion" seria apoyarse en una
     * propiedad del backend -- que la operacion atrape SIEMPRE, en interprete,
     * JIT y nativo --, y el dominio no razona sobre backends.
     */
    ValueRange dividir(const ValueRange &o) const;
    /// Resto: su valor absoluto es menor que el del divisor y su signo lo pone
    /// el dividendo.  Mismo trato del divisor cero que @c dividir.
    ValueRange resto(const ValueRange &o) const;

    // --- bit a bit -----------------------------------------------------------
    /// `x & c` con `c` constante no negativa no pasa de `c`, venga x de donde
    /// venga.  Es lo unico afirmable sin mirar los bits del otro lado.
    ValueRange conjuncion(const ValueRange &o) const;
    /// `x | y` solo ENCIENDE bits: no baja de ninguno de los dos, y no puede
    /// pasar del tope de bits del mayor.  Requiere los dos no negativos.
    ValueRange disyuncion(const ValueRange &o) const;
    /// `x ^ y` puede apagar bits, asi que solo se acota por arriba.
    ValueRange exclusiva(const ValueRange &o) const;
    /// `~x` es una biyeccion que INVIERTE el orden: exacta siempre.
    ValueRange complemento() const;

    // --- desplazamientos -----------------------------------------------------
    /// `x << k` es `x * 2^k` dentro del tipo.  Un `k` fuera de [0, bits) no
    /// tiene significado definido y no se afirma nada.
    ValueRange desplazar_izq(const ValueRange &o) const;
    /// `x >> k` LOGICO: se razona en el dominio sin signo del mismo ancho.
    ValueRange desplazar_der_logico(const ValueRange &o) const;
    /// `x >> k` ARITMETICO: conserva el signo; es division con redondeo hacia
    /// abajo, no hacia cero, y por eso no es lo mismo que `dividir` por 2^k.
    ValueRange desplazar_der_aritmetico(const ValueRange &o) const;

    // ------------------------------------------------- conversiones del IR
    //
    //  CUATRO operaciones distintas, no una: `u8 -> u32`, `i8 -> i32`,
    //  `u32 -> u8` y "los mismos bits, otra lectura" no significan lo mismo.
    /// Extension SIN signo: el origen se lee como natural (un `i8` que vale -1
    /// pasa a valer 255).
    ValueRange extender_sin_signo(RangeType destino) const;
    /// Extension CON signo: el numero no cambia.
    ValueRange extender_con_signo(RangeType destino) const;
    /// Truncado: es MODULAR.  `u16 [250,260]` en `u8` no es `[250,255]`; el
    /// conjunto exacto da la vuelta, asi que lo afirmable es `[0,255]`.
    ValueRange truncar(RangeType destino) const;
    /// Misma representacion, otra lectura.  La correspondencia no es monotona
    /// (`u32 [0x7FFFFFF0,0xFFFFFFFF]` en `i32` se parte en dos), y cuando se
    /// rompe el orden lo afirmable es el tipo entero.
    ValueRange reinterpretar(RangeType destino) const;

    // --------------------------------------------------------- restricciones
    //
    //  Lo que AFIRMA una comparacion sobre este valor.  Viven aqui, y no en el
    //  motor, porque un `<` no significa lo mismo en `i8` que en `u64`, y esa
    //  diferencia es del tipo.  Se apoyan en @c corte: si lo afirmado contradice
    //  lo que ya se sabia, el resultado es BOTTOM -- por ahi no se pasa.
    ValueRange restringir_menor(const ValueRange &o) const;
    ValueRange restringir_menor_igual(const ValueRange &o) const;
    ValueRange restringir_mayor(const ValueRange &o) const;
    ValueRange restringir_mayor_igual(const ValueRange &o) const;
    ValueRange restringir_igual(const ValueRange &o) const;
    /**
     * @brief El valor NO esta en @p o.  Resta de intervalos.
     *
     * Si @p o se come este intervalo entero, no queda nada: BOTTOM.  Si lo muerde
     * por un extremo, se recorta.  Si lo parte por en medio quedarian dos trozos
     * y eso no se representa, asi que se deja como estaba -- correcto, solo menos
     * preciso.
     *
     * Sirve para dos cosas distintas que son la misma: `x != k` y la rama por
     * DEFECTO de un `switch`, que afirma que el selector cae fuera de la tabla.
     */
    ValueRange restringir_fuera(const ValueRange &o) const;
    /// `x != k`.  Solo afirma algo con un @p o de un unico valor: `x != y` con
    /// `y` en un rango no dice nada de `x`.
    ValueRange restringir_distinto(const ValueRange &o) const;

  private:
    /// Constructor interno: los extremos YA cumplen el invariante.
    static ValueRange armar(RangeType ty, uint64_t lo, uint64_t hi) {
        ValueRange r;
        r.kind = RangeKind::Bounded;
        r.t = ty;
        r.lo_c = lo;
        r.hi_c = hi;
        return r;
    }
};

/* PENDIENTE, y dicho aqui para que no se pierda: un rango simple no basta para
 * `base + i*8`.  Hace falta el PASO ademas del intervalo -- `i en [0,63]` paso 8
 * -> `offset en [0,504]` -- para demostrar `offset + sizeof(T) <= tamano` sin
 * conocer `i`, y ademas la CONGRUENCIA (`offset % 8 == 0`) para hablar de
 * alineacion.  Es lo que hara comprobables las vistas dinamicas (`@overlay`),
 * cuya geometria se compone de sumas de cotas.
 *
 * Ese paso NO entra aqui dentro: sale un hecho HERMANO -- congruencias -- y el
 * consumidor pregunta a los dos.  Un rango que ademas conoce el paso, los bits,
 * la region y la alineacion deja de ser un dominio demostrable para convertirse
 * en el saco de todo lo que se sabe de un valor. */

/// Ajustes del analisis.  Los presupuestos son una RED ante un fallo del propio
/// motor, no el mecanismo de terminacion (de eso se encarga el ensanchamiento).
struct RangeOptions {
    /// Vueltas por una arista de retroceso antes de ensanchar.  Con 0 se
    /// ensancha a la primera (termina antes, afirma menos).
    uint32_t retardo_ensanche = 3;
    uint32_t pasos_por_bloque = 64;
    uint32_t pasos_extra = 256;
    /// Vueltas del descenso.  El descenso solo estrecha, asi que pararlo antes
    /// cuesta precision, nunca correccion.
    uint32_t pasos_descenso = 8;
};

/**
 * @brief Que hizo el motor para llegar al resultado.
 *
 * Sirve para depurar el COMPILADOR, no el programa: un caso que no converge o
 * que ensancha mil veces es un fallo del analisis, y sin estas cuentas se
 * confunde con "ese programa es dificil".
 */
struct RangeStats {
    uint32_t pasos = 0;       ///< bloques procesados en total (ascenso+descenso)
    uint32_t cambios = 0;     ///< veces que un IN[B] cambio
    uint32_t ensanches = 0;   ///< veces que se aplico ensanchamiento
    uint32_t estrechados = 0; ///< veces que el descenso mejoro un IN[B]
    /// Si el descenso llego hasta el final.  Pararlo a medias NO invalida el
    /// resultado -- toda la cadena descendente sigue conteniendo al punto fijo
    /// real --, solo lo deja menos preciso; por eso se cuenta aparte de
    /// @c RangeFacts::convergio, que si es una condicion de correccion.
    bool descenso_completo = true;
};

/// Rango de cada valor SSA, indexado por value-id.
struct RangeFacts {
    std::vector<ValueRange> r;
    /**
     * @brief Si el calculo llego a PUNTO FIJO o se paro por presupuesto.
     *
     * El tope no significa "analisis terminado": significa "hasta aqui he
     * llegado".  Quien va a DEMOSTRAR algo tiene derecho a distinguir una
     * conclusion de una parada, asi que sin convergencia no se afirma nada.
     */
    bool       convergio = true;
    RangeStats stats;

    const ValueRange &at(ir::IrValueId v) const {
        static const ValueRange kTop = ValueRange::top();
        return v < r.size() ? r[v] : kTop;
    }
};

/**
 * @brief Calcula los rangos de @p fn, con sensibilidad al FLUJO.
 *
 * Contrato del motor -- tres estados, no uno:
 *
 *     IN[B]              = union de OUT_ARISTA[P -> B]
 *     OUT[B]             = transferencia(B, IN[B])
 *     OUT_ARISTA[P -> S] = cortar(OUT[P], lo que afirma la guarda de esa arista)
 *
 * Las PHI se resuelven leyendo el estado DE LA ARISTA por la que llega cada
 * argumento: sin eso, una guarda no puede afectar a la PHI que depende de ella.
 *
 * DOS FASES con papeles distintos, no el mismo bucle con una bandera:
 *
 *     ASCENSO   crece hasta un post-punto-fijo; ensancha en las aristas de
 *               retroceso, que es lo unico que garantiza terminar.
 *     DESCENSO  parte de esa solucion y solo ESTRECHA; recupera lo que el
 *               ensanchamiento solto.  Pararlo antes cuesta precision, nunca
 *               correccion.
 *
 * El resultado por valor es su rango EN SU PUNTO DE DEFINICION -- una proyeccion
 * derivada, no el transporte del analisis.  Vale en cualquier uso porque en SSA
 * la definicion domina a todos ellos, pero NO incorpora los refinamientos
 * posteriores: en `if (i < 10) usar(i)`, la definicion puede decir `[0,1000]`
 * mientras el uso esta en `[0,9]`.  Quien necesite esa precision pregunta por el
 * punto, no por el valor.
 */
RangeFacts compute_ranges(const ir::IrFunction &fn, const IrFacts &facts,
                          const RangeOptions &op = RangeOptions{});

/// Marcador para el AnalysisManager (cachea por funcion; depende de IRFacts).
struct RangeAnalysis {
    using Result = RangeFacts;
    static char ID;
};

} // namespace analysis

#endif // ANALYSIS_FACTS_VALUE_RANGE_H
