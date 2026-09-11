/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file util/crono_tramo.h
 * @brief Cronometrar un TRAMO de codigo y sumarlo a una etiqueta.
 *
 * Saber que un pase es caro no sirve para arreglarlo; saber QUE PARTE de el lo
 * es, si.  Esto permite partir cualquier trabajo por dentro sin montar nada:
 * un objeto en el ambito del tramo, una etiqueta, y el tiempo se acumula donde
 * lo recoge quien informa (@c ir::tiempos_de_pases).
 *
 * Vive aqui, y no dentro del optimizador donde nacio, porque el trabajo caro no
 * siempre esta en un pase: el ultimo que hubo que partir estaba en el MODELO DE
 * EFECTOS, en otra libreria.  Es el mismo camino que hicieron el hash, el
 * lector/escritor de bytes y la escritura atomica.
 *
 * CUIDADO, y ya costo una medicion falsa: mide hasta que termina SU ambito.
 * Declarado suelto al principio de una funcion mide la funcion entera, no el
 * tramo que sigue -- dio una "parte" mayor que el todo, que es la senal de que
 * mide otra cosa.  Envolver el tramo en su propio bloque.
 */

#ifndef VESTA_UTIL_CRONO_TRAMO_H
#define VESTA_UTIL_CRONO_TRAMO_H

#include <chrono>
#include <cstdint>

#include "util/reloj.h"
#include <vector>

namespace util {

/**
 * @brief Suma @p us a la cuenta de @p etiqueta.
 *
 * @param etiqueta Literal estable; se agrupa por el.
 * @param us       Microsegundos a sumar.
 */
/**
 * @brief Suma @p ns a la cuenta de @p etiqueta.
 *
 * En NANOSEGUNDOS, y no es un detalle: truncando a microsegundos, un tramo de
 * 0,4 us mide CERO, y doscientas mil llamadas de esas suman cero habiendo
 * costado 80 ms.  Un tramo corto y muy repetido es justo el que se quiere
 * descubrir, y era justo el que desaparecia.
 */
void acumular_tramo_ns(const char *etiqueta, long long ns);

/// Compatibilidad: suma microsegundos (los convierte).
inline void acumular_tramo(const char *etiqueta, long long us) {
    acumular_tramo_ns(etiqueta, us * 1000);
}

/**
 * @brief Un tramo medido: cuanto se llevo, cuantas veces, y DENTRO DE QUE.
 *
 * El padre no es un adorno.  Sin el, el informe es una lista PLANA de numeros
 * que se solapan: `emitir` 2386 ms, `report_bounds` 1810 y `effects` 190, y
 * quien lo lee no puede saber si eso suma 4386 o 2386 -- ni, por tanto, donde
 * esta el cuello de botella.  Ya costo tres atribuciones equivocadas en un solo
 * dia: `emitir` no emitia, `bajada` no bajaba, y el analisis de escape parecia
 * del emisor.
 *
 * Con el padre, el mismo dato se lee como lo que es -- 190 DENTRO de 1810
 * DENTRO de 2386 -- y ademas aparece lo que ningun hijo explica, que es donde
 * suele esconderse lo que nadie ha medido todavia.
 */
struct Span {
    const char *name = "?";
    /// El tramo que estaba abierto al entrar en este, o nulo si ninguno.
    const char *parent = nullptr;
    long long us = 0; ///< microsegundos (se acumula en ns y se divide al leer).
    long long runs = 0;
    /**
     * @brief En cuantos HILOS se midio este tramo.
     *
     * Uno significa que @ref us es tiempo de PARED.  Mas de uno significa que
     * es la SUMA de lo que midio cada hilo, y entonces no se puede dividir por
     * el total de una fase -- que si es de pared -- sin obtener un disparate.
     *
     * No es un adorno del informe: sin este numero, un pase repartido entre
     * ocho hilos se lee como si costara ocho veces mas de lo que tarda, y eso
     * ya hizo perseguir un cuello de botella que no existia (el eliminador de
     * codigo muerto parecia el 85 % del optimizador; medido en un solo hilo es
     * el 10 %).
     */
    long long threads = 0;
};

/**
 * @brief Abre un tramo: lo apila y dice quien lo contiene.
 * @param label El que se abre.
 * @return El que estaba abierto, o nulo.
 */
const char *enter_span(const char *label);

/**
 * @brief Cierra el tramo abierto y le suma lo que costo.
 * @param label  El que se cierra.
 * @param parent El que devolvio @ref enter_span .
 * @param ns     Lo que tardo.
 */
void leave_span(const char *label, const char *parent, long long ns);

/// El tramo abierto ahora mismo en este hilo, o nulo.  Lo usa quien lleva su
/// propia contabilidad y quiere colgarla del arbol igualmente.
const char *current_span();

/// Lo que cuesta medir y lo fino que es el reloj, en nanosegundos.  Se
/// descuenta el coste al informar; la resolucion se ensena para que nadie se
/// crea una cifra mas fina que el reloj que la tomo.
struct Calibracion_ {
    long long coste_ns = 0;
    double resolucion_ns = 0.0; ///< en coma flotante: puede ser < 1 ns.
    const char *fuente = "?";   ///< que reloj se esta usando.
};
Calibracion_ calibracion_del_cronometro();

/// Los tramos medidos, del mas caro al mas barato.  Ya descontado el coste de
/// medir (que es proporcional a las tomas y siempre hacia arriba).
std::vector<Span> measured_spans();

/// Pone el acumulador a cero.
void reiniciar_tramos();

/**
 * @brief Cronometra lo que viva el objeto y lo suma a su etiqueta.
 *
 * @p medir NO tiene valor por defecto, y es a proposito: quien mide no debe
 * cobrarle a quien no mide, y esta es una utilidad -- no sabe ni tiene por que
 * saber bajo que bandera vive cada uno de sus usuarios.  La decision es de
 * quien la usa, y escribirla obliga a tomarla.
 *
 * Apagado no toca el reloj ni el acumulador: son dos lecturas del reloj y una
 * suma en una tabla indexada por CADENA, y eso corria SIEMPRE.  Medido con
 * VTune, el cronometraje se llevaba ~3,5 % de compilar -- un tercio de todo lo
 * que el compilador hacia con tablas hash -- para responder una pregunta que
 * casi nadie hace.
 */
struct CronoTramo {
    const char *n;
    uint64_t t0; ///< en ticks del reloj elegido, no en tiempo.
    bool on;     ///< se decidio al construir; no se vuelve a preguntar.

    /**
     * @param etiqueta Bajo que nombre se suma.
     * @param medir    Si hay que medir; falso no cuesta nada.
     */
    const char *parent; ///< el tramo que lo contiene, para poder anidarlo.

    CronoTramo(const char *etiqueta, bool medir)
        : n(etiqueta), t0(0), on(medir), parent(nullptr) {
        if (!on) return;
        parent = enter_span(etiqueta);
        t0 = reloj::ahora();
    }
    ~CronoTramo() {
        if (!on) return;
        /* Se convierte a tiempo AQUI y no al informar porque el acumulador es
         * uno solo y mezclar unidades seria peor que una multiplicacion. */
        leave_span(n, parent, reloj::a_ns(reloj::ahora() - t0));
    }
    CronoTramo(const CronoTramo &) = delete;
    CronoTramo &operator=(const CronoTramo &) = delete;
};

} // namespace util

#endif // VESTA_UTIL_CRONO_TRAMO_H
