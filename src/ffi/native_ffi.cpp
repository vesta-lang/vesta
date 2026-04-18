/*
 * VestaVM - Máquina Virtual Distribuida
 *
 * Copyright © 2026 David López.T (DesmonHak) (Castilla y León, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribución obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
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
            return it->second;

        uint32_t idx = modules.size();
        modules.push_back(name);
        module_map[name] = idx;
        return idx;
    }

    uint32_t FFI::intern_function(const std::string &name) {
        auto it = function_map.find(name);
        if (it != function_map.end())
            return it->second;

        uint32_t idx = functions.size();
        functions.push_back(name);
        function_map[name] = idx;
        return idx;
    }


    uint32_t FFI::intern_signature(const std::string &sig) {
        auto it = signature_map.find(sig);
        if (it != signature_map.end())
            return it->second;

        uint32_t idx = signatures.size();
        signatures.push_back(sig);
        signature_map[sig] = idx;
        return idx;
    }

    void *FFI::load_native_module(const std::string &name) {
        auto it = native_modules.find(name);
        if (it != native_modules.end())
            return it->second;

#ifdef _WIN32
        HMODULE h = LoadLibraryA(name.c_str());
        if (!h) {
            DWORD err = GetLastError();
            throw FFIError(
                "FFI: No se pudo cargar la librería '" + name +
                "' (LoadLibraryA falló, código: " + std::to_string(err) + ")"
            );
        }
        native_modules[name] = h;
        return h;
#else
        void *h = dlopen(name.c_str(), RTLD_LAZY);
        if (!h) {
            const char *err = dlerror();
            throw FFIError(
                "FFI: No se pudo cargar la librería '" + name +
                "' (dlopen falló: " + std::string(err ? err : "error desconocido") + ")"
            );
        }
        native_modules[name] = h;
        return h;
#endif
    }

    void *FFI::resolve_native_symbol(void *module, const std::string &func) {
#ifdef _WIN32
        FARPROC p = GetProcAddress((HMODULE) module, func.c_str());
        if (!p) {
            DWORD err = GetLastError();
            throw FFIError(
                "FFI: No se pudo resolver el símbolo '" + func +
                "' (GetProcAddress falló, código: " + std::to_string(err) + ")"
            );
        }
        return (void *) p;
#else
        dlerror(); // limpiar error previo
        void *p = dlsym(module, func.c_str());
        if (!p) {
            const char *err = dlerror();
            throw FFIError(
                "FFI: No se pudo resolver el símbolo '" + func +
                "' (dlsym falló: " + std::string(err) + ")"
            );
        }
        return p;
#endif
    }

    void FFI::resolve_all(uint8_t *file_base, uint64_t offset_real_bytecode) {
        for (auto &[key, entry]: imports) {
            const std::string &module   = modules[key.module_idx];
            const std::string &function = functions[key.function_idx];

            void *mod          = load_native_module(module);
            void *fn           = resolve_native_symbol(mod, function);
            entry.resolved_ptr = fn;

            // Parchear todos los CALL que apuntan a este símbolo
            for (uint32_t off: entry.patch_sites) {
                uint64_t real = off + offset_real_bytecode;
                patch_call(file_base, real, fn);
            }
        }
    }
}
