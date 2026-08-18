/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file jit/osr_registry.cpp
 * @brief Registro de bucles re-entrables: implementacion.
 *
 * Estado global deliberado, y por una razon: los bucles se apuntan al compilar
 * y se consultan al ejecutar, en momentos distintos y desde sitios distintos.
 * Lo que NO hay aqui es ninguna decision de generacion de codigo -- emitir el
 * contador y la captura es de quien tiene el marco de pila delante.
 */

#include "jit/osr_registry.h"

#include <cstdio>
#include <cstdlib>

namespace jit {
namespace {

/**
 * @struct Bucle
 * @brief Lo que se sabe de un bucle instrumentado.
 */
struct Bucle {
    std::string fn_name;                ///< funcion que lo contiene.
    uint32_t header_block = 0;          ///< bloque de entrada.
    std::vector<osr::Captura> capturas; ///< estado vivo a la entrada.
    bool abortado = false;              ///< true si no se capturo el estado.
};

/* Indexado por identificador de bucle. */
std::vector<Bucle> g_bucles;
/* Vueltas totales: lo incrementa el codigo emitido, por direccion. */
uint64_t g_vueltas = 0;
/* Cuantos bucles se han instrumentado (== el proximo identificador). */
uint32_t g_sitios = 0;
/* Cuantas veces se cruzo el umbral. */
uint64_t g_disparos = 0;
/* Quien atiende el disparo; sin el, el disparador solo cuenta. */
uint64_t (*g_handler)(uint64_t) = nullptr;

/**
 * @brief ¿Se detalla cada disparo por la salida de errores?
 *
 * `VESTA_OSR_LOG=1`.  Apagado por defecto para no llenar la consola cuando el
 * salto ya funciona y no se esta depurando.
 *
 * @return true si esta activado.
 */
bool detalle_activado() noexcept {
    static const bool on = [] {
        const char *v = std::getenv("VESTA_OSR_LOG");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return on;
}

/**
 * @brief El disparador.  Lo llama el codigo emitido una vez, cuando el bucle
 *        cruza el umbral, con el estado capturado.
 *
 * @param loop_id Identificador del bucle.
 * @param buffer  Estado capturado, indexado por identificador de valor.
 * @return Direccion por la que entrar en la version mejor, o 0 para seguir.
 */
uint64_t disparar(void * /*proc*/, uint64_t loop_id, uint64_t *buffer) {
    ++g_disparos;
    if (detalle_activado()) {
        if (loop_id < g_bucles.size()) {
            const Bucle &b = g_bucles[static_cast<size_t>(loop_id)];
            std::fprintf(stderr,
                         "[osr] disparo bucle=%llu fn=%s bloque_entrada=%u "
                         "capturas=%zu%s\n",
                         static_cast<unsigned long long>(loop_id),
                         b.fn_name.c_str(), b.header_block, b.capturas.size(),
                         b.abortado ? " (ABORTADO: estado no capturable)" : "");
            if (buffer && !b.abortado) {
                for (const osr::Captura &c : b.capturas) {
                    const uint64_t val = buffer[c.vid];
                    std::fprintf(stderr, "[osr]   v%u%s = %lld (0x%llx)\n",
                                 c.vid, c.es_gc ? " gc" : "",
                                 static_cast<long long>(val),
                                 static_cast<unsigned long long>(val));
                }
            }
        } else {
            std::fprintf(stderr, "[osr] disparo bucle=%llu (umbral %u)\n",
                         static_cast<unsigned long long>(loop_id),
                         osr::umbral());
        }
    }
    return g_handler ? g_handler(loop_id) : 0;
}

} // namespace

namespace osr {

bool contador_activado() noexcept {
    static const bool on = [] {
        const char *v = std::getenv("VESTA_OSR_COUNT");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return on;
}

uint32_t umbral() noexcept {
    static const uint32_t t = [] {
        const char *v = std::getenv("VESTA_OSR_THRESHOLD");
        return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 10))
                 : 100000u;
    }();
    return t;
}

uint64_t registrar_bucle(std::string fn_name, uint32_t header_block,
                         std::vector<Captura> capturas, bool abortado) {
    const uint64_t id = g_sitios++;
    if (g_bucles.size() <= static_cast<size_t>(id))
        g_bucles.resize(static_cast<size_t>(id) + 1u);
    Bucle &b = g_bucles[static_cast<size_t>(id)];
    b.fn_name = std::move(fn_name);
    b.header_block = header_block;
    b.capturas = std::move(capturas);
    b.abortado = abortado;
    return id;
}

uint64_t *contador_de_vueltas() noexcept {
    return &g_vueltas;
}

void *disparador() noexcept {
    return reinterpret_cast<void *>(&disparar);
}

void instalar_resumen_al_salir() {
    static bool instalado = false;
    if (instalado) return;
    instalado = true;
    std::atexit([] {
        std::fprintf(stderr,
                     "[osr] bucles=%u  vueltas_totales=%llu  disparos=%llu\n",
                     g_sitios, static_cast<unsigned long long>(g_vueltas),
                     static_cast<unsigned long long>(g_disparos));
    });
}

} // namespace osr

void set_osr_handler(uint64_t (*handler)(uint64_t)) {
    g_handler = handler;
}

uint32_t osr_loop_count() {
    return g_sitios;
}

bool osr_loop_info(uint64_t loop_id, std::string &fn_name_out,
                   uint32_t &header_block_out) {
    if (loop_id >= g_bucles.size()) return false;
    const Bucle &b = g_bucles[static_cast<size_t>(loop_id)];
    if (b.abortado) return false;
    fn_name_out = b.fn_name;
    header_block_out = b.header_block;
    return true;
}

bool osr_loop_captures(uint64_t loop_id, std::vector<uint32_t> &out_vids) {
    if (loop_id >= g_bucles.size()) return false;
    const Bucle &b = g_bucles[static_cast<size_t>(loop_id)];
    if (b.abortado) return false;
    out_vids.clear();
    out_vids.reserve(b.capturas.size());
    for (const osr::Captura &c : b.capturas)
        out_vids.push_back(c.vid);
    return true;
}

} // namespace jit
