/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file comptime_artifact.cpp
 * @brief Implementacion del artefacto comptime por contenido (ver el header).
 */

#include "vx/comptime/comptime_artifact.h"

#include "toolchain/toolchain.h"

#include <filesystem>
#include <fstream>
#include <iterator>

namespace vx {

ComptimeArtifact comptime_artifact_get(const ComptimeUnit &unit,
                                       const CasStore &cas,
                                       const std::string &work_dir) {
    ComptimeArtifact out;
    /* Sin conjunto no hay artefacto, y eso NO es un fallo: la mayoria de los
     * modulos no tienen nada comptime.  Se distingue de un error dejando
     * @c error vacio -- confundir "no hay nada que hacer" con "no se pudo" es
     * lo que hace que un fallo real pase desapercibido. */
    if (unit.empty() || unit.unit_source.empty()) return out;

    const MerkleKey key = unit.content_hash;
    if (key != 0 && cas.get(key, out.velb) && !out.velb.empty()) {
        out.ok = true;
        out.from_cache = true;
        return out;
    }

    std::error_code ec;
    std::filesystem::create_directories(work_dir, ec);

    /* Compilar el conjunto AISLADO.  Se le pasa por @c source_overlay: el texto
     * ya esta en memoria y escribirlo a un fichero para que la linea siguiente
     * lo relea seria trabajo puro.  @c quiet porque esto ocurre DENTRO de otra
     * compilacion y su salida no es la que el usuario pidio. */
    vesta::tc::CompileRequest req;
    const std::string prefix =
        (std::filesystem::path(work_dir) / ("ct_" + std::to_string(key)))
            .string();
    req.input = prefix + ".vx"; // nombre logico para los diagnosticos
    req.source_overlay = unit.unit_source;
    req.output = prefix;
    req.is_project = false;
    req.quiet = true;

    const vesta::tc::CompileResponse resp = vesta::tc::compile(req);
    out.frontend_us = resp.frontend_us;
    if (!resp.ok) {
        /* Que el conjunto no compile significa que NO es auto-suficiente, o sea
         * un fallo del recolector.  Se propaga con el primer diagnostico
         * concreto en vez de un "no se pudo": sin el, esto se depura leyendo el
         * codigo en vez del mensaje. */
        out.error = "el conjunto comptime no compila por si solo";
        if (!resp.message.empty()) out.error += ": " + resp.message;
        else if (!resp.diagnostics.empty())
            out.error += ": " + resp.diagnostics.front().message;
        return out;
    }

    std::ifstream f(resp.output_path, std::ios::binary);
    out.velb.assign(std::istreambuf_iterator<char>(f),
                    std::istreambuf_iterator<char>());
    if (out.velb.empty()) {
        out.error = "la compilacion del conjunto comptime no dejo bytecode en " +
                    resp.output_path;
        return out;
    }

    /* Guardar es idempotente y atomico (temp + rename), asi que dos procesos
     * compilando el mismo conjunto a la vez no se pisan.  Un fallo al guardar NO
     * invalida el artefacto que ya tenemos en la mano: se pierde el ahorro de la
     * proxima vez, no esta compilacion. */
    if (key != 0) (void)cas.put(key, out.velb);

    out.ok = true;
    out.from_cache = false;
    return out;
}

} // namespace vx
