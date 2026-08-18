/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file test_vxdbg_store.cpp
 * @brief Comprueba la serializacion y el almacen por contenido.
 *
 * Lo que se prueba no es que "funcione", sino las propiedades de las que
 * depende todo lo demas: que leer bytes rotos no se salga nunca, que guardar
 * algo que ya esta no cueste nada -- que es lo que hace el sistema incremental
 * -- y que el almacen en memoria y el de disco se comporten igual, porque si no
 * probar contra uno no diria nada del otro.
 */

#include "vxdbg/serialize.h"
#include "vxdbg/store.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

static int fallos = 0;

/**
 * @brief Comprueba una condicion y deja constancia.
 * @param cond Lo que debe cumplirse.
 * @param que Descripcion.
 */
static void comprobar(bool cond, const char *que) {
    if (cond) {
        std::printf("  OK   %s\n", que);
    } else {
        std::printf("  FALLA %s\n", que);
        ++fallos;
    }
}

/// Ida y vuelta de cada tipo, y que la lectura nunca se sale.
static void probar_serializacion() {
    std::printf("Serializacion\n");

    vxdbg::ByteWriter w;
    w.u8(0xAB);
    w.u16(0x1234);
    w.u32(0xDEADBEEF);
    w.u64(0x0123456789ABCDEFull);
    w.i64(-42);
    w.f64(3.5);
    w.boolean(true);
    w.boolean(false);
    w.str("una cadena");
    w.str("");
    const auto h = vxdbg::hash_bytes("x", 1);
    w.hash(h);

    const auto bytes = w.bytes();
    vxdbg::ByteReader r(bytes);
    comprobar(r.u8() == 0xAB, "u8 de ida y vuelta");
    comprobar(r.u16() == 0x1234, "u16 de ida y vuelta");
    comprobar(r.u32() == 0xDEADBEEF, "u32 de ida y vuelta");
    comprobar(r.u64() == 0x0123456789ABCDEFull, "u64 de ida y vuelta");
    comprobar(r.i64() == -42, "un entero negativo conserva el signo");
    comprobar(r.f64() == 3.5, "un real conserva sus bits");
    comprobar(r.boolean() == true && r.boolean() == false,
              "los booleanos de ida y vuelta");
    comprobar(r.str() == "una cadena", "una cadena de ida y vuelta");
    comprobar(r.str().empty(), "una cadena vacia sigue vacia");
    comprobar(r.hash() == h, "una huella de ida y vuelta");
    comprobar(r.ok(), "tras leerlo todo bien, el lector sigue sano");
    comprobar(r.remaining() == 0, "y no queda nada por leer");

    // El escritor queda reutilizable tras llevarse los bytes.
    vxdbg::ByteWriter w2;
    w2.u32(1);
    const auto sacados = w2.take();
    comprobar(sacados.size() == 4, "take() devuelve lo escrito");
    comprobar(w2.size() == 0, "y deja el escritor vacio, no en un estado raro");

    // Leer de menos NO debe salirse: es el caso de un fichero truncado.
    std::printf("Lectura de bytes truncados\n");
    std::vector<uint8_t> cortos(bytes.begin(), bytes.begin() + 3);
    vxdbg::ByteReader r2(cortos);
    (void)r2.u8();
    (void)r2.u64(); // pide 8 y solo quedan 2
    comprobar(!r2.ok(), "pedir mas de lo que hay marca el lector roto");
    comprobar(r2.u32() == 0, "y a partir de ahi devuelve ceros");
    comprobar(r2.remaining() == 0, "un lector roto no dice que le quede nada");

    // Una longitud disparatada en una cadena no debe reservar lo que diga.
    vxdbg::ByteWriter w3;
    w3.u32(0xFFFFFFFFu); // longitud imposible
    vxdbg::ByteReader r3(w3.bytes());
    comprobar(r3.str().empty(),
              "una longitud imposible no revienta la lectura");
    comprobar(!r3.ok(), "y deja constancia de que los bytes estaban mal");

    // Ojear no consume ni rompe.
    vxdbg::ByteWriter w4;
    w4.u8(7);
    vxdbg::ByteReader r4(w4.bytes());
    comprobar(r4.peek_u8() == 7, "ojear devuelve el siguiente byte");
    comprobar(r4.position() == 0, "y no lo consume");
    comprobar(r4.u8() == 7, "que sigue ahi para leerlo");
    comprobar(r4.peek_u8() == 0 && r4.ok(),
              "ojear el final da cero sin romper el lector");
}

/**
 * @brief Prueba comun a los dos almacenes.
 *
 * Se pasa el mismo juego a ambos: si el de disco no se comportara igual que el
 * de memoria, probar contra uno no diria nada del otro.
 *
 * @param s Almacen a probar.
 * @param nombre Como llamarlo en la salida.
 */
static void probar_almacen(vxdbg::NodeStore &s, const char *nombre) {
    std::printf("Almacen %s\n", nombre);

    vxdbg::StoredNode n;
    n.header.kind = vxdbg::NodeKind::Statement;
    n.header.schema_version = 1;
    n.payload = {1, 2, 3, 4, 5};
    const auto h = vxdbg::seal(n); // serializar -> sellar -> guardar

    comprobar(!s.contains(h), "al principio no esta");
    comprobar(s.put(n), "se guarda");
    comprobar(s.contains(h), "y ahora si esta");

    vxdbg::StoredNode leido;
    comprobar(s.get(h, leido), "se lee");
    comprobar(leido.header.kind == vxdbg::NodeKind::Statement,
              "conserva el genero del nodo");
    comprobar(leido.header.schema_version == 1,
              "conserva la version del esquema");
    comprobar(leido.payload == n.payload, "y el contenido intacto");

    // La propiedad que hace incremental al sistema: guardar algo que ya esta
    // no cuesta nada y no lo corrompe.
    comprobar(s.put(n), "guardar dos veces no falla");
    comprobar(s.get(h, leido) && leido.payload == n.payload,
              "y lo guardado sigue bien");

    // Lo que no esta, no esta.
    vxdbg::StoredNode nada;
    comprobar(!s.get(vxdbg::hash_bytes("no existe", 9), nada),
              "leer lo que no hay falla en vez de inventar");

    // Un nodo sin contenido tambien es un nodo.
    vxdbg::StoredNode vacio;
    vacio.header.kind = vxdbg::NodeKind::Scope;
    vacio.header.schema_version = 1;
    const auto hv = vxdbg::seal(vacio);
    comprobar(s.put(vacio), "se guarda un nodo sin contenido");

    // Un nodo sin huella no tiene donde ir: rechazarlo es mejor que guardarlo
    // bajo una clave inventada.
    vxdbg::StoredNode sin_huella;
    sin_huella.header.kind = vxdbg::NodeKind::Scope;
    comprobar(!s.put(sin_huella), "un nodo sin huella se rechaza");
    comprobar(s.get(hv, leido) && leido.payload.empty(),
              "y se lee con el contenido vacio");
}

/// El almacen de disco, ademas, tiene que repartir los ficheros.
static void probar_disco() {
    namespace fs = std::filesystem;
    const std::string raiz =
        (fs::temp_directory_path() / "vxdbg_prueba_almacen").string();
    std::error_code ec;
    fs::remove_all(raiz, ec);

    vxdbg::FileNodeStore s(raiz);
    probar_almacen(s, "en disco");

    std::printf("Reparto en disco\n");
    const auto h = vxdbg::hash_bytes("reparto", 7);
    const std::string ruta = s.path_for(h);
    const std::string hex = h.to_hex();
    comprobar(ruta.find("/" + hex.substr(0, 2) + "/") != std::string::npos,
              "el fichero va en la subcarpeta de sus dos primeros digitos");
    comprobar(ruta.size() > hex.size(), "y se nombra con la huella entera");

    // Un fichero a medias NO debe pasar por bueno.
    vxdbg::StoredNode n;
    n.header.kind = vxdbg::NodeKind::Code;
    n.payload = {9, 9, 9, 9, 9, 9, 9, 9};
    vxdbg::seal(n);
    const auto h2 = n.header.hash;
    s.put(n);
    {
        std::ofstream f(s.path_for(h2), std::ios::binary | std::ios::trunc);
        f << "roto";
    }
    vxdbg::StoredNode leido;
    comprobar(!s.get(h2, leido), "un fichero truncado no se da por bueno");

    // Con verificacion, un fichero MANIPULADO -- entero y bien formado, pero
    // con otro contenido -- tampoco pasa: si no, se serviria como si fuera el
    // nodo que se pedia.
    vxdbg::FileNodeStore sv(raiz, /*verify=*/true);
    vxdbg::StoredNode bueno;
    bueno.header.kind = vxdbg::NodeKind::Code;
    bueno.payload = {1, 2, 3};
    const auto hb = vxdbg::seal(bueno);
    sv.put(bueno);
    comprobar(sv.get(hb, leido), "con verificacion, lo intacto se lee igual");
    {
        // Se reescribe el fichero con otro contenido, bien formado.
        vxdbg::StoredNode falso;
        falso.header.kind = vxdbg::NodeKind::Code;
        falso.payload = {9, 9, 9};
        falso.header.hash = hb; // nombre de otro
        vxdbg::FileNodeStore sin_verificar(raiz);
        std::error_code e2;
        std::filesystem::remove(sv.path_for(hb), e2);
        sin_verificar.put(falso);
    }
    comprobar(!sv.get(hb, leido),
              "y un fichero manipulado se detecta en vez de servirse");

    fs::remove_all(raiz, ec);
}

int main() {
    std::printf("=== vxdbg: serializacion y almacen ===\n");
    probar_serializacion();

    vxdbg::MemoryNodeStore mem;
    probar_almacen(mem, "en memoria");
    comprobar(mem.size() == 2, "el de memoria sabe cuantos nodos tiene");

    // La misma clave con OTRO contenido no deberia poder ocurrir; si ocurre,
    // es un fallo de quien calculo la huella y hay que verlo aqui y no tres
    // capas mas arriba sirviendo un nodo por otro.
    vxdbg::StoredNode vacio_ref;
    vacio_ref.header.kind = vxdbg::NodeKind::Scope;
    const auto hv_ajena = vxdbg::seal(vacio_ref);
    vxdbg::StoredNode impostor;
    impostor.header.kind = vxdbg::NodeKind::Statement;
    impostor.header.hash = hv_ajena; // se le pone la clave de otro
    impostor.payload = {7, 7, 7};
    comprobar(!mem.put(impostor),
              "misma clave con otro contenido se rechaza en vez de pisarlo");

    probar_disco();

    if (fallos == 0) {
        std::printf("=== todo correcto ===\n");
        return 0;
    }
    std::printf("=== %d comprobaciones fallidas ===\n", fallos);
    return 1;
}
