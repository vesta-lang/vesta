/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file toolchain/toolchain.cpp
 * @brief Implementacion del driver de compilacion reutilizable (ver el header).
 */

#include "toolchain/toolchain.h"

#include <chrono>
#include <fstream>
#include <unordered_map>

#include "util/assembler_multiprocess.h" // asm_multi_process::run_worker
#include "vx/compiler.h"                  // vx::compile_vx_source / _project

// Algunos headers arrastrados (windows.h y utilidades vendored) definen ERROR
// y ERR como macros; colisionan con los enumeradores.  Se limpian aqui, tras
// los includes, para poder nombrar los enums con seguridad.
#ifdef ERROR
#undef ERROR
#endif
#ifdef ERR
#undef ERR
#endif

namespace vesta {
namespace tc {

namespace {

/// @brief Traduce el nivel de diagnostico del frontend al del toolchain.
DiagLevel map_level(vx::DiagLevel lvl) {
    switch (lvl) {
    case vx::DiagLevel::ERR: return DiagLevel::Error;
    case vx::DiagLevel::WARN: return DiagLevel::Warning;
    default: return DiagLevel::Note;
    }
}

/// @brief Copia los diagnosticos del @c CompileResult al vector de salida.
/// @return true si hay al menos un error.
bool collect_diags(const vx::CompileResult &res, std::vector<Diag> &out) {
    bool had_error = false;
    for (const auto &d : res.diagnostics.all()) {
        Diag e;
        e.level = map_level(d.level);
        e.line = d.loc.line;
        e.column = d.loc.column;
        e.message = d.message;
        e.file = d.loc.file;
        if (e.level == DiagLevel::Error)
            had_error = true;
        out.push_back(std::move(e));
    }
    return had_error;
}

/// @brief Deriva el prefijo de salida a partir del @c input si no se dio @c -o.
///        Quita la extension @c .vx (o cualquier extension) del nombre.
std::string derive_output(const std::string &input) {
    // Quedarse con todo hasta el ultimo '.', respetando separadores de ruta.
    size_t slash = input.find_last_of("/\\");
    size_t dot = input.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        return input.substr(0, dot);
    return input;
}

} // namespace

CompileResponse compile(const CompileRequest &req) {
    CompileResponse resp;

    if (req.input.empty()) {
        resp.message = "no se indico fichero de entrada";
        return resp;
    }
    if (req.mode == ExecMode::AOT) {
        // El emisor nativo AOT (PE/ELF, formatos exe/obj/...) todavia vive en
        // main.cpp; se portara a este driver sin cambiar la firma.  Hasta
        // entonces el consumidor debe usar el binario @c vm para AOT.
        resp.message =
            "el modo AOT (artefacto nativo) aun no esta disponible en el "
            "toolchain embebido; usa el binario 'vm -m aot' para compilar a "
            ".exe/.o/formatos nativos";
        return resp;
    }

    // 1) Leer el fuente del raiz (o usar el overlay del buffer en memoria).
    std::string source = req.source_overlay;
    if (source.empty() && !req.is_project) {
        std::ifstream ifs(req.input, std::ios::binary);
        if (!ifs.is_open()) {
            resp.message = "no se pudo abrir el fichero: " + req.input;
            return resp;
        }
        source.assign(std::istreambuf_iterator<char>(ifs),
                      std::istreambuf_iterator<char>());
    }

    // 2) Mapear la peticion a las opciones del frontend.
    vx::CompileOptions opts;
    opts.module_name = req.module_name.empty() ? "main" : req.module_name;
    opts.emit_debug = req.debug;
    opts.instrument_mode = req.instrument;
    // mode VM/JIT producen el mismo .velb (native_poo solo lo activa AOT, ya
    // descartado arriba).

    // 3) Frontend: .vx -> .vel (+ IR embebido) + diagnosticos.
    const auto t0 = std::chrono::steady_clock::now();
    vx::CompileResult cr;
    if (req.is_project) {
        std::unordered_map<std::string, std::string> overlay;
        const std::unordered_map<std::string, std::string> *ovl = nullptr;
        if (!req.source_overlay.empty()) {
            overlay[req.input] = req.source_overlay;
            ovl = &overlay;
        }
        const std::vector<std::string> *paths =
            req.search_paths.empty() ? nullptr : &req.search_paths;
        cr = vx::compile_vx_project(req.input, opts, ovl, paths);
    } else {
        cr = vx::compile_vx_source(source, req.input, opts);
    }
    const auto t1 = std::chrono::steady_clock::now();
    resp.frontend_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());

    // 4) Recolectar diagnosticos; abortar si hubo errores.
    if (collect_diags(cr, resp.diagnostics)) {
        resp.message = "la compilacion produjo errores";
        return resp;
    }

    // 5) Escribir el .vel intermedio junto al artefacto de salida.
    const std::string out_prefix =
        req.output.empty() ? derive_output(req.input) : req.output;
    const std::string vel_path = out_prefix + ".vel";
    {
        std::ofstream ofs(vel_path);
        if (!ofs.is_open()) {
            resp.message = "no se pudo escribir el .vel intermedio: " + vel_path;
            return resp;
        }
        if (opts.emit_debug)
            ofs << "// @file " << req.input << "\n";
        ofs << cr.vel_text;
    }

    // 6) Ensamblar + linkar el .vel -> .velb (embebe el IR en la seccion @ir).
    const int rc = asm_multi_process::run_worker(
        vel_path, out_prefix,
        /*skip_preprocessor=*/true,
        /*keep_labels=*/req.keep_labels,
        /*ir_section_bytes=*/&cr.ir_section_bytes,
        /*emit_map=*/req.emit_map);
    if (rc != 0) {
        resp.message = "el ensamblado/linkado del .velb fallo (run_worker)";
        return resp;
    }

    resp.ok = true;
    resp.output_path = out_prefix + ".velb";
    return resp;
}

} // namespace tc
} // namespace vesta
