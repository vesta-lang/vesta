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

#include "runtime/decode_table.h"

#include "emmit/emmit_decl.h"
#include "runtime/exec_instruction.h"

namespace runtime {
    /**
        * Tabla de opcodes primarios, si no se usa 0x00 siempre se accedera a esta
        * tabla.
        */
    InstrFormat decode_table_primary[0X100] = {
        /* 0x00 */{
            // aunque el 0x00 no se usa, lo definimos por seguridad.
            "EXT_OPCODE", Assembly::Bytecode::AddressingMode::NONE,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x01 */{
            // vminfo
            "vminfo", Assembly::Bytecode::AddressingMode::NONE,
            Assembly::Bytecode::InstrSizeMode::FIXED_2,
            nullptr, nullptr
        },

        /* 0x02 */{
            // vminfomanager
            "vminfomanager", Assembly::Bytecode::AddressingMode::NONE,
            Assembly::Bytecode::InstrSizeMode::FIXED_2,
            nullptr, nullptr
        },

        /* 0x03 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x04 */{
            // inc / dec
            "inc / dec", Assembly::Bytecode::AddressingMode::REG,
            Assembly::Bytecode::InstrSizeMode::FIXED_2,
            exec_instr_inc_dec_reg, decode_instr_one_op_reg
        },

        /* 0x05 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x06 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x07 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x08 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x09 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x0A */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x0B */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x0C */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x0D */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x0E */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x0F */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x10 */{
            // callvm
            "callvm", Assembly::Bytecode::AddressingMode::INMED,
            Assembly::Bytecode::InstrSizeMode::FIXED_8,
            nullptr, nullptr
        },

        /* 0x11 */{
            // jmp
            "jmp", Assembly::Bytecode::AddressingMode::INMED,
            Assembly::Bytecode::InstrSizeMode::FIXED_8,
            nullptr, nullptr
        },

        /* 0x12 */{
            // push
            "push", Assembly::Bytecode::AddressingMode::REG,
            Assembly::Bytecode::InstrSizeMode::FIXED_2,
            nullptr, nullptr
        },

        /* 0x13 */{
            // pop
            "pop", Assembly::Bytecode::AddressingMode::REG,
            Assembly::Bytecode::InstrSizeMode::FIXED_2,
            nullptr, nullptr
        },

        /* 0x14 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x15 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x16 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x17 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x18 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x19 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x1A */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x1B */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x1C */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x1D */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x1E */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x1F */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x20 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x21 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x22 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x23 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x24 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x25 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x26 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x27 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x28 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x29 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x2A */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x2B */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x2C */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x2D */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x2E */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x2F */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x30 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x31 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x32 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x33 */{
            // nop1
            "nop1", Assembly::Bytecode::AddressingMode::NONE,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x34 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x35 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x36 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x37 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x38 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x39 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x3A */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x3B */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x3C */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x3D */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x3E */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x3F */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x40 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },


        /* 0x41 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x42 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x43 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x44 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x45 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x46 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x47 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x48 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x49 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x4A */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x4B */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x4C */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x4D */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x4E */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x4F */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x50 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },


        /* 0x51 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x52 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x53 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x54 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x55 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x56 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x57 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x58 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x59 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x5A */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x5B */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x5C */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x5D */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x5E */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x5F */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x60 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },


        /* 0x61 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x62 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x63 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x64 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x65 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x66 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x67 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x68 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x69 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x6A */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x6B */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x6C */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x6D */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x6E */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x6F */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x70 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },


        /* 0x71 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x72 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x73 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x74 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x75 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x76 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x77 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x78 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x79 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x7A */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x7B */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x7C */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x7D */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x7E */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x7F */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x80 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },


        /* 0x81 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x82 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x83 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x84 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x85 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x86 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x87 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x88 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x89 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x8A */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x8B */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x8C */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x8D */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x8E */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x8F */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x90 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },


        /* 0x91 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x92 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x93 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x94 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x95 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x96 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x97 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x98 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x99 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x9A */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x9B */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x9C */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x9D */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x9E */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x9F */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xA0 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },


        /* 0xA1 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xA2 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xA3 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xA4 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xA5 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xA6 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xA7 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xA8 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xA9 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xAA */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xAB */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xAC */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xAD */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xAE */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xAF */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xB0 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },


        /* 0xB1 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xB2 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xB3 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xB4 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xB5 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xB6 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xB7 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xB8 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xB9 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xBA */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xBB */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xBC */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xBD */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xBE */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xBF */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xC0 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },


        /* 0xC1 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xC2 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xC3 */{
            // ret
            "ret", Assembly::Bytecode::AddressingMode::NONE,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xC4 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xC5 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xC6 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xC7 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xC8 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xC9 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xCA */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xCB */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xCC */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xCD */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xCE */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xCF */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xD0 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },


        /* 0xD1 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xD2 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xD3 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xD4 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xD5 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xD6 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xD7 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xD8 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xD9 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xDA */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xDB */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xDC */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xDD */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xDE */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xDF */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xE0 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },


        /* 0xE1 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xE2 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xE3 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xE4 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xE5 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xE6 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xE7 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xE8 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xE9 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xEA */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xEB */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xEC */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xED */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xEE */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xEF */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xF0 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },


        /* 0xF1 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xF2 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xF3 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xF4 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xF5 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xF6 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xF7 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xF8 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xF9 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xFA */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xFB */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xFC */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xFD */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xFE */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xFF */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

    };

    /**
     * Tabla de codigos extendidos, solo se usa si el primer byte de la instruccion
     * usa 0x00 para extender el opcode1 a dos.
     */
    InstrFormat decode_table_extended[0X100] = {
        /* 0x00 */{
            // edmw4
            "edmw4", Assembly::Bytecode::AddressingMode::NONE,
            Assembly::Bytecode::InstrSizeMode::FIXED_4,
            nullptr, nullptr
        },

        /* 0x01 */{
            // edmw6
            "edmw6", Assembly::Bytecode::AddressingMode::NONE,
            Assembly::Bytecode::InstrSizeMode::FIXED_4,
            nullptr, nullptr
        },

        /* 0x02 */{
            // edm
            "edm", Assembly::Bytecode::AddressingMode::NONE,
            Assembly::Bytecode::InstrSizeMode::FIXED_2,
            nullptr, nullptr
        },

        /* 0x03 */{
            // hlt
            "hlt", Assembly::Bytecode::AddressingMode::NONE,
            Assembly::Bytecode::InstrSizeMode::FIXED_2,
            exec_instr_hlt, decode_instr_simple // sin descodificacion compleja `decode_instr_simple`
        },

        /* 0x04 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x05 */{
            // add reg, reg
            "add", Assembly::Bytecode::AddressingMode::REG,
            Assembly::Bytecode::InstrSizeMode::FIXED_4,
            exec_instr_add_reg, decode_instr_two_op_reg
        },

        /* 0x06 */{
            // add reg, [mem] || [mem], reg
            "add", Assembly::Bytecode::AddressingMode::MEM,
            Assembly::Bytecode::InstrSizeMode::FIXED_8,
            nullptr, nullptr
        },

        /* 0x07 */{
            // add REG, SIB || SIB, REG
            "add", Assembly::Bytecode::AddressingMode::SIB,
            Assembly::Bytecode::InstrSizeMode::FIXED_4,
            nullptr, nullptr
        },

        /* 0x08 */{
            // sub reg, reg
            "sub", Assembly::Bytecode::AddressingMode::REG,
            Assembly::Bytecode::InstrSizeMode::FIXED_4,
            nullptr, nullptr
        },

        /* 0x09 */{
            // sub reg, [mem] || [mem], reg
            "sub", Assembly::Bytecode::AddressingMode::MEM,
            Assembly::Bytecode::InstrSizeMode::FIXED_8,
            nullptr, nullptr
        },

        /* 0x0A */{
            // sub REG, SIB || SIB, REG
            "sub", Assembly::Bytecode::AddressingMode::SIB,
            Assembly::Bytecode::InstrSizeMode::FIXED_4,
            nullptr, nullptr
        },

        /* 0x0B */{
            // mul reg, reg
            "mul", Assembly::Bytecode::AddressingMode::REG,
            Assembly::Bytecode::InstrSizeMode::FIXED_4,
            nullptr, nullptr
        },

        /* 0x0C */{
            // mul reg, [mem] || [mem], reg
            "mul", Assembly::Bytecode::AddressingMode::MEM,
            Assembly::Bytecode::InstrSizeMode::FIXED_4,
            nullptr, nullptr
        },

        /* 0x0D */{
            // mul REG, SIB || SIB, REG
            "mul", Assembly::Bytecode::AddressingMode::SIB,
            Assembly::Bytecode::InstrSizeMode::FIXED_4,
            nullptr, nullptr
        },

        /* 0x0E */{
            // div reg, reg
            "div", Assembly::Bytecode::AddressingMode::REG,
            Assembly::Bytecode::InstrSizeMode::FIXED_4,
            nullptr, nullptr
        },

        /* 0x0F */{
            // div reg, [mem] || [mem], reg
            "div", Assembly::Bytecode::AddressingMode::MEM,
            Assembly::Bytecode::InstrSizeMode::FIXED_8,
            nullptr, nullptr
        },

        /* 0x10 */{
            // div REG, SIB || SIB, REG
            "div", Assembly::Bytecode::AddressingMode::SIB,
            Assembly::Bytecode::InstrSizeMode::FIXED_4,
            nullptr, nullptr
        },

        /* 0x11 */{
            // cmp reg, reg
            "cmp", Assembly::Bytecode::AddressingMode::REG,
            Assembly::Bytecode::InstrSizeMode::FIXED_4,
            nullptr, nullptr
        },

        /* 0x12 */{
            // cmp reg, [mem] || [mem], reg
            "cmp", Assembly::Bytecode::AddressingMode::MEM,
            Assembly::Bytecode::InstrSizeMode::FIXED_8,
            nullptr, nullptr
        },

        /* 0x13 */{
            // cmp REG, SIB || SIB, REG
            "cmp", Assembly::Bytecode::AddressingMode::SIB,
            Assembly::Bytecode::InstrSizeMode::FIXED_4,
            nullptr, nullptr
        },

        /* 0x14 */{
            // mov reg, reg
            "mov", Assembly::Bytecode::AddressingMode::REG,
            Assembly::Bytecode::InstrSizeMode::FIXED_4,
            exec_instr_mov_reg, decode_instr_simple_mov
        },

        /* 0x15 */{
            // mov reg, [mem] || [mem], reg
            "mov", Assembly::Bytecode::AddressingMode::MEM,
            Assembly::Bytecode::InstrSizeMode::FIXED_8,
            nullptr, nullptr
        },

        /* 0x16 */{
            // mov REG, SIB || SIB, REG
            "mov", Assembly::Bytecode::AddressingMode::SIB,
            Assembly::Bytecode::InstrSizeMode::FIXED_4,
            nullptr, nullptr
        },

        /* 0x17 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x18 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x19 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x1A */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x1B */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x1C */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x1D */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x1E */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x1F */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x20 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x21 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x22 */{
            // CALLVM/JMP   <addr56bits>
            "call / jmp", Assembly::Bytecode::AddressingMode::REG,
            Assembly::Bytecode::InstrSizeMode::FIXED_4,
            nullptr, nullptr
        },

        /* 0x23 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x24 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x25 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x26 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x27 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x28 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x29 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x2A */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x2B */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x2C */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x2D */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x2E */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x2F */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x30 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x31 */{
            // loop
            "loop", Assembly::Bytecode::AddressingMode::INMED,
            Assembly::Bytecode::InstrSizeMode::FIXED_8,
            nullptr, nullptr
        },

        /* 0x32 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x33 */{
            // nop2
            "nop2", Assembly::Bytecode::AddressingMode::NONE,
            Assembly::Bytecode::InstrSizeMode::FIXED_2,
            nullptr, nullptr
        },

        /* 0x34 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x35 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x36 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x37 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x38 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x39 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x3A */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x3B */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x3C */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x3D */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x3E */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x3F */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x40 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },


        /* 0x41 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x42 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x43 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x44 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x45 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x46 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x47 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x48 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x49 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x4A */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x4B */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x4C */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x4D */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x4E */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x4F */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x50 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },


        /* 0x51 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x52 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x53 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x54 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x55 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x56 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x57 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x58 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x59 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x5A */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x5B */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x5C */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x5D */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x5E */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x5F */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x60 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },


        /* 0x61 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x62 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x63 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x64 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x65 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x66 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x67 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x68 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x69 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x6A */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x6B */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x6C */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x6D */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x6E */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x6F */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x70 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },


        /* 0x71 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x72 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x73 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x74 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x75 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x76 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x77 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x78 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x79 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x7A */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x7B */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x7C */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x7D */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x7E */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x7F */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x80 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },


        /* 0x81 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x82 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x83 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x84 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x85 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x86 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x87 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x88 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x89 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x8A */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x8B */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x8C */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x8D */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x8E */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x8F */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x90 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },


        /* 0x91 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x92 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x93 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x94 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x95 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x96 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x97 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x98 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x99 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x9A */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x9B */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x9C */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x9D */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x9E */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0x9F */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xA0 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },


        /* 0xA1 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xA2 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xA3 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xA4 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xA5 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xA6 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xA7 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xA8 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xA9 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xAA */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xAB */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xAC */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xAD */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xAE */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xAF */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xB0 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },


        /* 0xB1 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xB2 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xB3 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xB4 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xB5 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xB6 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xB7 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xB8 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xB9 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xBA */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xBB */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xBC */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xBD */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xBE */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xBF */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xC0 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },


        /* 0xC1 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xC2 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xC3 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xC4 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xC5 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xC6 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xC7 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xC8 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xC9 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xCA */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xCB */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xCC */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xCD */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xCE */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xCF */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xD0 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },


        /* 0xD1 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xD2 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xD3 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xD4 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xD5 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xD6 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xD7 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xD8 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xD9 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xDA */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xDB */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xDC */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xDD */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xDE */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xDF */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xE0 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },


        /* 0xE1 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xE2 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xE3 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xE4 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xE5 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xE6 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xE7 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xE8 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xE9 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xEA */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xEB */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xEC */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xED */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xEE */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xEF */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xF0 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },


        /* 0xF1 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xF2 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xF3 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xF4 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xF5 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xF6 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xF7 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xF8 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xF9 */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xFA */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xFB */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xFC */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xFD */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xFE */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },

        /* 0xFF */{
            //
            "", Assembly::Bytecode::AddressingMode::COUNT,
            Assembly::Bytecode::InstrSizeMode::FIXED_1,
            nullptr, nullptr
        },
    };
}
