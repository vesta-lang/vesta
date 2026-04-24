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
 * @file exec_instruction_oop.cpp
 * @brief Implementacion de instrucciones del sistema de objetos (0x00 0xD0..0xEB).
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

    /**
     * @brief Comprueba si @p obj_class es igual a @p target o hereda de el.
     *
     * Realiza una busqueda BFS recursiva sobre la cadena de superclases e
     * interfaces.  La recursion es aceptable porque las jerarquias suelen
     * ser cortas en la practica.
     *
     * @param obj_class Clase del objeto a comprobar.
     * @param target    Clase objetivo de la comprobacion.
     * @return true si obj_class es target o hereda de el.
     */
    static bool is_instance_of(const loader::ClassInfo *obj_class,
                                const loader::ClassInfo *target) {
        if (obj_class == nullptr || target == nullptr) return false; // punteros nulos -> falso
        if (obj_class == target) return true;                        // coincidencia directa

        // comprobar superclases
        for (size_t i = 0; i < obj_class->super_count; ++i) {
            if (is_instance_of(obj_class->supers[i], target)) return true;
        }
        // comprobar interfaces implementadas
        for (size_t i = 0; i < obj_class->interface_count; ++i) {
            if (is_instance_of(obj_class->interfaces[i], target)) return true;
        }
        return false; // no encontrado en toda la jerarquia
    }

    /**
     * @brief Implementacion comun de THROW y RETHROW.
     *
     * Recorre la cadena de frames buscando un handler compatible con la clase
     * de la excepcion lanzada:
     *   - Si encuentra handler: restaura frame_stack, pone el puntero en R00 y salta.
     *   - Si no encuentra handler: mata el proceso con EVT_ERROR.
     *
     * @param vm            Proceso virtual sobre el que se lanza la excepcion.
     * @param exception_ptr Puntero host al ObjectHeader de la excepcion (0 = invalido).
     */
    static void do_throw(ProcessVM *vm, uint64_t exception_ptr) {
        if (exception_ptr == 0) {
            // excepcion nula -> error de segmentacion
            vm->err_thread = THREAD_SEGMENTATION_FAULT;
            vm->scheduler.on_event(EVT_ERROR);
            return;
        }

        auto *exc_hdr = reinterpret_cast<loader::ObjectHeader *>(exception_ptr);
        loader::ClassInfo *exc_class = exc_hdr->class_ptr; // clase de la excepcion

        // guardar la excepcion activa para que RETHROW pueda relanzarla
        vm->current_exception = exception_ptr;

        loader::FrameHeader *frame = vm->frame_stack; // comenzar desde el frame actual

        while (frame != nullptr) {
            loader::MethodInfo *method = frame->method;
            if (method == nullptr) {
                frame = frame->prev; // frame sin metodo, subir al siguiente
                continue;
            }

            // calcular el offset del PC respecto al inicio del metodo
            uint64_t pc_offset = vm->registers.rip.raw() - method->code_vaddr;

            for (size_t i = 0; i < method->handler_count; ++i) {
                const loader::HandlerException &h = method->handlers[i];

                // verificar que el PC esta dentro del rango cubierto por el handler
                if (pc_offset < h.start_pc || pc_offset >= h.end_pc) continue;

                // verificar compatibilidad de tipo (nullptr = catch-all / finally)
                bool matches = (h.type == nullptr)
                             || is_instance_of(exc_class, h.type);

                if (matches) {
                    // desapilar todos los frames intermedios hasta el handler
                    loader::FrameHeader *cur = vm->frame_stack;
                    while (cur != nullptr && cur != frame) {
                        loader::FrameHeader *tmp = cur;
                        cur = cur->prev;
                        delete tmp; // liberar cada frame intermedio
                    }
                    vm->frame_stack = frame; // restaurar el frame del handler

                    // convencion: R00 contiene el puntero al objeto excepcion
                    vm->registers.regs[R00].qword(exception_ptr);

                    // saltar al handler dentro del metodo
                    vm->registers.rip.qword(method->code_vaddr + h.handler_pc);
                    vm->decoded_ptr->flags_info.did_jump = true; // notificar el salto
                    return;
                }
            }

            // handler no encontrado en este frame: subir al anterior
            loader::FrameHeader *done = frame;
            frame = frame->prev;
            delete done; // liberar el frame que acaba de ser descartado
        }

        // excepcion no capturada en ningun frame: matar el proceso
        vm->frame_stack = nullptr;
        vm->scheduler.on_event(EVT_ERROR);
    }

    // =========================================================================
    //  0xD0 NEWOBJRAW reg_classinfo, reg_size -> R0 = host_ptr
    // =========================================================================

    /**
     * @brief Ejecuta la instruccion NEWOBJRAW: aloca un objeto raw sin GC.
     *
     * Aloca un bloque de memoria bruta usando el raw_allocator del proceso.
     * El tamano se toma del segundo registro si es distinto de cero; de lo
     * contrario se usa instance_size de la clase.  El ObjectHeader se inicializa
     * con OBJ_FLAG_RAW_OWNED para distinguirlo de los objetos GC.
     *
     * @param vm    Proceso virtual que ejecuta NEWOBJRAW.
     * @param instr Instruccion descodificada con reg_data.reg1 (ClassInfo*) y reg_data.reg2 (size).
     */
    void exec_instr_newobjraw(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_cls = instr.data_instruction.reg_data.reg1; // registro con puntero a ClassInfo
        const uint8_t r_sz  = instr.data_instruction.reg_data.reg2; // registro con tamano (0 = usar instance_size)

        auto *cls = reinterpret_cast<loader::ClassInfo *>(vm->registers.regs[r_cls].qword());
        if (cls == nullptr) {
            vm->registers.regs[R00].qword(0); // clase nula -> puntero nulo
            return;
        }

        // si r_sz != 0, usar el tamano del registro; si no, usar el de la clase
        size_t alloc_size = (r_sz != 0)
            ? static_cast<size_t>(vm->registers.regs[r_sz].qword())
            : static_cast<size_t>(cls->instance_size);

        uint64_t ptr = vm->raw_alloc.alloc(alloc_size); // alocar con el allocator bruto
        if (ptr != 0) {
            auto *hdr      = reinterpret_cast<loader::ObjectHeader *>(ptr);
            hdr->class_ptr = cls;                                          // enlazar la clase
            hdr->flags     = loader::OBJ_FLAG_RAW_OWNED;                   // marcar como raw (no GC)
            hdr->hash_code = static_cast<uint32_t>(ptr & 0xFFFFFFFF);     // hash basado en la direccion
        }

        vm->registers.regs[R00].qword(ptr); // devolver el puntero host en R00
    }

    // =========================================================================
    //  0xD1 CALLVIRT reg_obj, vtable_idx
    // =========================================================================

    /**
     * @brief Ejecuta la instruccion CALLVIRT: llamada virtual a un metodo de objeto.
     *
     * Resuelve el metodo en tiempo de ejecucion a traves de la vtable de la clase
     * del objeto.  Empuja un FrameHeader en frame_stack para que THROW/RETHROW
     * puedan localizar handlers, y tambien empuja la direccion de retorno en la
     * pila convencional (compatible con RET).
     *
     * @param vm    Proceso virtual que ejecuta CALLVIRT.
     * @param instr Instruccion descodificada con reg_data.reg1 (objeto) y reg_data.reg2 (vtable_idx).
     */
    void exec_instr_callvirt(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_obj    = instr.data_instruction.reg_data.reg1; // registro con el puntero al objeto
        const uint8_t vtbl_idx = instr.data_instruction.reg_data.reg2; // indice en la vtable

        uint64_t obj_ptr = vm->registers.regs[r_obj].qword();
        if (obj_ptr == 0) {
            // objeto nulo -> error de segmentacion
            vm->err_thread = THREAD_SEGMENTATION_FAULT;
            vm->scheduler.on_event(EVT_ERROR);
            return;
        }

        auto *hdr = reinterpret_cast<loader::ObjectHeader *>(obj_ptr);
        loader::ClassInfo *cls = hdr->class_ptr; // obtener la clase del objeto

        if (cls == nullptr || vtbl_idx >= cls->vtable_size || cls->vtable == nullptr) {
            // clase invalida o indice fuera de rango -> error de instruccion ilegal
            vm->err_thread = THREAD_ILLEGAL_INSTRUCTION;
            vm->scheduler.on_event(EVT_ERROR);
            return;
        }

        loader::MethodInfo *method = cls->vtable[vtbl_idx]; // resolver el metodo en la vtable
        if (method == nullptr || method->code_vaddr == 0) {
            // metodo abstracto o no implementado
            vm->err_thread = THREAD_ILLEGAL_INSTRUCTION;
            vm->scheduler.on_event(EVT_ERROR);
            return;
        }

        uint64_t ret_addr = vm->registers.rip.raw()
                            + static_cast<uint64_t>(instr.flags_info.size_instr); // PC de retorno

        // crear y empujar el FrameHeader para el soporte de excepciones
        auto *frame       = new loader::FrameHeader{};
        frame->prev       = vm->frame_stack;                        // encadenar con el frame anterior
        frame->method     = method;                                  // metodo en ejecucion
        frame->return_pc  = ret_addr;                                // PC de retorno
        frame->frame_base = vm->registers.stack_pointer.qword();    // base del frame en la pila
        vm->frame_stack   = frame;                                   // actualizar la cadena de frames

        // empujar la direccion de retorno en la pila convencional (para RET)
        vm->registers.stack_pointer.qword(vm->registers.stack_pointer.qword() - 8);
        vm->vm_mem.write_bytes(vm->registers.stack_pointer.raw(), &ret_addr, 8);

        // saltar al cuerpo del metodo
        vm->registers.rip.qword(method->code_vaddr);
        vm->decoded_ptr->flags_info.did_jump = true; // notificar el salto
    }

    // =========================================================================
    //  0xD2 CALLSUPER reg_classinfo, vtable_idx
    // =========================================================================

    /**
     * @brief Ejecuta la instruccion CALLSUPER: llamada a un metodo de la superclase.
     *
     * Similar a CALLVIRT pero resuelve el metodo en la vtable de la clase
     * indicada por el registro (no la del objeto receptor), permitiendo
     * llamadas super() explicitas.
     *
     * @param vm    Proceso virtual que ejecuta CALLSUPER.
     * @param instr Instruccion descodificada con reg_data.reg1 (ClassInfo*) y reg_data.reg2 (vtable_idx).
     */
    void exec_instr_callsuper(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_cls    = instr.data_instruction.reg_data.reg1; // registro con puntero a ClassInfo
        const uint8_t vtbl_idx = instr.data_instruction.reg_data.reg2; // indice en la vtable

        auto *cls = reinterpret_cast<loader::ClassInfo *>(vm->registers.regs[r_cls].qword());
        if (cls == nullptr || vtbl_idx >= cls->vtable_size || cls->vtable == nullptr) {
            vm->err_thread = THREAD_ILLEGAL_INSTRUCTION;
            vm->scheduler.on_event(EVT_ERROR);
            return;
        }

        loader::MethodInfo *method = cls->vtable[vtbl_idx]; // resolver metodo en la clase indicada
        if (method == nullptr || method->code_vaddr == 0) {
            vm->err_thread = THREAD_ILLEGAL_INSTRUCTION;
            vm->scheduler.on_event(EVT_ERROR);
            return;
        }

        uint64_t ret_addr = vm->registers.rip.raw()
                            + static_cast<uint64_t>(instr.flags_info.size_instr);

        // crear FrameHeader para soporte de excepciones
        auto *frame       = new loader::FrameHeader{};
        frame->prev       = vm->frame_stack;
        frame->method     = method;
        frame->return_pc  = ret_addr;
        frame->frame_base = vm->registers.stack_pointer.qword();
        vm->frame_stack   = frame;

        // empujar la direccion de retorno en la pila convencional
        vm->registers.stack_pointer.qword(vm->registers.stack_pointer.qword() - 8);
        vm->vm_mem.write_bytes(vm->registers.stack_pointer.raw(), &ret_addr, 8);

        // saltar al metodo de la superclase
        vm->registers.rip.qword(method->code_vaddr);
        vm->decoded_ptr->flags_info.did_jump = true;
    }

    // =========================================================================
    //  0xD3 THROW reg_obj
    // =========================================================================

    /**
     * @brief Ejecuta la instruccion THROW: lanza una excepcion con el objeto indicado.
     *
     * @param vm    Proceso virtual que ejecuta THROW.
     * @param instr Instruccion descodificada con reg_data.reg1 (objeto excepcion).
     */
    void exec_instr_throw(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_obj = instr.data_instruction.reg_data.reg1; // registro con el objeto excepcion
        uint64_t exception_ptr = vm->registers.regs[r_obj].qword();
        do_throw(vm, exception_ptr); // delegar en el helper comun
    }

    // =========================================================================
    //  0xD4 RETHROW
    // =========================================================================

    /**
     * @brief Ejecuta la instruccion RETHROW: relanza la excepcion activa.
     *
     * Usa el campo current_exception del proceso para relanzar la ultima excepcion
     * capturada, permitiendo propagacion desde handlers catch.
     *
     * @param vm    Proceso virtual que ejecuta RETHROW.
     * @param instr Instruccion descodificada (no se usan sus campos).
     */
    void exec_instr_rethrow(ProcessVM *vm, const DecodedInstr &instr) {
        (void)instr; // sin operandos
        do_throw(vm, vm->current_exception); // relanzar la excepcion activa
    }

    // =========================================================================
    //  0xD5 GETCLASS reg_obj -> R0 = ClassInfo*
    // =========================================================================

    /**
     * @brief Ejecuta la instruccion GETCLASS: obtiene el puntero a la clase de un objeto.
     *
     * Lee el ObjectHeader del objeto y devuelve en R00 el puntero host a su ClassInfo.
     * Si el objeto es nulo devuelve 0.
     *
     * @param vm    Proceso virtual que ejecuta GETCLASS.
     * @param instr Instruccion descodificada con reg_data.reg1 (objeto).
     */
    void exec_instr_getclass(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_obj = instr.data_instruction.reg_data.reg1;
        uint64_t obj_ptr = vm->registers.regs[r_obj].qword();

        if (obj_ptr == 0) {
            vm->registers.regs[R00].qword(0); // objeto nulo -> ClassInfo nula
            return;
        }

        auto *hdr = reinterpret_cast<loader::ObjectHeader *>(obj_ptr);
        vm->registers.regs[R00].qword(reinterpret_cast<uint64_t>(hdr->class_ptr));
    }

    // =========================================================================
    //  0xD6 INSTANCEOF reg_obj, reg_classinfo -> R0 = bool
    // =========================================================================

    /**
     * @brief Ejecuta la instruccion INSTANCEOF: comprueba si un objeto es de una clase.
     *
     * Devuelve 1 en R00 si el objeto es instancia de la clase indicada o de una
     * subclase de ella; 0 en caso contrario o si alguno de los operandos es nulo.
     *
     * @param vm    Proceso virtual que ejecuta INSTANCEOF.
     * @param instr Instruccion descodificada con reg_data.reg1 (obj) y reg_data.reg2 (ClassInfo*).
     */
    void exec_instr_instanceof(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_obj = instr.data_instruction.reg_data.reg1;
        const uint8_t r_cls = instr.data_instruction.reg_data.reg2;

        uint64_t obj_ptr  = vm->registers.regs[r_obj].qword();
        auto *target_cls  = reinterpret_cast<loader::ClassInfo *>(vm->registers.regs[r_cls].qword());

        if (obj_ptr == 0 || target_cls == nullptr) {
            vm->registers.regs[R00].qword(0); // operandos nulos -> falso
            return;
        }

        auto *hdr    = reinterpret_cast<loader::ObjectHeader *>(obj_ptr);
        bool result  = is_instance_of(hdr->class_ptr, target_cls); // comprobar jerarquia
        vm->registers.regs[R00].qword(result ? 1ULL : 0ULL);
    }

    // =========================================================================
    //  0xD7 CHECKCAST reg_obj, reg_classinfo -> R0 = obj_ptr o THROW
    // =========================================================================

    /**
     * @brief Ejecuta la instruccion CHECKCAST: verifica y proyecta el tipo de un objeto.
     *
     * Si el objeto es compatible con la clase indicada devuelve el puntero en R00.
     * Si no es compatible notifica EVT_ERROR con THREAD_ILLEGAL_INSTRUCTION.
     * El puntero nulo siempre supera la comprobacion (convencion JVM).
     *
     * @param vm    Proceso virtual que ejecuta CHECKCAST.
     * @param instr Instruccion descodificada con reg_data.reg1 (obj) y reg_data.reg2 (ClassInfo*).
     */
    void exec_instr_checkcast(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_obj = instr.data_instruction.reg_data.reg1;
        const uint8_t r_cls = instr.data_instruction.reg_data.reg2;

        uint64_t obj_ptr = vm->registers.regs[r_obj].qword();
        auto *target_cls = reinterpret_cast<loader::ClassInfo *>(vm->registers.regs[r_cls].qword());

        if (obj_ptr == 0) {
            // null siempre supera el cast (convencion JVM-compatible)
            vm->registers.regs[R00].qword(0);
            return;
        }

        auto *hdr = reinterpret_cast<loader::ObjectHeader *>(obj_ptr);
        bool ok   = is_instance_of(hdr->class_ptr, target_cls);

        if (ok) {
            vm->registers.regs[R00].qword(obj_ptr); // cast correcto: devolver el puntero
        } else {
            // tipo incompatible: senalizar ClassCastException
            vm->err_thread = THREAD_ILLEGAL_INSTRUCTION;
            vm->scheduler.on_event(EVT_ERROR);
        }
    }

    // =========================================================================
    //  0xD8 GETFIELD reg_classinfo, field_idx -> R0 = FieldInfo*
    // =========================================================================

    /**
     * @brief Ejecuta la instruccion GETFIELD: obtiene el puntero a un descriptor de campo.
     *
     * Devuelve en R00 el puntero host a FieldInfo en el array fields[] de la clase.
     * Devuelve 0 si la clase es nula o el indice esta fuera de rango.
     *
     * @param vm    Proceso virtual que ejecuta GETFIELD.
     * @param instr Instruccion descodificada con reg_data.reg1 (ClassInfo*) y reg_data.reg2 (field_idx).
     */
    void exec_instr_getfield(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_cls     = instr.data_instruction.reg_data.reg1; // ClassInfo*
        const uint8_t field_idx = instr.data_instruction.reg_data.reg2; // indice del campo

        auto *cls = reinterpret_cast<loader::ClassInfo *>(vm->registers.regs[r_cls].qword());
        if (cls == nullptr || field_idx >= cls->field_count) {
            vm->registers.regs[R00].qword(0); // indice invalido -> puntero nulo
            return;
        }

        vm->registers.regs[R00].qword(reinterpret_cast<uint64_t>(&cls->fields[field_idx]));
    }

    // =========================================================================
    //  0xD9 GETMETHOD reg_classinfo, method_idx -> R0 = MethodInfo*
    // =========================================================================

    /**
     * @brief Ejecuta la instruccion GETMETHOD: obtiene el puntero a un descriptor de metodo.
     *
     * Devuelve en R00 el puntero host a MethodInfo en el array methods[] de la clase.
     * Devuelve 0 si la clase es nula o el indice esta fuera de rango.
     *
     * @param vm    Proceso virtual que ejecuta GETMETHOD.
     * @param instr Instruccion descodificada con reg_data.reg1 (ClassInfo*) y reg_data.reg2 (method_idx).
     */
    void exec_instr_getmethod(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_cls      = instr.data_instruction.reg_data.reg1;
        const uint8_t method_idx = instr.data_instruction.reg_data.reg2;

        auto *cls = reinterpret_cast<loader::ClassInfo *>(vm->registers.regs[r_cls].qword());
        if (cls == nullptr || method_idx >= cls->method_count) {
            vm->registers.regs[R00].qword(0);
            return;
        }

        vm->registers.regs[R00].qword(reinterpret_cast<uint64_t>(&cls->methods[method_idx]));
    }

    // =========================================================================
    //  0xDA FIELDCOUNT reg_classinfo -> R0 = uint64
    // =========================================================================

    /**
     * @brief Ejecuta la instruccion FIELDCOUNT: devuelve el numero de campos de una clase.
     *
     * @param vm    Proceso virtual que ejecuta FIELDCOUNT.
     * @param instr Instruccion descodificada con reg_data.reg1 (ClassInfo*).
     */
    void exec_instr_fieldcount(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_cls = instr.data_instruction.reg_data.reg1;
        auto *cls = reinterpret_cast<loader::ClassInfo *>(vm->registers.regs[r_cls].qword());

        vm->registers.regs[R00].qword(cls ? static_cast<uint64_t>(cls->field_count) : 0ULL);
    }

    // =========================================================================
    //  0xDB METHODCOUNT reg_classinfo -> R0 = uint64
    // =========================================================================

    /**
     * @brief Ejecuta la instruccion METHODCOUNT: devuelve el numero de metodos de una clase.
     *
     * @param vm    Proceso virtual que ejecuta METHODCOUNT.
     * @param instr Instruccion descodificada con reg_data.reg1 (ClassInfo*).
     */
    void exec_instr_methodcount(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_cls = instr.data_instruction.reg_data.reg1;
        auto *cls = reinterpret_cast<loader::ClassInfo *>(vm->registers.regs[r_cls].qword());

        vm->registers.regs[R00].qword(cls ? static_cast<uint64_t>(cls->method_count) : 0ULL);
    }

    // =========================================================================
    //  0xDC CLASSNAME reg_classinfo -> R0 = char* (host ptr a name.data)
    // =========================================================================

    /**
     * @brief Ejecuta la instruccion CLASSNAME: devuelve el nombre de una clase.
     *
     * Escribe en R00 el puntero host al buffer de caracteres del nombre de la clase.
     * Devuelve 0 si la clase es nula o su nombre no tiene datos.
     *
     * @param vm    Proceso virtual que ejecuta CLASSNAME.
     * @param instr Instruccion descodificada con reg_data.reg1 (ClassInfo*).
     */
    void exec_instr_classname(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_cls = instr.data_instruction.reg_data.reg1;
        auto *cls = reinterpret_cast<loader::ClassInfo *>(vm->registers.regs[r_cls].qword());

        if (cls == nullptr || cls->name.data == nullptr) {
            vm->registers.regs[R00].qword(0); // clase sin nombre -> puntero nulo
            return;
        }

        vm->registers.regs[R00].qword(reinterpret_cast<uint64_t>(cls->name.data));
    }

    // =========================================================================
    //  Helpers para acceso a doc/attrs
    // =========================================================================

    /**
     * @brief Devuelve el puntero host al buffer de una cadena stringx.
     * @param s Referencia a la cadena stringx.
     * @return Puntero host al buffer de caracteres, o 0 si es nulo.
     */
    static inline uint64_t str_ptr(const loader::stringx &s) {
        return reinterpret_cast<uint64_t>(s.data); // puntero al buffer de la cadena
    }

    /**
     * @brief Devuelve el puntero host a la clave del atributo indicado.
     * @tparam T Tipo que contiene attrs[] y attr_count (ClassInfo, MethodInfo, FieldInfo).
     * @param obj Puntero al objeto con atributos.
     * @param idx Indice del atributo.
     * @return Puntero host a la clave, o 0 si es invalido.
     */
    template<typename T>
    static inline uint64_t attr_key(T *obj, uint8_t idx) {
        if (obj == nullptr || idx >= obj->attr_count || obj->attrs == nullptr)
            return 0; // parametros invalidos
        return reinterpret_cast<uint64_t>(obj->attrs[idx].key.data);
    }

    /**
     * @brief Devuelve el puntero host al valor del atributo indicado.
     * @tparam T Tipo que contiene attrs[] y attr_count (ClassInfo, MethodInfo, FieldInfo).
     * @param obj Puntero al objeto con atributos.
     * @param idx Indice del atributo.
     * @return Puntero host al valor, o 0 si es invalido.
     */
    template<typename T>
    static inline uint64_t attr_val(T *obj, uint8_t idx) {
        if (obj == nullptr || idx >= obj->attr_count || obj->attrs == nullptr)
            return 0; // parametros invalidos
        return reinterpret_cast<uint64_t>(obj->attrs[idx].value.data);
    }

    // =========================================================================
    //  0xDD CLASSDOC -> R0 = char* (host ptr a doc.data)
    // =========================================================================

    /**
     * @brief Ejecuta CLASSDOC: devuelve el puntero al texto de documentacion de la clase.
     * @param vm    Proceso virtual que ejecuta CLASSDOC.
     * @param instr Instruccion descodificada con reg_data.reg1 (ClassInfo*).
     */
    void exec_instr_classdoc(ProcessVM *vm, const DecodedInstr &instr) {
        auto *cls = reinterpret_cast<loader::ClassInfo *>(
            vm->registers.regs[instr.data_instruction.reg_data.reg1].qword());
        vm->registers.regs[R00].qword(cls ? str_ptr(cls->doc) : 0ULL);
    }

    // =========================================================================
    //  0xDE CLASSATTRCOUNT -> R0 = uint64
    // =========================================================================

    /**
     * @brief Ejecuta CLASSATTRCOUNT: devuelve el numero de atributos de una clase.
     * @param vm    Proceso virtual que ejecuta CLASSATTRCOUNT.
     * @param instr Instruccion descodificada con reg_data.reg1 (ClassInfo*).
     */
    void exec_instr_classattrcount(ProcessVM *vm, const DecodedInstr &instr) {
        auto *cls = reinterpret_cast<loader::ClassInfo *>(
            vm->registers.regs[instr.data_instruction.reg_data.reg1].qword());
        vm->registers.regs[R00].qword(cls ? static_cast<uint64_t>(cls->attr_count) : 0ULL);
    }

    // =========================================================================
    //  0xDF CLASSATTRKEY -> R0 = char* (key del atributo)
    // =========================================================================

    /**
     * @brief Ejecuta CLASSATTRKEY: devuelve la clave del atributo indicado de una clase.
     * @param vm    Proceso virtual que ejecuta CLASSATTRKEY.
     * @param instr Instruccion descodificada con reg_data.reg1 (ClassInfo*) y reg_data.reg2 (idx).
     */
    void exec_instr_classattrkey(ProcessVM *vm, const DecodedInstr &instr) {
        auto *cls = reinterpret_cast<loader::ClassInfo *>(
            vm->registers.regs[instr.data_instruction.reg_data.reg1].qword());
        vm->registers.regs[R00].qword(attr_key(cls, instr.data_instruction.reg_data.reg2));
    }

    // =========================================================================
    //  0xE0 CLASSATTRVAL -> R0 = char* (valor del atributo)
    // =========================================================================

    /**
     * @brief Ejecuta CLASSATTRVAL: devuelve el valor del atributo indicado de una clase.
     * @param vm    Proceso virtual que ejecuta CLASSATTRVAL.
     * @param instr Instruccion descodificada con reg_data.reg1 (ClassInfo*) y reg_data.reg2 (idx).
     */
    void exec_instr_classattrval(ProcessVM *vm, const DecodedInstr &instr) {
        auto *cls = reinterpret_cast<loader::ClassInfo *>(
            vm->registers.regs[instr.data_instruction.reg_data.reg1].qword());
        vm->registers.regs[R00].qword(attr_val(cls, instr.data_instruction.reg_data.reg2));
    }

    // =========================================================================
    //  0xE1 METHODNAME -> R0 = char* (nombre del metodo)
    // =========================================================================

    /**
     * @brief Ejecuta METHODNAME: devuelve el puntero al nombre de un MethodInfo.
     * @param vm    Proceso virtual que ejecuta METHODNAME.
     * @param instr Instruccion descodificada con reg_data.reg1 (MethodInfo*).
     */
    void exec_instr_methodname(ProcessVM *vm, const DecodedInstr &instr) {
        auto *m = reinterpret_cast<loader::MethodInfo *>(
            vm->registers.regs[instr.data_instruction.reg_data.reg1].qword());
        vm->registers.regs[R00].qword(m ? str_ptr(m->name) : 0ULL);
    }

    // =========================================================================
    //  0xE2 METHODDOC -> R0 = char*
    // =========================================================================

    /**
     * @brief Ejecuta METHODDOC: devuelve el puntero a la documentacion de un metodo.
     * @param vm    Proceso virtual que ejecuta METHODDOC.
     * @param instr Instruccion descodificada con reg_data.reg1 (MethodInfo*).
     */
    void exec_instr_methoddoc(ProcessVM *vm, const DecodedInstr &instr) {
        auto *m = reinterpret_cast<loader::MethodInfo *>(
            vm->registers.regs[instr.data_instruction.reg_data.reg1].qword());
        vm->registers.regs[R00].qword(m ? str_ptr(m->doc) : 0ULL);
    }

    // =========================================================================
    //  0xE3 METHODDESC -> R0 = char*
    // =========================================================================

    /**
     * @brief Ejecuta METHODDESC: devuelve el puntero al descriptor de tipo de un metodo.
     * @param vm    Proceso virtual que ejecuta METHODDESC.
     * @param instr Instruccion descodificada con reg_data.reg1 (MethodInfo*).
     */
    void exec_instr_methoddesc(ProcessVM *vm, const DecodedInstr &instr) {
        auto *m = reinterpret_cast<loader::MethodInfo *>(
            vm->registers.regs[instr.data_instruction.reg_data.reg1].qword());
        vm->registers.regs[R00].qword(m ? str_ptr(m->descriptor) : 0ULL);
    }

    // =========================================================================
    //  0xE4 METHODATTRCOUNT -> R0 = uint64
    // =========================================================================

    /**
     * @brief Ejecuta METHODATTRCOUNT: devuelve el numero de atributos de un metodo.
     * @param vm    Proceso virtual que ejecuta METHODATTRCOUNT.
     * @param instr Instruccion descodificada con reg_data.reg1 (MethodInfo*).
     */
    void exec_instr_methodattrcount(ProcessVM *vm, const DecodedInstr &instr) {
        auto *m = reinterpret_cast<loader::MethodInfo *>(
            vm->registers.regs[instr.data_instruction.reg_data.reg1].qword());
        vm->registers.regs[R00].qword(m ? static_cast<uint64_t>(m->attr_count) : 0ULL);
    }

    // =========================================================================
    //  0xE5 METHODATTRKEY -> R0 = char*
    // =========================================================================

    /**
     * @brief Ejecuta METHODATTRKEY: devuelve la clave del atributo indicado de un metodo.
     * @param vm    Proceso virtual que ejecuta METHODATTRKEY.
     * @param instr Instruccion descodificada con reg_data.reg1 (MethodInfo*) y reg_data.reg2 (idx).
     */
    void exec_instr_methodattrkey(ProcessVM *vm, const DecodedInstr &instr) {
        auto *m = reinterpret_cast<loader::MethodInfo *>(
            vm->registers.regs[instr.data_instruction.reg_data.reg1].qword());
        vm->registers.regs[R00].qword(attr_key(m, instr.data_instruction.reg_data.reg2));
    }

    // =========================================================================
    //  0xE6 METHODATTRVAL -> R0 = char*
    // =========================================================================

    /**
     * @brief Ejecuta METHODATTRVAL: devuelve el valor del atributo indicado de un metodo.
     * @param vm    Proceso virtual que ejecuta METHODATTRVAL.
     * @param instr Instruccion descodificada con reg_data.reg1 (MethodInfo*) y reg_data.reg2 (idx).
     */
    void exec_instr_methodattrval(ProcessVM *vm, const DecodedInstr &instr) {
        auto *m = reinterpret_cast<loader::MethodInfo *>(
            vm->registers.regs[instr.data_instruction.reg_data.reg1].qword());
        vm->registers.regs[R00].qword(attr_val(m, instr.data_instruction.reg_data.reg2));
    }

    // =========================================================================
    //  0xE7 FIELDNAME -> R0 = char*
    // =========================================================================

    /**
     * @brief Ejecuta FIELDNAME: devuelve el puntero al nombre de un FieldInfo.
     * @param vm    Proceso virtual que ejecuta FIELDNAME.
     * @param instr Instruccion descodificada con reg_data.reg1 (FieldInfo*).
     */
    void exec_instr_fieldname(ProcessVM *vm, const DecodedInstr &instr) {
        auto *f = reinterpret_cast<loader::FieldInfo *>(
            vm->registers.regs[instr.data_instruction.reg_data.reg1].qword());
        vm->registers.regs[R00].qword(f ? str_ptr(f->name) : 0ULL);
    }

    // =========================================================================
    //  0xE8 FIELDDOC -> R0 = char*
    // =========================================================================

    /**
     * @brief Ejecuta FIELDDOC: devuelve el puntero a la documentacion de un campo.
     * @param vm    Proceso virtual que ejecuta FIELDDOC.
     * @param instr Instruccion descodificada con reg_data.reg1 (FieldInfo*).
     */
    void exec_instr_fielddoc(ProcessVM *vm, const DecodedInstr &instr) {
        auto *f = reinterpret_cast<loader::FieldInfo *>(
            vm->registers.regs[instr.data_instruction.reg_data.reg1].qword());
        vm->registers.regs[R00].qword(f ? str_ptr(f->doc) : 0ULL);
    }

    // =========================================================================
    //  0xE9 FIELDATTRCOUNT -> R0 = uint64
    // =========================================================================

    /**
     * @brief Ejecuta FIELDATTRCOUNT: devuelve el numero de atributos de un campo.
     * @param vm    Proceso virtual que ejecuta FIELDATTRCOUNT.
     * @param instr Instruccion descodificada con reg_data.reg1 (FieldInfo*).
     */
    void exec_instr_fieldattrcount(ProcessVM *vm, const DecodedInstr &instr) {
        auto *f = reinterpret_cast<loader::FieldInfo *>(
            vm->registers.regs[instr.data_instruction.reg_data.reg1].qword());
        vm->registers.regs[R00].qword(f ? static_cast<uint64_t>(f->attr_count) : 0ULL);
    }

    // =========================================================================
    //  0xEA FIELDATTRKEY -> R0 = char*
    // =========================================================================

    /**
     * @brief Ejecuta FIELDATTRKEY: devuelve la clave del atributo indicado de un campo.
     * @param vm    Proceso virtual que ejecuta FIELDATTRKEY.
     * @param instr Instruccion descodificada con reg_data.reg1 (FieldInfo*) y reg_data.reg2 (idx).
     */
    void exec_instr_fieldattrkey(ProcessVM *vm, const DecodedInstr &instr) {
        auto *f = reinterpret_cast<loader::FieldInfo *>(
            vm->registers.regs[instr.data_instruction.reg_data.reg1].qword());
        vm->registers.regs[R00].qword(attr_key(f, instr.data_instruction.reg_data.reg2));
    }

    // =========================================================================
    //  0xEB FIELDATTRVAL -> R0 = char*
    // =========================================================================

    /**
     * @brief Ejecuta FIELDATTRVAL: devuelve el valor del atributo indicado de un campo.
     * @param vm    Proceso virtual que ejecuta FIELDATTRVAL.
     * @param instr Instruccion descodificada con reg_data.reg1 (FieldInfo*) y reg_data.reg2 (idx).
     */
    void exec_instr_fieldattrval(ProcessVM *vm, const DecodedInstr &instr) {
        auto *f = reinterpret_cast<loader::FieldInfo *>(
            vm->registers.regs[instr.data_instruction.reg_data.reg1].qword());
        vm->registers.regs[R00].qword(attr_val(f, instr.data_instruction.reg_data.reg2));
    }

} // namespace runtime
