/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file vel_text_model.cpp
 * @brief Implementacion de lo que un diagrama sabe leer de un texto @c .vel.
 *
 * Ver @c vx/diagram/vel_text_model.h para el motivo de que esto viva aparte
 * de los dos generadores.
 */

#include "vx/diagram/vel_text_model.h"

#include <cctype>

namespace vx {

std::string get_mnemonic(const std::string &line) {
    size_t i = 0;
    // Saltar la sangria: el mnemonico es la primera palabra util.
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
        ++i;
    size_t start = i;
    while (i < line.size() &&
           !std::isspace(static_cast<unsigned char>(line[i]))) {
        ++i;
    }
    return line.substr(start, i - start);
}

bool is_call_mnemonic(const std::string &mn) {
    return mn == "callvm" || mn == "callvmr" || mn == "calln" ||
           mn == "callni" || mn == "callvirt" || mn == "callm" ||
           mn == "callsuper" || mn == "callclosure" || mn == "callrawclosure" ||
           mn == "spawn" || mn == "spawnon" || mn == "spawnargs" ||
           mn == "rspawn" || mn == "loadmod";
}

bool is_fused_instr(const std::string &mn) {
    // Comparacion + salto en una sola instruccion, con y sin signo.  Llevan
    // sufijo de condicion (cmpjmp.jne, cmpjmpu.jae...), de ahi el prefijo.
    if (mn == "cmpjmp" || mn == "cmpjmpu") return true;
    if (mn.size() >= 7 && mn.compare(0, 7, "cmpjmp.") == 0) return true;
    if (mn.size() >= 8 && mn.compare(0, 8, "cmpjmpu.") == 0) return true;

    // Decremento + salto si no es cero: el bucle contador clasico.
    if (mn == "decjnz") return true;

    // Aritmetica de tres operandos: ahorra el 'mov' previo cuando el
    // asignador de registros no pudo fundir destino y primera fuente.
    if (mn == "adds3" || mn == "subs3" || mn == "muls3") return true;
    if (mn == "addu3" || mn == "subu3" || mn == "mulu3") return true;
    if (mn == "and3" || mn == "or3" || mn == "xor3") return true;

    // Carga con extension a cero: ahorra el 'mov rd, 0' previo, porque la
    // maquina virtual no lo hace sola al escribir bytes parciales.
    if (mn == "loadz" || mn == "loadzh") return true;

    // Extension de signo en una instruccion, en vez de mov + shl + sar.
    if (mn == "sext") return true;

    // Reserva de bloque y puntero al contenido de una vez.
    if (mn == "gcallocp") return true;

    // Mover y anular el origen: el traspaso de propiedad de un puntero.
    if (mn == "mvtake") return true;

    // Crear proceso copiando ya los argumentos del padre.
    if (mn == "spawnargs") return true;

    // Resolver el futuro y terminar el proceso a la vez.
    if (mn == "fulfillhlt") return true;

    return false;
}

bool is_label_line(const std::string &line, std::string &name_out) {
    size_t i = 0;
    // Saltar la sangria.
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
        ++i;
    // El nombre admite letras, digitos y guion bajo.
    size_t start = i;
    while (
        i < line.size() &&
        (std::isalnum(static_cast<unsigned char>(line[i])) || line[i] == '_')) {
        ++i;
    }
    if (i == start) return false;
    // Se admiten espacios entre el nombre y los dos puntos.
    size_t after_ident = i;
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
        ++i;
    if (i >= line.size() || line[i] != ':') return false;
    // Tras los dos puntos solo puede quedar espacio o un comentario.
    size_t j = i + 1;
    while (j < line.size()) {
        if (std::isspace(static_cast<unsigned char>(line[j]))) {
            ++j;
            continue;
        }
        if (line[j] == '/' && j + 1 < line.size() && line[j + 1] == '/') break;
        return false; // hay codigo tras ':' -> no es solo una etiqueta
    }
    name_out = line.substr(start, after_ident - start);
    return true;
}

std::string extract_abs_target(const std::string &line) {
    const std::string marker = "@Absolute(\"code.";
    auto pos = line.find(marker);
    if (pos == std::string::npos) return std::string();
    pos += marker.size();
    auto end = line.find('"', pos);
    if (end == std::string::npos) return std::string();
    return line.substr(pos, end - pos);
}

} // namespace vx
