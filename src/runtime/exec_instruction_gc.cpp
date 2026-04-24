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
 * @file exec_instruction_gc.cpp
 * @brief Implementacion de las instrucciones de gestion de memoria del GC de VestaVM.
 *
 * Implementa las instrucciones ALLOC, FREE, REALLOC, NEWOBJ, DROP, GCRUN,
 * GCWB, GCCONFIG, GCALLOC, GCDEREF y las operaciones de cursor (READCUR, WRITECUR).
 */#include "runtime/exec_instruction.h"
#include "runtime/proceso_runtime.h"
#include "gc/gc_heap.h"
#include "gc/raw_allocator.h"
#include "loader/oop_types.h"

namespace runtime {

    // -------------------------------------------------------------------------
    // Cursor - acceso a memoria real del host - opcodes 0x00 0xC0 .. 0xC2
    // Los registros cursor (CUR0-CUR3) contienen punteros host directos.
    // -------------------------------------------------------------------------

    /**
     * @brief Ejecuta la instruccion READCUR: lee de la memoria host apuntada por un cursor.
     *
     * Obtiene la direccion host almacenada en el registro cursor indicado y lee
     * 1, 2, 4 u 8 bytes segun el modo codificado.  El resultado se almacena en
     * el registro general destino extendido a 64 bits.
     *
     * @param vm    Proceso virtual que ejecuta READCUR.
     * @param instr Instruccion descodificada con reg_data.reg2 (cursor) y reg_data.reg1 (destino).
     */
    void exec_instr_readcur(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  cur_idx = instr.data_instruction.reg_data.reg2;    // indice del cursor (0-3)
        const uint8_t  dst     = instr.data_instruction.reg_data.reg1;    // registro general destino
        const uint64_t addr    = vm->registers.cur[cur_idx].qword();      // puntero host del cursor

        // leer segun el modo: 8, 16, 32 o 64 bits
        switch (instr.flags_info.mode) {
            case 0b00: vm->registers.regs[dst].qword(*reinterpret_cast<const uint8_t  *>(addr)); break; //  8 bits
            case 0b01: vm->registers.regs[dst].qword(*reinterpret_cast<const uint16_t *>(addr)); break; // 16 bits
            case 0b10: vm->registers.regs[dst].qword(*reinterpret_cast<const uint32_t *>(addr)); break; // 32 bits
            default:   vm->registers.regs[dst].qword(*reinterpret_cast<const uint64_t *>(addr)); break; // 64 bits
        }
    }

    /**
     * @brief Ejecuta la instruccion WRITECUR: escribe en la memoria host apuntada por un cursor.
     *
     * Obtiene la direccion host del cursor indicado y escribe el valor del registro
     * fuente truncado al tamano indicado por el modo (1, 2, 4 u 8 bytes).
     *
     * @param vm    Proceso virtual que ejecuta WRITECUR.
     * @param instr Instruccion descodificada con reg_data.reg2 (cursor) y reg_data.reg1 (fuente).
     */
    void exec_instr_writecur(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  cur_idx = instr.data_instruction.reg_data.reg2;  // indice del cursor (0-3)
        const uint8_t  src     = instr.data_instruction.reg_data.reg1;  // registro general fuente
        const uint64_t addr    = vm->registers.cur[cur_idx].qword();    // puntero host del cursor
        const uint64_t val     = vm->registers.regs[src].qword();       // valor a escribir

        // escribir segun el modo: 8, 16, 32 o 64 bits
        switch (instr.flags_info.mode) {
            case 0b00: *reinterpret_cast<uint8_t  *>(addr) = static_cast<uint8_t >(val); break; //  8 bits
            case 0b01: *reinterpret_cast<uint16_t *>(addr) = static_cast<uint16_t>(val); break; // 16 bits
            case 0b10: *reinterpret_cast<uint32_t *>(addr) = static_cast<uint32_t>(val); break; // 32 bits
            default:   *reinterpret_cast<uint64_t *>(addr) = val;                        break; // 64 bits
        }
    }

    /**
     * @brief Ejecuta la instruccion GCDEREF: desreferencia un handle GC en un cursor.
     *
     * Convierte el handle GC almacenado en el registro indicado en un puntero
     * host al payload del objeto y lo escribe en el registro cursor destino.
     * El cursor queda listo para accesos directos con READCUR/WRITECUR.
     *
     * @param vm    Proceso virtual que ejecuta GCDEREF.
     * @param instr Instruccion descodificada con reg_data.reg2 (cursor) y reg_data.reg1 (handle).
     */
    void exec_instr_gcderef(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  cur_idx    = instr.data_instruction.reg_data.reg2;          // cursor destino (0-3)
        const uint8_t  handle_reg = instr.data_instruction.reg_data.reg1;          // registro con el handle GC
        const uint64_t raw_handle = vm->registers.regs[handle_reg].qword();       // valor del handle

        // desreferenciar el handle para obtener el puntero host al payload
        uint8_t *payload = vm->gc_heap.deref(static_cast<gc::GcHandle>(raw_handle));
        vm->registers.cur[cur_idx].qword(reinterpret_cast<uint64_t>(payload));    // guardar puntero en cursor
    }

    // -------------------------------------------------------------------------
    // GC generacional - opcodes 0x00 0xA0 .. 0xA5
    // -------------------------------------------------------------------------

    /**
     * @brief Ejecuta la instruccion NEWOBJ: aloca un objeto gestionado por el GC.
     *
     * Lee el puntero a ClassInfo del registro indicado y aloca instance_size bytes
     * en el heap GC.  Si la aloca tiene exito, inicializa el ObjectHeader con el
     * puntero a la clase, el flag GC_OWNED y el handle como hash inicial.
     * El handle resultante (o GC_NULL_HANDLE en caso de fallo) se escribe en R00.
     *
     * @param vm    Proceso virtual que ejecuta NEWOBJ.
     * @param instr Instruccion descodificada con reg_data.reg1 (registro con ClassInfo*).
     */
    void exec_instr_newobj(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t r_cls = instr.data_instruction.reg_data.reg1; // registro con el puntero ClassInfo
        auto *cls = reinterpret_cast<loader::ClassInfo *>(vm->registers.regs[r_cls].qword());

        if (cls == nullptr) {
            // clase nula: devolver handle invalido
            vm->registers.regs[R00].qword(static_cast<uint64_t>(gc::GC_NULL_HANDLE));
            return;
        }

        gc::GcHandle h = vm->gc_heap.alloc(static_cast<size_t>(cls->instance_size)); // alocar en el heap GC

        if (h != gc::GC_NULL_HANDLE) {
            uint8_t *payload = vm->gc_heap.deref(h); // obtener puntero host al payload
            if (payload != nullptr) {
                auto *hdr      = reinterpret_cast<loader::ObjectHeader *>(payload);
                hdr->class_ptr = cls;                                     // enlazar la clase
                hdr->flags     = loader::OBJ_FLAG_GC_OWNED;               // marcar como gestionado por GC
                hdr->hash_code = static_cast<uint32_t>(h);                // usar el handle como hash inicial
            }
        }

        vm->registers.regs[R00].qword(static_cast<uint64_t>(h)); // devolver el handle en R00
    }

    /**
     * @brief Ejecuta la instruccion GCRUN: lanza un ciclo de GC menor.
     *
     * Fuerza la recoleccion de basura de la generacion joven.  Puede llamarse
     * desde bytecode para controlar la presion de memoria en puntos conocidos.
     *
     * @param vm    Proceso virtual que ejecuta GCRUN.
     * @param instr Instruccion descodificada (no se usan sus campos).
     */
    void exec_instr_gcrun(ProcessVM *vm, const DecodedInstr &instr) {
        (void)instr;         // instruccion sin operandos
        vm->gc_heap.minor_gc(); // ejecutar el GC menor
    }

    /**
     * @brief Ejecuta la instruccion GCCONFIG: configura el umbral de promocion del GC.
     *
     * Lee el valor del umbral desde el registro fuente segun el modo y llama a
     * set_old_threshold() para ajustar el limite de bytes tras el cual los objetos
     * se promueven a la generacion antigua.
     *
     * @param vm    Proceso virtual que ejecuta GCCONFIG.
     * @param instr Instruccion descodificada con reg_data.reg1 (fuente) y mode.
     */
    void exec_instr_gcconfig(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  rsrc      = instr.data_instruction.reg_data.reg1; // registro con el nuevo umbral
        const uint64_t threshold = read_reg_table[instr.flags_info.mode](vm, rsrc); // leer segun modo
        vm->gc_heap.set_old_threshold(static_cast<size_t>(threshold)); // aplicar la configuracion
    }

    /**
     * @brief Ejecuta la instruccion GCDROP: libera un objeto del heap GC por handle.
     *
     * Decrementa el contador de referencias o libera el objeto segun la politica
     * del GC.  El handle se lee del registro fuente segun el modo.
     *
     * @param vm    Proceso virtual que ejecuta GCDROP.
     * @param instr Instruccion descodificada con reg_data.reg1 (fuente) y mode.
     */
    void exec_instr_gc_drop(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  rsrc   = instr.data_instruction.reg_data.reg1; // registro con el handle a liberar
        const uint64_t handle = read_reg_table[instr.flags_info.mode](vm, rsrc);
        vm->gc_heap.drop(static_cast<gc::GcHandle>(handle)); // liberar el handle en el GC
    }

    /**
     * @brief Ejecuta la instruccion GCWB: notifica al GC una escritura (write barrier).
     *
     * Debe llamarse antes de sobrescribir un puntero GC para que el GC pueda
     * mantener la consistencia de sus estructuras de rastreo generacional.
     *
     * @param vm    Proceso virtual que ejecuta GCWB.
     * @param instr Instruccion descodificada con reg_data.reg1 (fuente) y mode.
     */
    void exec_instr_gcwb(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  rsrc       = instr.data_instruction.reg_data.reg1; // registro con el handle antiguo
        const uint64_t old_handle = read_reg_table[instr.flags_info.mode](vm, rsrc);
        vm->gc_heap.write_barrier(static_cast<gc::GcHandle>(old_handle)); // notificar la escritura al GC
    }

    /**
     * @brief Ejecuta la instruccion GCALLOC: aloca un bloque de bytes en el heap GC.
     *
     * A diferencia de NEWOBJ, no inicializa ninguna cabecera de objeto.  Util para
     * alocar buffers o arreglos gestionados por el GC.  El handle resultante se
     * devuelve en R00.
     *
     * @param vm    Proceso virtual que ejecuta GCALLOC.
     * @param instr Instruccion descodificada con reg_data.reg1 (tamano) y mode.
     */
    void exec_instr_gcalloc(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  rsrc = instr.data_instruction.reg_data.reg1; // registro con el tamano en bytes
        const uint64_t size = read_reg_table[instr.flags_info.mode](vm, rsrc);

        gc::GcHandle h = vm->gc_heap.alloc(static_cast<size_t>(size)); // alocar en el heap GC
        vm->registers.regs[R00].qword(static_cast<uint64_t>(h));       // devolver el handle en R00
    }

    // -------------------------------------------------------------------------
    // Raw allocator - opcodes 0x00 0xB0 .. 0xB2
    // Memoria no gestionada por el GC; ciclo de vida manual.
    // -------------------------------------------------------------------------

    /**
     * @brief Ejecuta la instruccion RAWALLOC: aloca memoria bruta no gestionada por el GC.
     *
     * El tamano se obtiene del registro fuente segun el modo.  El puntero host
     * al bloque alocalizado se escribe en R00 (0 si falla).
     *
     * @param vm    Proceso virtual que ejecuta RAWALLOC.
     * @param instr Instruccion descodificada con reg_data.reg1 (tamano) y mode.
     */
    void exec_instr_raw_alloc(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  rsrc = instr.data_instruction.reg_data.reg1; // registro con el tamano
        const uint64_t size = read_reg_table[instr.flags_info.mode](vm, rsrc);

        uint64_t ptr = vm->raw_alloc.alloc(static_cast<size_t>(size)); // alocar bloque
        vm->registers.regs[R00].qword(ptr); // devolver puntero host en R00
    }

    /**
     * @brief Ejecuta la instruccion RAWFREE: libera un bloque de memoria bruta.
     *
     * El registro fuente contiene el puntero host al bloque previamente alocalizado
     * con RAWALLOC o RAWREALLOC.
     *
     * @param vm    Proceso virtual que ejecuta RAWFREE.
     * @param instr Instruccion descodificada con reg_data.reg1 (puntero a liberar).
     */
    void exec_instr_raw_free(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  rsrc = instr.data_instruction.reg_data.reg1; // registro con el puntero a liberar
        const uint64_t ptr  = vm->registers.regs[rsrc].qword();
        vm->raw_alloc.free(ptr); // liberar el bloque
    }

    /**
     * @brief Ejecuta la instruccion RAWREALLOC: redimensiona un bloque de memoria bruta.
     *
     * Lee el puntero actual del primer registro y el nuevo tamano del segundo.
     * El puntero al bloque redimensionado (que puede diferir del original) se
     * devuelve en R00.
     *
     * @param vm    Proceso virtual que ejecuta RAWREALLOC.
     * @param instr Instruccion descodificada con reg_data.reg1 (ptr) y reg_data.reg2 (size).
     */
    void exec_instr_raw_realloc(ProcessVM *vm, const DecodedInstr &instr) {
        const uint8_t  rptr  = instr.data_instruction.reg_data.reg1; // registro con el puntero original
        const uint8_t  rsize = instr.data_instruction.reg_data.reg2; // registro con el nuevo tamano
        const uint64_t ptr   = vm->registers.regs[rptr].qword();     // puntero al bloque original
        const uint64_t size  = vm->registers.regs[rsize].qword();    // nuevo tamano en bytes

        uint64_t new_ptr = vm->raw_alloc.realloc(ptr, static_cast<size_t>(size)); // redimensionar
        vm->registers.regs[R00].qword(new_ptr); // devolver el nuevo puntero en R00
    }

} // namespace runtime
