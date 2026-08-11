/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/analysis/test_asa_fact_file.cpp
 * @brief Que un hecho sobreviva al disco SIENDO EL MISMO hecho.
 *
 * El test EXIGE las propiedades de las que depende todo lo que se construya
 * encima, no que el fichero se escriba:
 *
 *  - que la DERIVACION siga en pie al volver: un hecho que se apoyaba en otro
 *    tiene que seguir apoyandose en ese y no en el vecino, con los
 *    identificadores recolocados sobre lo que ya hubiera en el almacen;
 *  - que los nombres de productor recuperen su LITERAL, porque ASA los compara
 *    por direccion y una cadena recien leida no se reconoceria a si misma;
 *  - que una huella que no cuadra NO se lea -- responder con hechos de otro
 *    programa es peor que no responder --;
 *  - que un dominio desconocido SE SALTE en vez de romper el fichero, que es lo
 *    que permite añadir productores sin invalidar las caches escritas antes;
 *  - que un fichero truncado se descarte y lo diga;
 *  - y que el NIVEL de cache no cambie lo que se sabe, solo cuanto se guarda.
 */

#include "analysis/asa/base_hechos.h"
#include "analysis/asa/fact_file.h"
#include "analysis/asa/fact_store.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace analysis::asa;

static int g_checks = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_fail;                                                          \
            std::printf("  [FALLO] %s (linea %d)\n", (msg), __LINE__);         \
        }                                                                      \
    } while (0)

/// Un almacen pequeño con una derivacion de dos escalones y dos dominios.
static void poblar(FactStore &a) {
    Fact base;
    base.que.dominio = kProductorEstructura;
    base.que.codigo = "estructura.forma";
    base.que.a = 3;
    base.que.b = 7;
    base.que.detalle = a.internar("tres bloques");
    base.de_quien.clase = Sujeto::Clase::Funcion;
    base.de_quien.funcion = a.internar("f");
    base.sello.certeza = Certeza::Demostrada;
    base.sello.origen.productor = kProductorEstructura;
    base.sello.origen.funcion = a.internar("f");
    const FactId id_base = a.anadir(std::move(base));

    Fact rango;
    rango.que.dominio = kProductorRangos;
    rango.que.codigo = "rango.acotado";
    rango.que.a = -5;
    rango.que.b = 40;
    rango.que.detalle = a.internar("del parametro");
    rango.de_quien.clase = Sujeto::Clase::Valor;
    rango.de_quien.funcion = a.internar("f");
    rango.de_quien.id = 12;
    rango.sello.certeza = Certeza::Inferida;
    rango.sello.origen.fuente = Fuente::Perfil;
    rango.sello.origen.productor = kProductorRangos;
    rango.sello.origen.sitio = 12;
    rango.sello.apoyos.anadir(kProductorEstructura);
    rango.prueba.regla = "flujo-de-datos";
    rango.prueba.de.push_back(id_base);
    a.anadir(std::move(rango));
}

// ===========================================================================
// 1. Ida y vuelta: los hechos vuelven enteros y la derivacion se sostiene
// ===========================================================================
static void probar_ida_y_vuelta() {
    FactStore origen;
    poblar(origen);
    const std::vector<uint8_t> bytes =
        serialize(origen, 0xCAFEull, CacheLevel::All, {});
    CHECK(!bytes.empty(), "escribir dos hechos no puede dar un fichero vacio");

    FactStore destino;
    const ReadResult r = read_facts(bytes.data(), bytes.size(), 0xCAFEull, destino);
    CHECK(r.ok, "el fichero recien escrito tiene que poder leerse");
    CHECK(r.facts == 2, "vuelven los dos hechos");
    CHECK(r.domains == 2, "cada dominio es su propio registro");
    CHECK(r.lost_proofs == 0, "no se pierde ningun apoyo");
    CHECK(destino.size() == 2, "y quedan depositados en el almacen");

    const Fact &f0 = destino.at(0);
    CHECK(std::string(f0.que.codigo) == "estructura.forma", "el codigo vuelve");
    CHECK(f0.que.a == 3 && f0.que.b == 7, "los numeros vuelven");
    CHECK(std::string(f0.que.detalle) == "tres bloques", "el detalle vuelve");
    CHECK(f0.sello.certeza == Certeza::Demostrada, "la certeza vuelve");

    const Fact &f1 = destino.at(1);
    CHECK(f1.que.a == -5, "un numero negativo no se estropea al guardarlo");
    CHECK(f1.sello.origen.fuente == Fuente::Perfil, "la fuente vuelve");
    CHECK(f1.de_quien.clase == Sujeto::Clase::Valor, "la clase de sujeto vuelve");
    CHECK(f1.de_quien.id == 12, "y el valor del que habla");

    /* Lo que de verdad importa: el hecho derivado sigue apoyandose EN EL MISMO,
     * y se puede recorrer hacia atras.  Sin esto el fichero guardaria hechos
     * sueltos y no conocimiento. */
    CHECK(f1.prueba.de.size() == 1, "el apoyo sobrevive");
    CHECK(f1.prueba.de[0] == 0, "y apunta al hecho que lo sostiene");
    CHECK(std::string(f1.prueba.regla) == "flujo-de-datos", "la regla vuelve");
    CHECK(destino.explicar(1).size() == 2, "la derivacion se recorre entera");
}

// ===========================================================================
// 2. Los nombres vuelven a SER los literales (ASA compara por direccion)
// ===========================================================================
static void probar_identidad_de_nombres() {
    FactStore origen;
    poblar(origen);
    const std::vector<uint8_t> bytes =
        serialize(origen, 1, CacheLevel::All, {});
    FactStore destino;
    read_facts(bytes.data(), bytes.size(), 1, destino);

    CHECK(destino.at(0).que.dominio == kProductorEstructura,
          "el dominio vuelve a ser el literal, no una copia");
    CHECK(destino.at(1).sello.origen.productor == kProductorRangos,
          "el productor tambien");
    CHECK(destino.at(1).sello.apoyos.depende_de(kProductorEstructura),
          "y por eso la dependencia se sigue reconociendo");
    /* Y la consulta por dominio, que va indexada por ese mismo puntero. */
    CHECK(destino.de_dominio(kProductorRangos).size() == 1,
          "un hecho de disco se encuentra buscando por su dominio");
}

// ===========================================================================
// 3. Se AÑADE a lo que ya hay, recolocando los identificadores
// ===========================================================================
static void probar_anade_sobre_lo_existente() {
    FactStore origen;
    poblar(origen);
    const std::vector<uint8_t> bytes =
        serialize(origen, 2, CacheLevel::All, {});

    FactStore destino;
    Fact      previo;
    previo.que.dominio = kProductorBucles;
    previo.que.codigo = "bucle.forma";
    destino.anadir(std::move(previo));

    const ReadResult r = read_facts(bytes.data(), bytes.size(), 2, destino);
    CHECK(r.ok && destino.size() == 3, "lo leido se suma a lo que ya habia");
    /* El apoyo se ha corrido: el hecho base ya no es el 0, es el 1. */
    CHECK(destino.at(2).prueba.de.size() == 1 && destino.at(2).prueba.de[0] == 1,
          "los apoyos se recolocan sobre el almacen destino");
}

// ===========================================================================
// 4. Una huella que no cuadra no se lee
// ===========================================================================
static void probar_huella() {
    FactStore origen;
    poblar(origen);
    const std::vector<uint8_t> bytes =
        serialize(origen, 0xAAAAull, CacheLevel::All, {});

    FactStore destino;
    const ReadResult r = read_facts(bytes.data(), bytes.size(), 0xBBBBull, destino);
    CHECK(!r.ok, "hechos de otro modulo no se aceptan");
    CHECK(destino.size() == 0, "y no se deposita nada a medias");
    /* Y el motivo es un DATO con su codigo de catalogo, no una frase: quien lo
     * enseñe lo hace en el idioma de quien compila. */
    CHECK(r.reason == ReadReason::OtherModule, "y se dice por que, como dato");
    CHECK(std::string(diag_code(r.reason)) == "VXA042",
          "con su codigo del catalogo multi-idioma");
}

// ===========================================================================
// 5. Un dominio desconocido se SALTA; el resto del fichero se lee
// ===========================================================================
static void probar_dominio_desconocido() {
    FactStore origen;
    poblar(origen);
    const std::vector<uint8_t> bytes =
        serialize(origen, 3, CacheLevel::All, {});

    /* Se simula un productor de otra version tocando la version DEL PRIMER
     * REGISTRO.  Es lo que pasaria de verdad: alguien cambia el contenido de su
     * dominio y las caches escritas antes traen el layout viejo. */
    std::vector<uint8_t> tocado = bytes;
    const std::string    marca(kProductorEstructura);
    size_t               pos = std::string::npos;
    for (size_t i = 0; i + marca.size() <= tocado.size(); ++i) {
        if (std::memcmp(tocado.data() + i, marca.data(), marca.size()) == 0) {
            pos = i;
            break;
        }
    }
    CHECK(pos != std::string::npos, "el nombre del dominio esta en el fichero");
    if (pos == std::string::npos) return;
    tocado[pos + marca.size()] = 0xFE; // version del hecho, byte bajo.

    FactStore destino;
    const ReadResult r = read_facts(tocado.data(), tocado.size(), 3, destino);
    CHECK(r.ok, "un dominio ilegible no invalida el fichero entero");
    CHECK(r.skipped == 1, "se cuenta el registro que no se entendio");
    CHECK(r.facts == 1, "y se lee el que si");
    CHECK(r.lost_proofs == 1,
          "el apoyo que se quedo fuera se PIERDE y se cuenta");
    CHECK(destino.at(0).prueba.de.empty(),
          "nunca se guarda una referencia a un hecho que no existe");
}

// ===========================================================================
// 6. Un fichero truncado se descarta y lo dice
// ===========================================================================
static void probar_truncado() {
    FactStore origen;
    poblar(origen);
    std::vector<uint8_t> bytes = serialize(origen, 4, CacheLevel::All, {});
    bytes.resize(bytes.size() / 2);

    FactStore destino;
    const ReadResult r = read_facts(bytes.data(), bytes.size(), 4, destino);
    CHECK(!r.ok, "medio fichero no se da por bueno");
    CHECK(destino.size() == 0, "y no se deposita lo que se alcanzo a leer");
}

// ===========================================================================
// 7. El nivel decide CUANTO se guarda, nunca QUE se sabe
// ===========================================================================
static void probar_niveles() {
    FactStore origen;
    poblar(origen);

    /* Estructura es cara de rehacer; rangos, barato y recomputable. */
    const std::vector<DomainCost> costes = {
        {kProductorEstructura, 50000, false},
        {kProductorRangos, 10, true},
    };

    CHECK(serialize(origen, 5, CacheLevel::Off, costes).empty(),
          "con la cache apagada no se escribe nada");

    FactStore d_min;
    const std::vector<uint8_t> b_min =
        serialize(origen, 5, CacheLevel::Minimum, costes);
    read_facts(b_min.data(), b_min.size(), 5, d_min);
    CHECK(d_min.size() == 1, "el minimo guarda solo lo que no se puede rehacer");

    FactStore d_coste;
    const std::vector<uint8_t> b_coste =
        serialize(origen, 5, CacheLevel::ByCost, costes);
    read_facts(b_coste.data(), b_coste.size(), 5, d_coste);
    CHECK(d_coste.size() == 1, "por coste descarta lo barato de rehacer");

    FactStore d_todo;
    const std::vector<uint8_t> b_todo =
        serialize(origen, 5, CacheLevel::All, costes);
    read_facts(b_todo.data(), b_todo.size(), 5, d_todo);
    CHECK(d_todo.size() == 2, "y el nivel maximo se lo lleva todo");

    /* La regla que hace sano el sistema: los niveles son MONOTONOS.  Lo que
     * guarda uno lo guarda el siguiente, asi que cambiar de nivel no invalida
     * lo que ya hay en disco -- solo deja de producir mas. */
    CHECK(b_min.size() <= b_coste.size() && b_coste.size() <= b_todo.size(),
          "cada nivel contiene al anterior");

    /* Y por coste, con el mismo dominio caro pero recomputable, sigue entrando:
     * el criterio es lo que cuesta rehacerlo, no su naturaleza. */
    const std::vector<DomainCost> caro = {
        {kProductorEstructura, 50000, true},
        {kProductorRangos, 10, true},
    };
    FactStore d_caro;
    const std::vector<uint8_t> b_caro =
        serialize(origen, 5, CacheLevel::ByCost, caro);
    read_facts(b_caro.data(), b_caro.size(), 5, d_caro);
    CHECK(d_caro.size() == 1, "lo recomputable pero caro tambien se guarda");
}

// ===========================================================================
// 8. Un almacen vacio no escribe fichero (no hay nada que decir)
// ===========================================================================
static void probar_vacio() {
    FactStore vacio;
    CHECK(serialize(vacio, 6, CacheLevel::All, {}).empty(),
          "sin hechos no se escribe un fichero con solo cabecera");

    FactStore destino;
    const ReadResult r = read_facts(nullptr, 0, 6, destino);
    CHECK(!r.ok && r.reason == ReadReason::Empty,
          "leer nada dice por que no se pudo");
    CHECK(diag_code(ReadReason::Ok)[0] == '\0',
          "y cuando todo fue bien no hay nada que contar");
}

// ===========================================================================
// 9. La cache es GRANULAR: tocar un dominio no tira los demas
// ===========================================================================
static void probar_granularidad() {
    FactStore origen;
    poblar(origen);

    /* Cada dominio guarda la huella de LO QUE EL MIRO. */
    std::vector<DomainCost> escritos = {
        {kProductorEstructura, 0, true, 0x1111ull},
        {kProductorRangos, 0, true, 0x2222ull},
    };
    const std::vector<uint8_t> bytes =
        serialize(origen, 7, CacheLevel::All, escritos);

    /* Cambia lo que miraba UNO de los dos.  Lo suyo caduca; lo del otro no. */
    std::vector<DomainCost> vigentes = {
        {kProductorEstructura, 0, true, 0x1111ull},
        {kProductorRangos, 0, true, 0x9999ull},
    };
    FactStore        destino;
    const ReadResult r =
        read_facts(bytes.data(), bytes.size(), 7, destino, vigentes);
    CHECK(r.ok, "que un dominio caduque no invalida el fichero");
    CHECK(r.stale == 1, "caduca solo el registro cuya huella cambio");
    CHECK(r.facts == 1, "y se conserva lo del dominio que no se toco");
    CHECK(destino.at(0).que.dominio == kProductorEstructura,
          "lo que sobrevive es justo lo que no dependia de lo que cambio");

    /* Sin decir nada, se acepta todo: no se puede comprobar, y suponer lo peor
     * tiraria una cache que probablemente vale. */
    FactStore d2;
    CHECK(read_facts(bytes.data(), bytes.size(), 7, d2).facts == 2,
          "quien no dice de que depende se queda como estaba");

    /* Y un dominio que no supo decir de que dependia tampoco se puede tirar. */
    std::vector<DomainCost> sin_huella = {{kProductorEstructura, 0, true, 0}};
    const std::vector<uint8_t> b2 =
        serialize(origen, 7, CacheLevel::All, sin_huella);
    FactStore d3;
    CHECK(read_facts(b2.data(), b2.size(), 7, d3, vigentes).stale == 0,
          "sin huella no hay nada que comparar, asi que no caduca");
}

int main() {
    std::printf("=== ASA: los hechos sobreviven al disco ===\n");
    probar_ida_y_vuelta();
    probar_identidad_de_nombres();
    probar_anade_sobre_lo_existente();
    probar_huella();
    probar_dominio_desconocido();
    probar_truncado();
    probar_niveles();
    probar_vacio();
    probar_granularidad();
    std::printf("%d comprobaciones, %d fallos\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
