/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file explain.cpp
 * @brief Lee el fichero acompanante del AOT y explica un punto del codigo.
 *
 * Ver @ref vxdbg/explain.h para por que esto vive FUERA del binario.
 */

#include "vxdbg/explain.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

namespace vxdbg {

namespace {

/// Una funcion del binario y donde cambia su linea de fuente.
struct FuncionMapeada {
    std::string nombre;
    /// Pares (desplazamiento dentro de la funcion, linea), ordenados.
    std::vector<std::pair<uint32_t, uint32_t>> cambios;
};

/// Lo que el AOT dejo escrito al compilar.
struct Acompanante {
    std::string fuente; ///< Ruta del `.vx` compilado.
    std::vector<FuncionMapeada> funciones;
};

/**
 * @brief Lee un entero de 32 bits del buffer y avanza.
 * @param d Datos.
 * @param off Posicion (se avanza).
 * @param out Valor leido.
 * @return false si no habia bytes suficientes.
 */
bool leer_u32(const std::vector<uint8_t> &d, size_t &off, uint32_t &out) {
    if (off + 4 > d.size()) return false;
    std::memcpy(&out, d.data() + off, 4);
    off += 4;
    return true;
}

/**
 * @brief Lee una cadena con longitud delante y avanza.
 * @param d Datos.
 * @param off Posicion (se avanza).
 * @param out Cadena leida.
 * @return false si el buffer se queda corto.
 */
bool leer_str(const std::vector<uint8_t> &d, size_t &off, std::string &out) {
    uint32_t n = 0;
    if (!leer_u32(d, off, n)) return false;
    if (off + n > d.size()) return false;
    out.assign(reinterpret_cast<const char *>(d.data() + off), n);
    off += n;
    return true;
}

/**
 * @brief Carga el fichero acompanante de un binario.
 * @param ruta Ruta del `.vxdbg`.
 * @param out Contenido interpretado.
 * @param err Motivo del fallo.
 * @return true si se pudo leer entero.
 */
bool cargar(const std::string &ruta, Acompanante &out, std::string &err) {
    std::ifstream f(ruta, std::ios::binary);
    if (!f) {
        err = "no se encuentra '" + ruta +
              "'; hay que compilar con --debug-info=1";
        return false;
    }
    const std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
    size_t off = 0;
    if (d.size() < 8 || std::memcmp(d.data(), "VXDG", 4) != 0) {
        err = "'" + ruta + "' no es un fichero de depuracion de Vesta";
        return false;
    }
    off = 4;
    uint32_t version = 0;
    if (!leer_u32(d, off, version) || version != 1u) {
        err = "version desconocida del fichero de depuracion";
        return false;
    }
    if (!leer_str(d, off, out.fuente)) return false;
    uint32_t nf = 0;
    if (!leer_u32(d, off, nf)) return false;
    out.funciones.reserve(nf);
    for (uint32_t i = 0; i < nf; ++i) {
        FuncionMapeada fm;
        if (!leer_str(d, off, fm.nombre)) return false;
        uint32_t nc = 0;
        if (!leer_u32(d, off, nc)) return false;
        fm.cambios.reserve(nc);
        for (uint32_t k = 0; k < nc; ++k) {
            uint32_t a = 0, b = 0;
            if (!leer_u32(d, off, a) || !leer_u32(d, off, b)) return false;
            fm.cambios.emplace_back(a, b);
        }
        out.funciones.push_back(std::move(fm));
    }
    return true;
}

/**
 * @brief Parte `funcion+0xNN` en sus dos partes.
 * @param donde Texto tal cual lo escribio quien llama.
 * @param nombre Nombre de la funcion.
 * @param desp Desplazamiento (0 si no se dio).
 * @return false si el desplazamiento no es un numero.
 */
bool partir(const std::string &donde, std::string &nombre, uint64_t &desp) {
    const size_t mas = donde.rfind('+');
    if (mas == std::string::npos) {
        nombre = donde;
        desp = 0;
        return true;
    }
    nombre = donde.substr(0, mas);
    const std::string n = donde.substr(mas + 1);
    if (n.empty()) return false;
    char *fin = nullptr;
    desp = std::strtoull(n.c_str(), &fin, 0);
    return fin != nullptr && *fin == '\0';
}

/**
 * @brief Escribe una linea del fuente, sin la sangria de la izquierda.
 * @param ruta Fichero.
 * @param linea Numero de linea (1 en adelante).
 * @return true si se pudo leer.
 */
bool escribir_linea_fuente(const std::string &ruta, uint32_t linea) {
    std::ifstream f(ruta);
    if (!f || linea == 0) return false;
    std::string texto;
    for (uint32_t i = 0; i < linea && std::getline(f, texto); ++i) {
    }
    const size_t ini = texto.find_first_not_of(" \t");
    if (ini == std::string::npos) return false;
    std::cout << "      " << texto.substr(ini) << "\n";
    return true;
}

} // namespace

bool explain_location(const std::string &binario, const std::string &donde,
                      std::string &err) {
    Acompanante ac;
    if (!cargar(binario + ".vxdbg", ac, err)) return false;

    std::string nombre;
    uint64_t desp = 0;
    if (!partir(donde, nombre, desp)) {
        err = "no entiendo '" + donde + "'; se espera funcion+0xNN";
        return false;
    }

    const FuncionMapeada *fm = nullptr;
    for (const auto &f : ac.funciones)
        if (f.nombre == nombre) {
            fm = &f;
            break;
        }
    if (fm == nullptr) {
        err = "el binario no tiene ninguna funcion '" + nombre + "'";
        return false;
    }

    /* La linea es la del ULTIMO cambio que quede en o antes del punto: entre
     * dos cambios, la linea sigue siendo la del primero. */
    uint32_t linea = 0;
    for (const auto &c : fm->cambios) {
        if (c.first > desp) break;
        linea = c.second;
    }
    if (linea == 0) {
        /* Pasa en el prologo: el codigo de entrada de una funcion no viene de
         * ninguna linea escrita.  Se dice desde donde SI consta, que es lo que
         * quien pregunta necesita saber para volver a preguntar. */
        err = "el punto +0x" + std::to_string(desp) + " de '" + nombre +
              "' es anterior a la primera linea atribuida";
        if (!fm->cambios.empty()) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), " (la primera es +0x%x, linea %u)",
                          (unsigned)fm->cambios.front().first,
                          (unsigned)fm->cambios.front().second);
            err += buf;
        }
        return false;
    }

    std::cout << "  en " << nombre << "+0x" << std::hex << desp << std::dec
              << " (" << ac.fuente << ":" << linea << ")\n";
    if (!escribir_linea_fuente(ac.fuente, linea)) {
        std::cout << "      (no se pudo leer el fuente en '" << ac.fuente
                  << "')\n";
    }
    return true;
}

} // namespace vxdbg
