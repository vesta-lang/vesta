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
#ifndef VM_REGISTERS_H
#define VM_REGISTERS_H
#include <string>

#include "rflags.h"
#include "cli/sync_io.h"

namespace runtime {
    /**
     * ID de registros de proposito general
     */
    enum RegID {
        R00, R01, R02, R03,
        R04, R05, R06, R07,
        R08, R09, R10, R11,
        R12, R13, R14, R15,

        /**
         * Se usa para contar cuantos registros de proposito general hay
         */
        COUNT
    };

    class GeneralRegister {
    private:
        uint64_t data = 0;

    public:
        // Lectura segura (zero-extend)
        [[nodiscard]] uint8_t byte_lo() const {
            return static_cast<uint8_t>(data & 0xFF);
        }

        [[nodiscard]] uint8_t byte_hi() const {
            return static_cast<uint8_t>((data >> 8) & 0xFF);
        }

        [[nodiscard]] uint16_t word_lo() const {
            return static_cast<uint16_t>(data & 0xFFFF);
        }

        [[nodiscard]] uint16_t word_hi() const {
            return static_cast<uint16_t>((data >> 48) & 0xFFFF);
        }

        [[nodiscard]] uint32_t dword_lo() const {
            return static_cast<uint32_t>(data & 0xFFFFFFFF);
        }

        [[nodiscard]] uint32_t dword_hi() const {
            return static_cast<uint32_t>((data >> 32) & 0xFFFFFFFF);
        }

        [[nodiscard]] uint64_t qword() const {
            return data;
        }

        // Escritura segura (zero-extend automático)
        GeneralRegister &byte_lo(const uint8_t v) {
            data = (data & ~0xFFULL) | v;
            return *this;
        }

        GeneralRegister &byte_hi(const uint8_t v) {
            data = (data & ~0xFF00ULL) | (static_cast<uint64_t>(v) << 8);
            return *this;
        }

        GeneralRegister &word_lo(const uint16_t v) {
            data = (data & ~0xFFFFULL) | v;
            return *this;
        }

        GeneralRegister &word_hi(const uint16_t v) {
            data = (data & ~0xFFFF000000000000ULL) | (static_cast<uint64_t>(v) << 48);
            return *this;
        }

        GeneralRegister &dword_lo(const uint32_t v) {
            data = (data & 0xFFFFFFFF00000000ULL) | v;
            return *this;
        }

        GeneralRegister &dword_hi(const uint32_t v) {
            data = (data & 0xFFFFFFFFULL) | (static_cast<uint64_t>(v) << 32);
            return *this;
        }

        GeneralRegister &qword(const uint64_t v) {
            data = v;
            return *this;
        }

        // Acceso directo al raw
        [[nodiscard]] uint64_t raw() const {
            return data;
        }

        void raw(const uint64_t v) {
            data = v;
        }

        [[nodiscard]] std::string to_string() const {
            std::ostringstream oss;
            oss << data;
            return oss.str();
        }
    };

    typedef struct context_registers_vm {
        // registros
        // -----------------------------------------------------------
        GeneralRegister stack_pointer{}; // puntero tope de la pila
        GeneralRegister base_pointer{};  // puntero base de la pila

        GeneralRegister rip{}; // puntero de instruccion
        RFlags_t        flags{};

        /**
         * Registros de proposito general.
         */
        GeneralRegister regs[16];

        /**
         * Registros cursor, cur0, cur1, cur2 y cur3
         */
        GeneralRegister cur[4];
    } context_registers_vm;
}
#endif //VM_REGISTERS_H
