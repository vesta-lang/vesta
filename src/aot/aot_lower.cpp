/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 */

/**
 * @file aot/aot_lower.cpp
 * @brief  AOT.2 -- impl del pase de re-bajada LIBC_MAPPED -> CALL libc.
 */

#include "aot/aot_lower.h"
#include "ir/ssa_ir.h"

#include <unordered_map>

namespace aot {

void aot_lower_runtime(ir::IrModule &mod, const AotLowerConfig &cfg) {
    for (auto &fn : mod.functions) {
        // Mapa vid -> op que lo define (para detectar raw_free de un ALLOCA:
        // memoria de pila que NO debe liberarse -> evita free de un puntero
        // colgante / crash nativo).
        std::unordered_map<ir::IrValueId, ir::IrOp> def_op;
        for (auto &bb : fn.blocks)
            for (auto &in : bb.instrs)
                if (in.dst != ir::IR_NO_VALUE) def_op[in.dst] = in.op;

        for (auto &bb : fn.blocks) {
            for (auto &in : bb.instrs) {
                switch (in.op) {
                case ir::IrOp::RAW_ALLOC:
                    // %dst = raw_alloc.ptr %size  ->  %dst = call
                    // <alloc>(%size) <alloc> = simbolo externo (convencion
                    // "malloc"), lo resuelve el linker (libc, kernel o
                    // override).
                    in.op = ir::IrOp::CALL;
                    in.func_name = cfg.alloc_sym;
                    break;

                case ir::IrOp::RAW_FREE: {
                    // raw_free %ptr  ->  call free(%ptr), salvo que %ptr:
                    //   (a) provenga de un ALLOCA (pila): NOP (liberar pila =
                    //       dangling/crash);
                    //   (b) sea un valor COLGANTE -- sin definicion en la fn y
                    //       que NO es un parametro.  Esto pasa cuando un pase
                    //       (scalar-replace / dead-alloc-elim) ELIMINA el objeto
                    //       pero deja su `raw_free` referenciando un SSA value
                    //       ya inexistente.  El interp/JIT/PE lo toleran, pero
                    //       `free()` de libc aborta ("invalid pointer", visto en
                    //       callvirt_hot via gcc).  NOPearlo es correcto: el
                    //       objeto ya no existe, no hay nada que liberar.
                    bool nop = in.operands.empty();
                    if (!in.operands.empty()) {
                        const ir::IrValueId p = in.operands[0];
                        auto it = def_op.find(p);
                        if (it != def_op.end()) {
                            if (it->second == ir::IrOp::ALLOCA) nop = true;
                        } else {
                            // sin def: param (heap legitimo pasado) -> free;
                            // colgante (objeto eliminado) -> NOP.
                            bool is_param = false;
                            for (ir::IrValueId pv : fn.params)
                                if (pv == p) {
                                    is_param = true;
                                    break;
                                }
                            if (!is_param) nop = true;
                        }
                    }
                    if (nop) {
                        in.op = ir::IrOp::NOP;
                        in.operands.clear();
                        in.func_name.clear();
                        in.dst = ir::IR_NO_VALUE;
                    } else {
                        in.op = ir::IrOp::CALL;
                        in.func_name = cfg.free_sym;
                    }
                    break;
                }

                case ir::IrOp::CALL:
                case ir::IrOp::TAILCALL:
                    // AOT.2.d: el `new` nativo emite calloc(1,size) para
                    // zero-init.  Con un @AllocatorOverride (incluido el slab
                    // vx_mem por defecto) lo reescribimos a alloc_sym(size)
                    // -- 1 arg, descartando el `count` (=1) -> el `new` usa el
                    // allocator override sin arrastrar calloc de libc.  El
                    // override debe zerificar (convencion kzalloc) para
                    // preservar el cero-init de los campos no escritos.  Cubre
                    // CALL y TAILCALL (un `new` con ctor trivial -> el
                    // optimizador promueve calloc a tail-call).
                    if (cfg.has_alloc_override && in.func_name == "calloc" &&
                        in.operands.size() == 2) {
                        in.func_name = cfg.alloc_sym;
                        const ir::IrValueId sz =
                            in.operands[1]; // (count, SIZE)
                        in.operands.clear();
                        in.operands.push_back(sz);
                    }
                    break;

                case ir::IrOp::CALLN:
                    // ffi_call: CALLN "__callni__:" con operands=[fn_ptr,args..]
                    // -> CALLIND (llamada INDIRECTA nativa que vreg_select baja
                    // a `call reg`): func_ptr = operands[0], args = el resto.
                    // El resto de CALLN (extern "lib" fn) pasa sin cambios.
                    if (in.func_name.compare(0, 11, "__callni__:") == 0 &&
                        !in.operands.empty()) {
                        in.op = ir::IrOp::CALLIND;
                        in.func_ptr = in.operands[0];
                        in.operands.erase(in.operands.begin());
                        in.func_name.clear();
                    }
                    break;

                case ir::IrOp::DLOPEN:
                    // ffi_open: dlopen %path_addr, %path_len  ->  call
                    // __vx_dlopen(%path_addr).  La funcion Vesta (vx_ffi.vx)
                    // usa LoadLibraryA/dlopen segun @Target.  El path es una
                    // cstring NUL-terminada (el frontend la NUL-termina).  Se
                    // descarta %path_len (las APIs nativas leen hasta el NUL).
                    in.op = ir::IrOp::CALL;
                    in.func_name = cfg.dlopen_sym;
                    if (!in.operands.empty())
                        in.operands.resize(1); // [path_addr]
                    break;

                case ir::IrOp::DLSYM:
                    // ffi_sym: dlsym %handle, %name_addr, %name_len  ->  call
                    // __vx_dlsym(%handle, %name_addr).  Descarta %name_len.
                    in.op = ir::IrOp::CALL;
                    in.func_name = cfg.dlsym_sym;
                    if (in.operands.size() > 2)
                        in.operands.resize(2); // [handle, name_addr]
                    break;

                case ir::IrOp::PANIC:
                    // panic(msg,len) -> call <panic>(...).  Con un
                    // @PanicHandler (panic_takes_msg) se le pasa
                    // (msg_addr, len); sin el (default abort) se
                    // descarta el mensaje (abort no toma argumentos).
                    in.op = ir::IrOp::CALL;
                    in.func_name = cfg.panic_sym;
                    if (!cfg.panic_takes_msg) in.operands.clear();
                    in.dst = ir::IR_NO_VALUE;
                    break;

                default: break;
                }
            }
        }
    }
}

} // namespace aot
