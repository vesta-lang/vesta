/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */


/**
 * @file native_ffi.cpp
 * @brief Implementacion del sistema FFI de VestaVM.
 *
 * Implementa el internado de strings (intern_module, intern_function,
 * intern_signature), la carga de modulos nativos (load_native_module)
 * y la resolucion de simbolos y aplicacion de patches en el bytecode
 * (resolve_native_symbol, resolve_all).
 */
#include "ffi/native_ffi.h"

#ifdef _WIN32
#include "windows.h"
#else
#include <dlfcn.h>
#endif

namespace ffi {

    uint32_t FFI::intern_module(const std::string &name) {
        auto it = module_map.find(name);
        if (it != module_map.end())
            return it->second; /* ya existe: devolver indice almacenado */

        /* nuevo modulo: agregar al vector y registrar su indice */
        uint32_t idx     = static_cast<uint32_t>(modules.size());
        modules.push_back(name);
        module_map[name] = idx;
        return idx;
    }

    uint32_t FFI::intern_function(const std::string &name) {
        auto it = function_map.find(name);
        if (it != function_map.end())
            return it->second;

        uint32_t idx       = static_cast<uint32_t>(functions.size());
        functions.push_back(name);
        function_map[name] = idx;
        return idx;
    }

    uint32_t FFI::intern_signature(const std::string &sig) {
        auto it = signature_map.find(sig);
        if (it != signature_map.end())
            return it->second;

        uint32_t idx        = static_cast<uint32_t>(signatures.size());
        signatures.push_back(sig);
        signature_map[sig]  = idx;
        return idx;
    }

    void *FFI::load_native_module(const std::string &name) {
        /* consultar cache antes de llamar al SO */
        auto it = native_modules.find(name);
        if (it != native_modules.end())
            return it->second;

#ifdef _WIN32
        HMODULE h = LoadLibraryA(name.c_str());
        if (!h) {
            DWORD err = GetLastError();
            throw FFIError(
                "FFI: No se pudo cargar la libreria '" + name +
                "' (LoadLibraryA fallo, codigo: " + std::to_string(err) + ")"
            );
        }
        native_modules[name] = static_cast<void *>(h);
        return static_cast<void *>(h);
#else
        void *h = dlopen(name.c_str(), RTLD_LAZY);
        if (!h) {
            const char *err = dlerror();
            throw FFIError(
                "FFI: No se pudo cargar la libreria '" + name +
                "' (dlopen fallo: " + std::string(err ? err : "error desconocido") + ")"
            );
        }
        native_modules[name] = h;
        return h;
#endif
    }

    void *FFI::resolve_native_symbol(void *module, const std::string &func) {
#ifdef _WIN32
        FARPROC p = GetProcAddress(static_cast<HMODULE>(module), func.c_str());
        if (!p) {
            DWORD err = GetLastError();
            throw FFIError(
                "FFI: No se pudo resolver el simbolo '" + func +
                "' (GetProcAddress fallo, codigo: " + std::to_string(err) + ")"
            );
        }
        return reinterpret_cast<void *>(p);
#else
        dlerror(); /* limpiar cualquier error previo de dlsym */
        void *p = dlsym(module, func.c_str());
        if (!p) {
            const char *err = dlerror();
            throw FFIError(
                "FFI: No se pudo resolver el simbolo '" + func +
                "' (dlsym fallo: " + std::string(err ? err : "error desconocido") + ")"
            );
        }
        return p;
#endif
    }

    void FFI::resolve_all(uint8_t *file_base, uint64_t offset_real_bytecode) {
        for (auto &[key, entry]: imports) {
            const std::string &mod_name  = modules[key.module_idx];
            const std::string &func_name = functions[key.function_idx];

            /* cargar modulo y resolver la funcion nativa */
            void *mod          = load_native_module(mod_name);
            void *fn           = resolve_native_symbol(mod, func_name);
            entry.resolved_ptr = fn;

            /* parchear cada instruccion CALLN que referencia este simbolo */
            for (uint32_t off : entry.patch_sites) {
                uint64_t real_offset = off + offset_real_bytecode;
                patch_call(file_base, static_cast<uint32_t>(real_offset), fn);
            }
        }
    }

} // namespace ffi
