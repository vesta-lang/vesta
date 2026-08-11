/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/asa/fact.h
 * @brief NUCLEO de ASA: que es un hecho, de donde viene y cuanto se puede uno
 *        fiar de el.  Vocabulario UNICO para todos los dominios.
 *
 * ASA no es un analisis: es la base de conocimiento del compilador.  Un dominio
 * (rangos, regiones, efectos, agregados, prestamos, bucles) DESCUBRE hechos; los
 * consumidores (comprobaciones de seguridad, optimizador, diagnosticos,
 * herramientas) deciden que hacer con ellos.  Este fichero define lo unico que
 * todos comparten, y existe por una razon concreta:
 *
 *   SI CADA DOMINIO INVENTA SU PROPIA PALABRA PARA "SEGURO", "DE DONDE SALE" Y
 *   "HECHO", CRUZARLOS DEJA DE SER POSIBLE.  No se puede combinar lo que no se
 *   mide igual, y lo que hace util una base de conocimiento es justo cruzarla.
 *
 * Tres piezas, y ninguna es decorativa:
 *
 *   CERTEZA      cuanto te puedes fiar.  Va DENTRO del hecho, no en el
 *                consumidor: si cada consumidor decide por su cuenta si algo es
 *                fiable, el mismo hecho justifica cosas contradictorias.
 *   PROCEDENCIA  quien lo descubrio y mirando que.  Sin esto no se puede
 *                explicar un veredicto ni depurar un analisis que miente.
 *   DEPENDENCIAS de que otros hechos se dedujo.  Un hecho derivado no puede ser
 *                mas fuerte que el peor de los que lo sostienen, y sin
 *                declararlas no hay forma de invalidarlo cuando uno cambia.
 */
#ifndef ANALYSIS_ASA_FACT_H
#define ANALYSIS_ASA_FACT_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace analysis {
namespace asa {

/**
 * @brief Confianza de un hecho.  El orden importa: a mayor valor, mas fuerte.
 *
 * La frontera que decide es la de arriba:
 *
 *   Desconocida   no hay evidencia suficiente.  NO es "falso": es que no se ha
 *                 mirado, o lo mirado no dice nada.  Distinguirlo de "falso" es
 *                 lo que impide que un analisis ciego parezca uno concluyente.
 *   Inferida      la evidencia apunta ahi, pero pudo quedar algo sin ver.
 *                 Sirve para OPTIMIZAR CON RED: especular con un guard, elegir
 *                 una version rapida con camino de vuelta.
 *   Demostrada    se ha visto TODO lo que podria contradecirlo.  Es lo unico
 *                 sobre lo que se puede rechazar un programa o quitar una
 *                 comprobacion sin dejar red.
 *
 * NO confundir con el VEREDICTO sobre una propiedad ("esto es seguro / no lo
 * es"): son ejes distintos.  Un hecho Demostrado puede afirmar que algo NO es
 * seguro.  Aqui se mide cuanto te fias de la afirmacion, no lo que dice.
 */
enum class Certeza : uint8_t {
    Desconocida = 0,
    Inferida = 1,
    Demostrada = 2,
};

/// La confianza de algo deducido de varias cosas es la MAS DEBIL de todas: una
/// cadena no es mas fuerte que su eslabon peor.
inline Certeza combinar(Certeza a, Certeza b) { return a < b ? a : b; }

/// Nombre estable para volcados y depuracion.  NO es texto de usuario: los
/// mensajes salen del catalogo i18n, nunca de aqui.
inline const char *nombre_certeza(Certeza c) {
    switch (c) {
    case Certeza::Demostrada: return "demostrada";
    case Certeza::Inferida: return "inferida";
    default: return "desconocida";
    }
}

/**
 * @brief De DONDE sale un hecho.  Ojo: no es su certeza.
 *
 * La certeza es lo que decide al consumidor -- usar directamente, usar con
 * guarda, o no suponer nada -- y va aparte a proposito.  Esto dice quien lo vio,
 * que sirve para explicarlo y depurarlo, no para decidir.  Justo por eso un
 * consumidor puede pasar de mirar la fuente: dos hechos de origen distinto con
 * la misma certeza se tratan igual.
 */
enum class Fuente : uint8_t {
    Estatico,  ///< leyendo el programa, sin ejecutarlo.
    Ejecucion, ///< observado corriendo: cierto en lo visto, no en general.
    Perfil,    ///< medido en corridas anteriores.
    Declarado, ///< lo afirma el programador (un contrato).  Se VERIFICA, no se cree.
};

/// Nombre estable para volcados.  No es texto de usuario.
const char *nombre_fuente(Fuente f);

/**
 * @brief Quien descubrio un hecho y mirando que.
 *
 * Un hecho sin procedencia no se puede explicar ni depurar: cuando un veredicto
 * sorprende, la primera pregunta es siempre "de donde ha salido esto".  El
 * @c productor es un literal estatico (el nombre del analisis), no una cadena
 * construida: identifica al modulo, no al caso.
 */
struct Procedencia {
    Fuente      fuente = Fuente::Estatico;
    const char *productor = "?"; ///< analisis que lo emitio.
    const char *funcion = "";    ///< funcion mirada (vacio si es de modulo).
    uint32_t    sitio = 0;       ///< value-id, bloque o linea, segun el dominio.
};

/**
 * @brief Que otros hechos sostienen a este.
 *
 * Se guarda por PRODUCTOR, no por instancia: basta para saber a quien invalidar
 * cuando algo cambia, y no obliga a que cada hecho arrastre punteros a otros.
 * Un maximo pequeno y fijo evita que un hecho crezca sin control; si a un
 * dominio le hicieran falta mas dependencias, es senal de que ese hecho hace
 * demasiadas cosas.
 */
struct Dependencias {
    static constexpr int kMax = 4;
    const char *de[kMax] = {nullptr, nullptr, nullptr, nullptr};

    void anadir(const char *productor) {
        for (int i = 0; i < kMax; ++i) {
            if (de[i] == nullptr) { de[i] = productor; return; }
            if (de[i] == productor) return;
        }
    }
    bool depende_de(const char *productor) const {
        for (int i = 0; i < kMax; ++i)
            if (de[i] == productor) return true;
        return false;
    }
};

/**
 * @brief Lo que TODO hecho de ASA lleva encima, sea cual sea su dominio.
 *
 * Se embebe por composicion en cada hecho concreto en vez de heredarse: los
 * hechos se copian en bucles calientes y viven en vectores, y una jerarquia con
 * funciones virtuales pagaria indireccion en el sitio equivocado.
 */
struct Sello {
    Certeza      certeza = Certeza::Desconocida;
    Procedencia  origen;
    Dependencias apoyos;
};

// ===========================================================================
// EL HECHO.  La frontera comun: lo que un productor deja y un consumidor lee.
// ===========================================================================

/// Identidad de un hecho dentro de un almacen.  Indice, no puntero: los hechos
/// viven en un vector contiguo y se referencian entre si.
using FactId = uint32_t;
/// "Ningun hecho".  No es el hecho cero.
extern const FactId kSinHecho;

/**
 * @brief De QUE habla un hecho.
 *
 * Tipado y no una cadena: el sujeto es la clave por la que un consumidor busca,
 * y compararlo por texto convertiria cada consulta en un parseo.  El nombre de
 * la funcion es un puntero al que guarda el almacen, estable mientras el viva.
 */
struct Sujeto {
    enum class Clase : uint8_t {
        Modulo,      ///< el programa entero.
        Funcion,     ///< una funcion; @c id no se usa.
        Valor,       ///< un valor SSA; @c id es su value-id.
        Bloque,      ///< un bloque basico; @c id es su indice.
        Instruccion, ///< una instruccion; @c id es su posicion lineal.
        Simbolo      ///< algo con nombre que no es funcion (global, clase).
    };
    Clase       clase = Clase::Modulo;
    const char *funcion = ""; ///< a quien pertenece (vacio si es del modulo).
    uint32_t    id = 0;

    bool operator==(const Sujeto &o) const {
        return clase == o.clase && id == o.id &&
               (funcion == o.funcion || std::string(funcion) == o.funcion);
    }
};

const char *nombre_clase_sujeto(Sujeto::Clase c);

/**
 * @brief QUE se afirma, en DATOS.
 *
 * Cada dominio tiene su vocabulario -- un rango no es un prestamo --, pero el
 * ASA exige que la afirmacion sea material y no una frase: un @c codigo estable
 * que un consumidor puede comparar, los NUMEROS que la acompanan, y un texto
 * SOLO para lo que no cabe en numeros.  Quien decida algo mira @c codigo y los
 * numeros; el texto es para que lo lea una persona.
 */
struct Proposicion {
    const char *dominio = "?"; ///< quien habla (uno de los productores).
    const char *codigo = "?";  ///< que dice, en vocabulario estable del dominio.
    int64_t     a = 0;         ///< primer numero del hecho (lo, offset, ...).
    int64_t     b = 0;         ///< segundo (hi, ancho, profundidad, ...).
    /**
     * @brief Lo que no cabe en dos numeros, INTERNADO en el almacen.
     *
     * Puntero y no cadena propia por dos razones que van juntas: un programa
     * grande produce cientos de miles de hechos y los textos se REPITEN
     * ("vale todo su tipo", "i64", "monton#3"), asi que internarlos guarda una
     * sola copia de cada uno; y comparar dos hechos pasa a ser comparar
     * punteros.  Vive lo que el almacen.
     */
    const char *detalle = "";
};

/**
 * @brief COMO se llego a el.
 *
 * @c Dependencias dice de que PRODUCTORES depende (grueso, para invalidar);
 * esto dice de que HECHOS CONCRETOS se dedujo, que es lo que permite recorrer
 * una derivacion hacia atras y ensenarla.  Sin ella, "todo veredicto lleva su
 * prueba" se queda en una intencion.
 */
struct Prueba {
    const char          *regla = ""; ///< la regla aplicada, nombre estable.
    std::vector<FactId>  de;         ///< hechos de los que se sigue.
};

/**
 * @brief DONDE vale un hecho.  Un campo vacio quiere decir "en cualquiera".
 *
 * NO TODO LO QUE SE SABE VALE EN TODAS PARTES, y hasta ahora eso no se podia
 * decir: la unica forma de proteger un hecho especifico de una arquitectura era
 * separar el ALMACEN ENTERO por objetivo, con lo que se duplicaba tambien todo
 * lo universal -- que es la mayor parte.  El alcance es propiedad del HECHO, no
 * del sitio donde se guarda: dicho aqui, un solo almacen sirve a todos los
 * objetivos y solo se descarta lo que de verdad es de otro.
 *
 * Los tres ejes son independientes y se cruzan: un hecho puede valer para
 * cualquier backend pero solo en x86-64 (lo que exige una instruccion concreta),
 * o para cualquier ISA pero solo compilando a nativo (lo que impone el enlace),
 * o para todo (que un valor cabe en 32 bits).
 *
 * REGLA AL PRODUCIR: en la duda, el alcance MAS ESTRECHO.  Equivocarse hacia
 * estrecho solo cuesta recalcular; hacia ancho es afirmar en un sitio algo que
 * se comprobo en otro.
 */
struct Ambito {
    const char *isa = "";      ///< "x86-64", "aarch64"...  "" = cualquiera.
    const char *sistema = "";  ///< "windows", "linux"...   "" = cualquiera.
    const char *backend = "";  ///< "vm", "aot".            "" = cualquiera.

    /// Si un campo del hecho es vacio vale en cualquiera; si no, tiene que
    /// coincidir con el de @p aqui.
    bool vale_en(const Ambito &aqui) const {
        auto casa = [](const char *mio, const char *suyo) {
            if (mio == nullptr || mio[0] == ' ') return true;
            if (suyo == nullptr) return false;
            for (size_t i = 0;; ++i) {
                if (mio[i] != suyo[i]) return false;
                if (mio[i] == ' ') return true;
            }
        };
        return casa(isa, aqui.isa) && casa(sistema, aqui.sistema) &&
               casa(backend, aqui.backend);
    }
    /// Universal: sin ninguna restriccion.
    bool universal() const {
        return (isa == nullptr || isa[0] == ' ') &&
               (sistema == nullptr || sistema[0] == ' ') &&
               (backend == nullptr || backend[0] == ' ');
    }
};

/**
 * @brief Un hecho completo.  Es LA representacion; no hay otra.
 *
 * Da igual que lo produzca el analisis estatico, la observacion en ejecucion o
 * un perfil de corridas anteriores: lo que cambia es la @c Procedencia y la
 * @c Certeza, que viajan DENTRO.  Un consumidor no pregunta de donde viene: mira
 * lo que dice y cuanto puede fiarse.
 */
struct Fact {
    Proposicion que;
    Sujeto      de_quien;
    Ambito      donde; ///< en que objetivos vale (vacio = en todos).
    Sello       sello;
    Prueba      prueba;
};

} // namespace asa
} // namespace analysis

#endif // ANALYSIS_ASA_FACT_H
