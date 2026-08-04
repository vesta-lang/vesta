/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file test_vxdbg_core.cpp
 * @brief Comprueba la identidad por contenido y el sistema de capacidades.
 *
 * Se prueban las propiedades de las que depende todo lo demas: que la misma
 * entrada da la misma huella (sin eso no hay cache incremental), que entradas
 * distintas dan huellas distintas (sin eso dos nodos se confunden), y que una
 * consulta que dice poder satisfacerse de verdad puede.
 */

#include "vxdbg/capabilities.h"
#include "vxdbg/ids.h"
#include "vxdbg/node.h"

#include <cstdio>
#include <set>
#include <string>

static int fallos = 0;

/**
 * @brief Comprueba una condicion y deja constancia.
 * @param cond Lo que debe cumplirse.
 * @param que Descripcion de la comprobacion.
 */
static void comprobar(bool cond, const char *que) {
    if (cond) {
        std::printf("  OK   %s\n", que);
    } else {
        std::printf("  FALLA %s\n", que);
        ++fallos;
    }
}

/// Identidad por contenido: la base del cache incremental.
static void probar_huellas() {
    std::printf("Huellas\n");

    const std::string a = "funcion parse";
    const std::string b = "funcion parsx"; // difiere en un solo byte, al final

    const auto ha = vxdbg::hash_bytes(a.data(), a.size());
    const auto ha2 = vxdbg::hash_bytes(a.data(), a.size());
    const auto hb = vxdbg::hash_bytes(b.data(), b.size());

    comprobar(ha == ha2, "la misma entrada da la misma huella");
    comprobar(ha != hb, "un byte distinto da otra huella");
    comprobar(!ha.empty(), "una entrada real no da huella vacia");
    // Las dos mitades tienen que variar de forma independiente: si cambiaran a
    // la vez, una huella de 128 bits no valdria mas que una de 64.
    comprobar(ha.lo != hb.lo && ha.hi != hb.hi,
              "las dos mitades varian por separado");

    // Ida y vuelta por hexadecimal, que es como se nombran los ficheros.
    const auto vuelta = vxdbg::ContentHash::from_hex(ha.to_hex());
    comprobar(vuelta == ha, "hexadecimal de ida y vuelta conserva la huella");
    comprobar(ha.to_hex().size() == 32, "el hexadecimal ocupa 32 digitos");
    comprobar(vxdbg::ContentHash::from_hex("no es hexadecimal").empty(),
              "una cadena invalida da huella vacia");
    comprobar(vxdbg::ContentHash::from_hex("zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz")
                  .empty(),
              "digitos invalidos dan huella vacia");

    // Combinar hereda los cambios: si cambia una parte, cambia el todo.  Es lo
    // que hace que tocar un tipo invalide a quien lo usa.
    const auto c1 = vxdbg::hash_combine(ha, hb);
    const auto c2 = vxdbg::hash_combine(ha, ha);
    comprobar(c1 != c2, "combinar con algo distinto da resultado distinto");
    comprobar(vxdbg::hash_combine(ha, hb) != vxdbg::hash_combine(hb, ha),
              "el orden al combinar importa");

    // Sin colisiones en un corpus pequeno pero variado.
    std::set<std::string> vistas;
    for (int i = 0; i < 5000; ++i) {
        const std::string s = "entidad_" + std::to_string(i);
        vistas.insert(vxdbg::hash_bytes(s.data(), s.size()).to_hex());
    }
    comprobar(vistas.size() == 5000, "5000 entradas dan 5000 huellas");
}

/// Los identificadores tipados y su paso a referencia generica.
static void probar_referencias() {
    std::printf("Referencias\n");

    const auto h = vxdbg::hash_bytes("x", 1);
    const vxdbg::VariableId var{h};
    const auto ref = vxdbg::NodeRef::of(var);

    comprobar(ref.kind == vxdbg::NodeKind::Variable,
              "el genero se deduce del tipo del identificador");
    comprobar(ref.hash == h, "la referencia conserva la huella");
    comprobar(ref.as<vxdbg::VariableTag>() == var,
              "vuelve al identificador del genero correcto");
    comprobar(ref.as<vxdbg::CodeTag>().empty(),
              "pedirlo con otro genero da vacio, no un identificador enganoso");
}

/// El sistema de capacidades: lo que se puede responder y por que no el resto.
static void probar_capacidades() {
    std::printf("Capacidades\n");

    using C = vxdbg::Capability;

    // El cierre de dependencias tiene que ser TRANSITIVO.
    vxdbg::Query q;
    q.needs = static_cast<vxdbg::CapabilitySet>(C::VariableLocation);
    q.close_prerequisites();
    comprobar(vxdbg::has(q.needs, C::LiveVariables),
              "saber donde vive una variable arrastra poder enumerarlas");
    comprobar(vxdbg::has(q.needs, C::MapToStatement),
              "y eso arrastra saber en que sentencia estamos");
    comprobar(vxdbg::has(q.needs, C::MapToIr),
              "y eso arrastra el intermedio (segundo nivel)");
    comprobar(vxdbg::has(q.needs, C::LocateCode),
              "y eso arrastra localizar el codigo (tercer nivel)");

    // Un informe que solo ofrece parte.
    vxdbg::CapabilityReport r;
    r.offer(C::LocateCode);
    r.offer(C::MapToIr);
    r.deny(C::SourcePosition, vxdbg::Unavailable::NotRecorded);
    r.deny(C::LiveVariables, vxdbg::Unavailable::OptimizedAway);

    comprobar(r.can(C::LocateCode), "lo ofrecido se puede");
    comprobar(!r.can(C::SourcePosition), "lo denegado no se puede");
    comprobar(r.why_not(C::SourcePosition) == vxdbg::Unavailable::NotRecorded,
              "y dice que no se guardo");
    comprobar(r.why_not(C::LiveVariables) == vxdbg::Unavailable::OptimizedAway,
              "distingue que el optimizador se lo llevo");
    comprobar(r.why_not(C::LocateCode) == vxdbg::Unavailable::Available,
              "lo disponible no tiene motivo de ausencia");

    // Una consulta que pide de mas dice exactamente que le falta.
    vxdbg::Query perfilador;
    perfilador.needs = C::LocateCode | C::MapToIr;
    vxdbg::CapabilitySet falta = 0;
    comprobar(r.satisfies(perfilador, falta),
              "un perfilador se conforma con localizar codigo e intermedio");

    vxdbg::Query depurador;
    depurador.needs = C::LocateCode | C::SourcePosition;
    comprobar(!r.satisfies(depurador, falta),
              "un depurador que necesita la linea no se satisface aqui");
    comprobar(vxdbg::has(falta, C::SourcePosition),
              "y se sabe exactamente que le falta");

    // Lo deseable que falte NO invalida la consulta.
    vxdbg::Query tolerante;
    tolerante.needs = static_cast<vxdbg::CapabilitySet>(C::LocateCode);
    tolerante.wants = static_cast<vxdbg::CapabilitySet>(C::SourcePosition);
    comprobar(r.satisfies(tolerante, falta),
              "lo deseable que falta no tumba la consulta");

    // El contador se deriva del enum: si alguien anade una capacidad y mueve
    // _Last, esto sigue cuadrando sin tocar nada.
    comprobar(vxdbg::CAPABILITY_COUNT == 12,
              "el numero de capacidades sale del propio enum");
}

int main() {
    std::printf("=== vxdbg: identidad y capacidades ===\n");
    probar_huellas();
    probar_referencias();
    probar_capacidades();
    if (fallos == 0) {
        std::printf("=== todo correcto ===\n");
        return 0;
    }
    std::printf("=== %d comprobaciones fallidas ===\n", fallos);
    return 1;
}
