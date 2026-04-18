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

namespace ffi{
    uint32_t FFI::intern_module(const std::string & name) {
        auto it = module_map.find(name);
        if (it != module_map.end())
            return it->second;

        uint32_t idx = modules.size();
        modules.push_back(name);
        module_map[name] = idx;
        return idx;

    }

    uint32_t FFI::intern_function(const std::string & name) {
        auto it = function_map.find(name);
        if (it != function_map.end())
            return it->second;

        uint32_t idx = functions.size();
        functions.push_back(name);
        function_map[name] = idx;
        return idx;

    }


    uint32_t FFI::intern_signature(const std::string & sig) {
        auto it = signature_map.find(sig);
        if (it != signature_map.end())
            return it->second;

        uint32_t idx = signatures.size();
        signatures.push_back(sig);
        signature_map[sig] = idx;
        return idx;

    }
}
