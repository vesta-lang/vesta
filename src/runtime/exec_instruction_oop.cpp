/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (c) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 */

/**
 * @file exec_instruction_oop.cpp
 * @brief Implementacion de instrucciones del sistema de objetos (0x00 0xD0..0xDC).
 *
 * Convencion de punteros en registros:
 *   - Todos los punteros a ClassInfo*, MethodInfo*, FieldInfo* y al ObjectHeader
 *     de un objeto son PUNTEROS HOST (uint64_t casteado) almacenados en registros
 *     de proposito general.
 *   - Para objetos GC: se debe hacer GCDEREF primero para obtener el host_ptr
 *     al ObjectHeader, y luego usar ese puntero con CALLVIRT/THROW/etc.
 *   - Para objetos raw (NEWOBJRAW): el puntero al ObjectHeader se obtiene
 *     directamente de R00 tras la instruccion.
 */

#include "runtime/exec_instruction.h"
#include "runtime/proceso_runtime.h"
#include "gc/gc_heap.h"
#include "gc/raw_allocator.h"
#include "loader/oop_types.h"

namespace runtime {

    // =========================================================================
    //  Helpers internos
    // =========================================================================

    /** Comprueba si obj_class es igual a target o hereda de el (BFS sobre supers
     *  e interfaces). Recursivo pero las jerarquias suelen ser cortas. */
    static bool is_instance_of(const loader::ClassInfo *obj_class,
                                const loader::ClassInfo *target) {
        if (obj_class == nullptr || target == nullptr) return false;
        if (obj_class == target) return true;

        for (size_t i = 0; i < obj_class->super_count; ++i) {
            if (is_instance_of(obj_class->supers[i], target)) return true;
        }
        for (size_t i = 0; i < obj_class->interface_count; ++i) {
            if (is_instance_of(obj_class->interfaces[i], target)) return true;
        }
        return false;
    }

    /** Implementacion comun de THROW/RETHROW: recorre la cadena de frames
     *  buscando un handler compatible con exception_class.
     *  Si encuentra uno: restaura frame_stack, pone el puntero en R00 y salta.
     *  Si no: mata el proceso con EVT_ERROR. */
    static void do_throw(ProcessVM *vm, uint64_t exception_ptr) {
        if (exception_ptr == 0) {
            vm->err_thread = THREAD_SEGMENTATION_FAULT;
            vm->scheduler.on_event(EVT_ERROR);
            return;
        }

        auto *exc_hdr = reinterpret_cast<loader::ObjectHeader *>(exception_ptr);
        loader::ClassInfo *exc_class = exc_hdr->class_ptr;

        // Guardamos la excepcion activa para RETHROW
        vm->current_exception = exception_ptr;

        loader::FrameHeader *frame = vm->frame_stack;

        while (frame != nullptr) {
            loader::MethodInfo *method = frame->method;
            if (method == nullptr) {
                frame = frame->prev;
                continue;
            }

            // Offset relativo al inicio del metodo
            uint64_t pc_offset = vm->registers.rip.raw() - method->code_vaddr;

            for (size_t i = 0; i < method->handler_count; ++i) {
                const loader::HandlerException &h = method->handlers[i];
                if (pc_offset < h.start_pc || pc_offset >= h.end_pc) continue;

                bool matches = (h.type == nullptr) // catch-all / finally
                             || is_instance_of(exc_class, h.type);

                if (matches) {
                    // Desapilamos frames hasta el handler
                    loader::FrameHeader *cur = vm->frame_stack;
                    while (cur != nullptr && cur != frame) {
                        loader::FrameHeader *tmp = cur;
                        cur = cur->prev;
                        delete tmp;
                    }
                    vm->frame_stack = frame;

                    // Convencion: R00 = puntero al objeto excepcion
                    vm->registers.regs[R00].qword(exception_ptr);

                    // Saltar al handler
                    vm->registers.rip.qword(method->code_vaddr + h.handler_pc);
                    vm->decoded_ptr->flags_info.did_jump = true;
                    return;
                }
            }

            // Subir un nivel
            loader::FrameHeader *done = frame;
            frame = frame->prev;
            delete done;
        }

        // No se encontro handler -> excepcion no capturada
        vm->frame_stack = nullptr;
        vm->scheduler.on_event(EVT_ERROR);
    }

    // =========================================================================
    //  0xD0 NEWOBJRAW reg_classinfo, reg_size -> R0 = host_ptr
    // =========================================================================
    void exec_instr_newobjraw(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_cls = instr.data_instruction.reg_data.reg1;
        const uint8_t r_sz  = instr.data_instruction.reg_data.reg2;

        auto *cls = reinterpret_cast<loader::ClassInfo *>(vm->registers.regs[r_cls].qword());
        if (cls == nullptr) {
            vm->registers.regs[R00].qword(0);
            return;
        }

        size_t alloc_size = (r_sz != 0)
            ? static_cast<size_t>(vm->registers.regs[r_sz].qword())
            : static_cast<size_t>(cls->instance_size);

        uint64_t ptr = vm->raw_alloc.alloc(alloc_size);
        if (ptr != 0) {
            auto *hdr      = reinterpret_cast<loader::ObjectHeader *>(ptr);
            hdr->class_ptr = cls;
            hdr->flags     = loader::OBJ_FLAG_RAW_OWNED;
            hdr->hash_code = static_cast<uint32_t>(ptr & 0xFFFFFFFF);
        }

        vm->registers.regs[R00].qword(ptr);
    }

    // =========================================================================
    //  0xD1 CALLVIRT reg_obj, vtable_idx
    // =========================================================================
    void exec_instr_callvirt(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_obj    = instr.data_instruction.reg_data.reg1;
        const uint8_t vtbl_idx = instr.data_instruction.reg_data.reg2;

        uint64_t obj_ptr = vm->registers.regs[r_obj].qword();
        if (obj_ptr == 0) {
            vm->err_thread = THREAD_SEGMENTATION_FAULT;
            vm->scheduler.on_event(EVT_ERROR);
            return;
        }

        auto *hdr = reinterpret_cast<loader::ObjectHeader *>(obj_ptr);
        loader::ClassInfo *cls = hdr->class_ptr;

        if (cls == nullptr || vtbl_idx >= cls->vtable_size || cls->vtable == nullptr) {
            vm->err_thread = THREAD_ILLEGAL_INSTRUCTION;
            vm->scheduler.on_event(EVT_ERROR);
            return;
        }

        loader::MethodInfo *method = cls->vtable[vtbl_idx];
        if (method == nullptr || method->code_vaddr == 0) {
            vm->err_thread = THREAD_ILLEGAL_INSTRUCTION;
            vm->scheduler.on_event(EVT_ERROR);
            return;
        }

        uint64_t ret_addr = vm->registers.rip.raw()
                            + static_cast<uint64_t>(instr.flags_info.size_instr);

        // Empujar FrameHeader (para THROW/RETHROW)
        auto *frame       = new loader::FrameHeader{};
        frame->prev       = vm->frame_stack;
        frame->method     = method;
        frame->return_pc  = ret_addr;
        frame->frame_base = vm->registers.stack_pointer.qword();
        vm->frame_stack   = frame;

        // Empujar ret_addr a la pila (para RET, igual que CALLVM)
        vm->registers.stack_pointer.qword(vm->registers.stack_pointer.qword() - 8);
        vm->vm_mem.write_bytes(vm->registers.stack_pointer.raw(), &ret_addr, 8);

        // Saltar al metodo
        vm->registers.rip.qword(method->code_vaddr);
        vm->decoded_ptr->flags_info.did_jump = true;
    }

    // =========================================================================
    //  0xD2 CALLSUPER reg_classinfo, vtable_idx
    // =========================================================================
    void exec_instr_callsuper(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_cls    = instr.data_instruction.reg_data.reg1;
        const uint8_t vtbl_idx = instr.data_instruction.reg_data.reg2;

        auto *cls = reinterpret_cast<loader::ClassInfo *>(vm->registers.regs[r_cls].qword());
        if (cls == nullptr || vtbl_idx >= cls->vtable_size || cls->vtable == nullptr) {
            vm->err_thread = THREAD_ILLEGAL_INSTRUCTION;
            vm->scheduler.on_event(EVT_ERROR);
            return;
        }

        loader::MethodInfo *method = cls->vtable[vtbl_idx];
        if (method == nullptr || method->code_vaddr == 0) {
            vm->err_thread = THREAD_ILLEGAL_INSTRUCTION;
            vm->scheduler.on_event(EVT_ERROR);
            return;
        }

        uint64_t ret_addr = vm->registers.rip.raw()
                            + static_cast<uint64_t>(instr.flags_info.size_instr);

        auto *frame       = new loader::FrameHeader{};
        frame->prev       = vm->frame_stack;
        frame->method     = method;
        frame->return_pc  = ret_addr;
        frame->frame_base = vm->registers.stack_pointer.qword();
        vm->frame_stack   = frame;

        // Empujar ret_addr a la pila (para RET, igual que CALLVM)
        vm->registers.stack_pointer.qword(vm->registers.stack_pointer.qword() - 8);
        vm->vm_mem.write_bytes(vm->registers.stack_pointer.raw(), &ret_addr, 8);

        vm->registers.rip.qword(method->code_vaddr);
        vm->decoded_ptr->flags_info.did_jump = true;
    }

    // =========================================================================
    //  0xD3 THROW reg_obj
    // =========================================================================
    void exec_instr_throw(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_obj = instr.data_instruction.reg_data.reg1;
        uint64_t exception_ptr = vm->registers.regs[r_obj].qword();
        do_throw(vm, exception_ptr);
    }

    // =========================================================================
    //  0xD4 RETHROW
    // =========================================================================
    void exec_instr_rethrow(ProcessVM *vm, const DecodedInstr &instr) {
        (void)instr;
        do_throw(vm, vm->current_exception);
    }

    // =========================================================================
    //  0xD5 GETCLASS reg_obj -> R0 = ClassInfo*
    // =========================================================================
    void exec_instr_getclass(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_obj = instr.data_instruction.reg_data.reg1;
        uint64_t obj_ptr = vm->registers.regs[r_obj].qword();

        if (obj_ptr == 0) {
            vm->registers.regs[R00].qword(0);
            return;
        }

        auto *hdr = reinterpret_cast<loader::ObjectHeader *>(obj_ptr);
        vm->registers.regs[R00].qword(reinterpret_cast<uint64_t>(hdr->class_ptr));
    }

    // =========================================================================
    //  0xD6 INSTANCEOF reg_obj, reg_classinfo -> R0 = bool
    // =========================================================================
    void exec_instr_instanceof(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_obj = instr.data_instruction.reg_data.reg1;
        const uint8_t r_cls = instr.data_instruction.reg_data.reg2;

        uint64_t obj_ptr = vm->registers.regs[r_obj].qword();
        auto *target_cls = reinterpret_cast<loader::ClassInfo *>(vm->registers.regs[r_cls].qword());

        if (obj_ptr == 0 || target_cls == nullptr) {
            vm->registers.regs[R00].qword(0);
            return;
        }

        auto *hdr = reinterpret_cast<loader::ObjectHeader *>(obj_ptr);
        bool result = is_instance_of(hdr->class_ptr, target_cls);
        vm->registers.regs[R00].qword(result ? 1ULL : 0ULL);
    }

    // =========================================================================
    //  0xD7 CHECKCAST reg_obj, reg_classinfo -> R0 = obj_ptr o THROW
    // =========================================================================
    void exec_instr_checkcast(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_obj = instr.data_instruction.reg_data.reg1;
        const uint8_t r_cls = instr.data_instruction.reg_data.reg2;

        uint64_t obj_ptr  = vm->registers.regs[r_obj].qword();
        auto *target_cls  = reinterpret_cast<loader::ClassInfo *>(vm->registers.regs[r_cls].qword());

        if (obj_ptr == 0) {
            // null siempre pasa el cast (convencion JVM-compatible)
            vm->registers.regs[R00].qword(0);
            return;
        }

        auto *hdr    = reinterpret_cast<loader::ObjectHeader *>(obj_ptr);
        bool ok      = is_instance_of(hdr->class_ptr, target_cls);

        if (ok) {
            vm->registers.regs[R00].qword(obj_ptr);
        } else {
            // ClassCastException: usamos el mecanismo de throw con el objeto actual
            // como excepcion sintetica (el objeto no es del tipo esperado).
            // En una implementacion completa se cre aria un ClassCastException real;
            // por ahora senalizamos un error de instruccion ilegal.
            vm->err_thread = THREAD_ILLEGAL_INSTRUCTION;
            vm->scheduler.on_event(EVT_ERROR);
        }
    }

    // =========================================================================
    //  0xD8 GETFIELD reg_classinfo, field_idx -> R0 = FieldInfo*
    // =========================================================================
    void exec_instr_getfield(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_cls    = instr.data_instruction.reg_data.reg1;
        const uint8_t field_idx = instr.data_instruction.reg_data.reg2;

        auto *cls = reinterpret_cast<loader::ClassInfo *>(vm->registers.regs[r_cls].qword());
        if (cls == nullptr || field_idx >= cls->field_count) {
            vm->registers.regs[R00].qword(0);
            return;
        }

        vm->registers.regs[R00].qword(
            reinterpret_cast<uint64_t>(&cls->fields[field_idx])
        );
    }

    // =========================================================================
    //  0xD9 GETMETHOD reg_classinfo, method_idx -> R0 = MethodInfo*
    // =========================================================================
    void exec_instr_getmethod(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_cls      = instr.data_instruction.reg_data.reg1;
        const uint8_t method_idx = instr.data_instruction.reg_data.reg2;

        auto *cls = reinterpret_cast<loader::ClassInfo *>(vm->registers.regs[r_cls].qword());
        if (cls == nullptr || method_idx >= cls->method_count) {
            vm->registers.regs[R00].qword(0);
            return;
        }

        vm->registers.regs[R00].qword(
            reinterpret_cast<uint64_t>(&cls->methods[method_idx])
        );
    }

    // =========================================================================
    //  0xDA FIELDCOUNT reg_classinfo -> R0 = uint64
    // =========================================================================
    void exec_instr_fieldcount(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_cls = instr.data_instruction.reg_data.reg1;
        auto *cls = reinterpret_cast<loader::ClassInfo *>(vm->registers.regs[r_cls].qword());

        vm->registers.regs[R00].qword(cls ? static_cast<uint64_t>(cls->field_count) : 0ULL);
    }

    // =========================================================================
    //  0xDB METHODCOUNT reg_classinfo -> R0 = uint64
    // =========================================================================
    void exec_instr_methodcount(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_cls = instr.data_instruction.reg_data.reg1;
        auto *cls = reinterpret_cast<loader::ClassInfo *>(vm->registers.regs[r_cls].qword());

        vm->registers.regs[R00].qword(cls ? static_cast<uint64_t>(cls->method_count) : 0ULL);
    }

    // =========================================================================
    //  0xDC CLASSNAME reg_classinfo -> R0 = char* (host ptr a name.data)
    // =========================================================================
    void exec_instr_classname(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_cls = instr.data_instruction.reg_data.reg1;
        auto *cls = reinterpret_cast<loader::ClassInfo *>(vm->registers.regs[r_cls].qword());

        if (cls == nullptr || cls->name.data == nullptr) {
            vm->registers.regs[R00].qword(0);
            return;
        }

        vm->registers.regs[R00].qword(reinterpret_cast<uint64_t>(cls->name.data));
    }

} // namespace runtime
