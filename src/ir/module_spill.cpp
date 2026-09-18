/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file ir/module_spill.cpp
 * @brief Bajar los cuerpos de un modulo a disco y volver a traerlos.  El
 *        porque esta en la cabecera.
 */

#include "ir/module_spill.h"

#include "ir/ssa_ir.h"
#include "ir/ssa_ir_serialize.h"
#include "util/file_read.h"
#include "util/fs_utils.h"

namespace ir {

namespace {

/**
 * @brief Cuanto monton usa una cadena.
 *
 * Cero si cabe dentro del propio objeto.  El umbral de libstdc++ son quince
 * caracteres mas el terminador; se usa ese numero y no `capacity()` a secas
 * porque una cadena corta devuelve capacidad sin haber reservado nada.
 */
size_t string_heap(const std::string &s) {
    static constexpr size_t kSso = 15;
    return s.capacity() > kSso ? s.capacity() + 1 : 0;
}

/// Lo que ocupa un vector por su RESERVA, no por lo que lleva dentro.
template <typename T> size_t vector_heap(const std::vector<T> &v) {
    return v.capacity() * sizeof(T);
}

} // namespace

size_t functions_footprint(const std::vector<IrFunction> &fns) {
    size_t total = vector_heap(fns);
    for (const IrFunction &fn : fns) {
        total += string_heap(fn.name);
        total += vector_heap(fn.values);
        total += vector_heap(fn.blocks);
        total += vector_heap(fn.params);
        for (const IrValue &v : fn.values)
            total += string_heap(v.name);
        for (const IrBlock &b : fn.blocks) {
            total += string_heap(b.name);
            total += vector_heap(b.instrs);
            total += vector_heap(b.preds);
            total += vector_heap(b.succs);
            for (const IrInstr &in : b.instrs) {
                total += string_heap(in.func_name);
                total += vector_heap(in.call_abi_regs);
                total += vector_heap(in.phi_args);
                total += vector_heap(in.jump_targets);
                for (const std::string &r : in.call_abi_regs)
                    total += string_heap(r);
            }
        }
    }
    return total;
}

bool spill_functions(IrModule &mod, const std::string &path,
                     bool already_written) {
    if (!already_written) {
        const std::vector<uint8_t> bytes = emit_ir_module_cache(mod);
        /* Si no se puede escribir NO se suelta nada.  Quedarse sin memoria es
         * un problema; perder el programa para ahorrarla es otro, y peor. */
        if (!fs::write_file_atomic(path, bytes)) return false;
    }
    /* `swap` y no `clear`: hay que devolver la reserva, que es justo lo que se
     * venia a soltar.  `clear` deja el vector vacio y la memoria cogida. */
    std::vector<IrFunction>().swap(mod.functions);
    return true;
}

bool restore_functions(IrModule &mod, const std::string &path, size_t expected,
                       std::string &err) {
    std::vector<uint8_t> bytes;
    if (!util::read_whole_file(path, bytes)) {
        err = "no se pudo leer " + path;
        return false;
    }
    /* A un modulo APARTE, y de el solo se mueven los cuerpos: `parse` reescribe
     * tambien `globals` y `static_data`, y esos nunca salieron de la RAM -- los
     * de aqui son los buenos. */
    IrModule parsed;
    if (!parse_ir_module_cache(bytes, parsed)) {
        err = "el intermedio de " + path + " no se pudo interpretar";
        return false;
    }
    if (parsed.functions.size() != expected) {
        /* Se DICE.  Un cuerpo que falta no revienta al fundir: revienta mucho
         * despues, como un simbolo sin resolver, o no revienta y sale otro
         * programa. */
        err = "de " + path + " vuelven " +
              std::to_string(parsed.functions.size()) +
              " funciones y bajaron " + std::to_string(expected);
        return false;
    }
    mod.functions = std::move(parsed.functions);
    return true;
}

} // namespace ir
