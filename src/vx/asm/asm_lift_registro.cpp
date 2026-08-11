/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vx/asm/asm_lift_registro.cpp
 * @brief Implementacion del registro de bloques `asm` bajados (ver
 *        @c vx/asm/asm_lift_registro.h).
 */

#include "vx/asm/asm_lift_registro.h"

#include <mutex>

namespace vx {

namespace {

/// El registro y su cerrojo.  Un proyecto se compila con varios hilos, asi que
/// dos modulos pueden estar bajando asm a la vez.  Anotar es raro (una vez por
/// bloque `asm` de todo el programa), asi que un cerrojo simple sobra: aqui no
/// hay nada caliente que proteger.
std::mutex                   &cerrojo() {
    static std::mutex m;
    return m;
}
std::vector<BloqueAsmBajado> &registro() {
    static std::vector<BloqueAsmBajado> r;
    return r;
}

/**
 * @brief Cuenta las instrucciones de un cuerpo de asm.
 *
 * Cuenta lineas con contenido que no sean solo una etiqueta ni un comentario.
 * Es una cuenta del FUENTE, no del codigo generado: una instruccion puede bajar
 * a varias operaciones IR, y decir lo contrario seria inventar.
 */
uint32_t contar_instrucciones(const std::string &cuerpo) {
    uint32_t n = 0;
    size_t   i = 0;
    while (i <= cuerpo.size()) {
        const size_t fin = cuerpo.find('\n', i);
        const std::string linea =
            cuerpo.substr(i, (fin == std::string::npos ? cuerpo.size() : fin) - i);
        i = (fin == std::string::npos) ? cuerpo.size() + 1 : fin + 1;

        size_t a = 0;
        while (a < linea.size() && (linea[a] == ' ' || linea[a] == '\t')) ++a;
        if (a >= linea.size()) continue;                       // vacia
        if (linea[a] == ';' || linea[a] == '#') continue;      // comentario
        if (linea.compare(a, 2, "//") == 0) continue;          // comentario
        size_t b = linea.size();
        while (b > a && (linea[b - 1] == ' ' || linea[b - 1] == '\t' ||
                         linea[b - 1] == '\r'))
            --b;
        if (b > a && linea[b - 1] == ':') continue; // solo una etiqueta
        ++n;
    }
    return n;
}

} // namespace

const char *nombre_destino_asm(DestinoAsm d) {
    switch (d) {
    case DestinoAsm::ElevadoAIr: return "elevado a IR";
    case DestinoAsm::MicroAsm: return "micro asm";
    default: return "sin elevar";
    }
}

void anotar_bloque_asm(const std::string &funcion, uint32_t linea,
                       const std::string &cuerpo, DestinoAsm destino) {
    BloqueAsmBajado b;
    b.funcion = funcion;
    b.linea = linea;
    b.instrucciones = contar_instrucciones(cuerpo);
    b.destino = destino;
    std::lock_guard<std::mutex> lk(cerrojo());
    registro().push_back(std::move(b));
}

std::vector<BloqueAsmBajado> bloques_asm_bajados() {
    std::lock_guard<std::mutex> lk(cerrojo());
    return registro();
}

void olvidar_bloques_asm() {
    std::lock_guard<std::mutex> lk(cerrojo());
    registro().clear();
}

} // namespace vx
