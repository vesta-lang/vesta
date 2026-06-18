/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 */

/**
 * @file aot/aot_lower.cpp
 * @brief Phase AOT.2 -- impl del pase de re-bajada LIBC_MAPPED -> CALL libc.
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
                    // raw_free %ptr  ->  call free(%ptr), salvo que %ptr
                    // provenga de un ALLOCA (pila): entonces NOP (no se
                    // libera la pila; liberar seria un dangling/crash).
                    bool is_stack = false;
                    if (!in.operands.empty()) {
                        auto it = def_op.find(in.operands[0]);
                        if (it != def_op.end() &&
                            it->second == ir::IrOp::ALLOCA)
                            is_stack = true;
                    }
                    if (is_stack) {
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
                    // zero-init.  Con un @AllocatorOverride lo reescribimos
                    // a alloc_sym(size) -- 1 arg, descartando el `count` (=1)
                    // -> en freestanding el `new` usa el allocator del
                    // usuario sin arrastrar calloc de libc.  El override
                    // debe zerificar (kzalloc) para preservar la semantica.
                    // Cubre tanto CALL como TAILCALL (un `new` cuyo ctor es
                    // trivial -> el optimizador promueve calloc a tail-call).
                    if (cfg.has_alloc_override && in.func_name == "calloc" &&
                        in.operands.size() == 2) {
                        in.func_name = cfg.alloc_sym;
                        const ir::IrValueId sz =
                            in.operands[1]; // (count, SIZE)
                        in.operands.clear();
                        in.operands.push_back(sz);
                    }
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
