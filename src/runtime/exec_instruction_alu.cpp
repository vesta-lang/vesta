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
#include "runtime/exec_instruction.h"

namespace runtime {
    struct IncOp {
        static constexpr bool is_compare = false;

        template<typename T>
        static inline T compute(T a, T) {
            return a + 1;
        }

        template<typename T>
        static inline void flags(VM *vm, T a, T, T result, bool is_signed) {
            using ST = std::make_signed_t<T>;

            // CF NO cambia en INC (igual que x86)
            vm->flags.bits.CF = vm->flags.bits.CF;

            if (is_signed) {
                ST sa = (ST) a;
                ST sres = (ST) result;
                vm->flags.bits.OF = ((sa ^ sres) & (~sa ^ sres)) < 0;
            } else {
                vm->flags.bits.OF = 0; // unsigned INC no tiene overflow aritmético
            }
        }
    };

    struct DecOp {
        static constexpr bool is_compare = false;

        template<typename T>
        static inline T compute(T a, T) {
            return a - 1;
        }

        template<typename T>
        static inline void flags(VM *vm, T a, T, T result, bool is_signed) {
            using ST = std::make_signed_t<T>;

            // CF NO cambia en DEC (igual que x86)
            vm->flags.bits.CF = vm->flags.bits.CF;

            if (is_signed) {
                ST sa = (ST) a;
                ST sres = (ST) result;
                vm->flags.bits.OF = ((sa ^ sres) & (sa ^ ~sres)) < 0;
            } else {
                vm->flags.bits.OF = 0;
            }
        }
    };


    template<typename T, typename Op>
    inline void alu_core(VM *vm, T a, T b, bool is_signed, int dst_reg_index) {
        using ST = std::make_signed_t<T>;
        using UT = std::make_unsigned_t<T>;

        // Calcular resultado usando la operación
        T result = Op::compute(a, b);

        // Flags comunes
        vm->flags.bits.ZF = (result == 0);

        constexpr int SIGNBIT = sizeof(T) * 8 - 1;
        vm->flags.bits.SF = (static_cast<UT>(result) >> SIGNBIT) & 1;

        // Flags específicos de la operación
        Op::flags(vm, a, b, result, is_signed);

        // Escribir resultado (excepto CMP)
        if constexpr (!Op::is_compare) {
            auto &dst = vm->regs[dst_reg_index];

            if constexpr (sizeof(T) == 1)
                dst.byte_lo((uint8_t) result);
            else if constexpr (sizeof(T) == 2)
                dst.word_lo((uint16_t) result);
            else if constexpr (sizeof(T) == 4)
                dst.dword_lo((uint32_t) result);
            else
                dst.qword((uint64_t) result);
        }
    }

    struct AddOp {
        static constexpr bool is_compare = false;

        template<typename T>
        static inline T compute(T a, T b) {
            return a + b;
        }

        template<typename T>
        static inline void flags(VM *vm, T a, T b, T result, bool is_signed) {
            using ST = std::make_signed_t<T>;
            using UT = std::make_unsigned_t<T>;

            if (is_signed) {
                ST sa = (ST) a, sb = (ST) b, sres = (ST) result;
                vm->flags.bits.OF = ((sa ^ sres) & (sb ^ sres)) < 0;
                vm->flags.bits.CF = 0;
            } else {
                UT ua = (UT) a, ub = (UT) b, ures = (UT) result;
                vm->flags.bits.CF = (ua + ub) < ua;
                vm->flags.bits.OF = 0;
            }
        }
    };

    struct SubOp {
        static constexpr bool is_compare = false;

        template<typename T>
        static inline T compute(T a, T b) {
            return a - b;
        }

        template<typename T>
        static inline void flags(VM *vm, T a, T b, T result, bool is_signed) {
            using ST = std::make_signed_t<T>;
            using UT = std::make_unsigned_t<T>;

            if (is_signed) {
                ST sa = (ST) a, sb = (ST) b, sres = (ST) result;
                vm->flags.bits.OF = ((sa ^ sb) & (sa ^ sres)) < 0;
                vm->flags.bits.CF = 0;
            } else {
                UT ua = (UT) a, ub = (UT) b;
                vm->flags.bits.CF = ua < ub;
                vm->flags.bits.OF = 0;
            }
        }
    };

    // CMP (igual que SUB pero sin escribir resultado)
    struct CmpOp {
        static constexpr bool is_compare = true;

        template<typename T>
        static inline T compute(T a, T b) {
            return a - b;
        }

        template<typename T>
        static inline void flags(VM *vm, T a, T b, T result, bool is_signed) {
            SubOp::flags(vm, a, b, result, is_signed);
        }
    };

    struct DivOp {
        static constexpr bool is_compare = false;

        template<typename T>
        static inline T compute(T a, T b) {
            // El núcleo ALU solo devuelve el cociente
            return a / b;
        }

        template<typename T>
        static inline void flags(VM *vm, T a, T b, T result, bool is_signed) {
            using ST = std::make_signed_t<T>;
            using UT = std::make_unsigned_t<T>;

            // División por cero
            if (b == 0) {
                vm->flags.bits.OF = 1;
                vm->flags.bits.CF = 1;
                vm->should_kill = true;
                return;
            }

            if (is_signed) {
                ST sa = (ST) a;
                ST sb = (ST) b;

                // Overflow en signed DIV: INT_MIN / -1
                if (sa == std::numeric_limits<ST>::min() && sb == -1) {
                    vm->flags.bits.OF = 1;
                    vm->flags.bits.CF = 1;
                    vm->should_kill = true;
                    return;
                }

                vm->flags.bits.OF = 0;
                vm->flags.bits.CF = 0;
            } else {
                // Unsigned DIV nunca tiene overflow excepto por división por cero
                vm->flags.bits.OF = 0;
                vm->flags.bits.CF = 0;
            }
        }
    };


    // AND
    struct AndOp {
        static constexpr bool is_compare = false;

        template<typename T>
        static inline T compute(T a, T b) { return a & b; }

        template<typename T>
        static inline void flags(VM *vm, T, T, T, bool) {
            vm->flags.bits.CF = 0;
            vm->flags.bits.OF = 0;
        }
    };

    struct OrOp {
        static constexpr bool is_compare = false;

        template<typename T>
        static inline T compute(T a, T b) { return a | b; }

        template<typename T>
        static inline void flags(VM *vm, T, T, T, bool) {
            vm->flags.bits.CF = 0;
            vm->flags.bits.OF = 0;
        }
    };

    struct XorOp {
        static constexpr bool is_compare = false;

        template<typename T>
        static inline T compute(T a, T b) { return a ^ b; }

        template<typename T>
        static inline void flags(VM *vm, T, T, T, bool) {
            vm->flags.bits.CF = 0;
            vm->flags.bits.OF = 0;
        }
    };

    struct ShlOp {
        static constexpr bool is_compare = false;

        template<typename T>
        static inline T compute(T a, T b) {
            using UT = std::make_unsigned_t<T>;
            uint32_t shift = (uint32_t) b & (sizeof(T) * 8 - 1);
            return (T) ((UT) a << shift);
        }

        template<typename T>
        static inline void flags(VM *vm, T a, T b, T result, bool) {
            using UT = std::make_unsigned_t<T>;
            uint32_t shift = (uint32_t) b & (sizeof(T) * 8 - 1);

            if (shift == 0) {
                vm->flags.bits.CF = 0;
                vm->flags.bits.OF = 0;
                return;
            }

            UT ua = (UT) a;
            UT cf_bit = (ua >> (sizeof(T) * 8 - shift)) & 1;
            vm->flags.bits.CF = cf_bit;

            // Overflow: SHL OF = XOR(CF, MSB(result))
            UT msb = (result >> (sizeof(T) * 8 - 1)) & 1;
            vm->flags.bits.OF = (cf_bit ^ msb);
        }
    };

    struct ShrOp {
        static constexpr bool is_compare = false;

        template<typename T>
        static inline T compute(T a, T b) {
            using UT = std::make_unsigned_t<T>;
            uint32_t shift = (uint32_t) b & (sizeof(T) * 8 - 1);
            return (T) ((UT) a >> shift);
        }

        template<typename T>
        static inline void flags(VM *vm, T a, T b, T result, bool) {
            using UT = std::make_unsigned_t<T>;
            uint32_t shift = (uint32_t) b & (sizeof(T) * 8 - 1);

            if (shift == 0) {
                vm->flags.bits.CF = 0;
                vm->flags.bits.OF = 0;
                return;
            }

            UT ua = (UT) a;
            vm->flags.bits.CF = (ua >> (shift - 1)) & 1;
            vm->flags.bits.OF = 0;
        }
    };

    struct SarOp {
        static constexpr bool is_compare = false;

        template<typename T>
        static inline T compute(T a, T b) {
            using ST = std::make_signed_t<T>;
            uint32_t shift = (uint32_t) b & (sizeof(T) * 8 - 1);
            return (T) ((ST) a >> shift);
        }

        template<typename T>
        static inline void flags(VM *vm, T a, T b, T result, bool) {
            using ST = std::make_signed_t<T>;
            using UT = std::make_unsigned_t<T>;

            uint32_t shift = (uint32_t) b & (sizeof(T) * 8 - 1);

            if (shift == 0) {
                vm->flags.bits.CF = 0;
                vm->flags.bits.OF = 0;
                return;
            }

            UT ua = (UT) a;
            vm->flags.bits.CF = (ua >> (shift - 1)) & 1;
            vm->flags.bits.OF = 0;
        }
    };


    void exec_instr_add_reg(VM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.reg_data.reg1;
        const int rsrc = instr.data_instruction.reg_data.reg2;

        auto &dst = vm->regs[rdst];
        auto &src = vm->regs[rsrc];

        switch (instr.mode) {
            case 0: {
                // 8 bits
                uint8_t a = static_cast<uint8_t>(dst.qword());
                uint8_t b = static_cast<uint8_t>(src.qword());
                alu_core<uint8_t, AddOp>(vm, a, b, instr._signed_instruct, rdst);
                break;
            }
            case 1: {
                // 16 bits
                uint16_t a = static_cast<uint16_t>(dst.qword());
                uint16_t b = static_cast<uint16_t>(src.qword());
                alu_core<uint16_t, AddOp>(vm, a, b, instr._signed_instruct, rdst);
                break;
            }
            case 2: {
                // 32 bits
                uint32_t a = static_cast<uint32_t>(dst.qword());
                uint32_t b = static_cast<uint32_t>(src.qword());
                alu_core<uint32_t, AddOp>(vm, a, b, instr._signed_instruct, rdst);
                break;
            }
            case 3: {
                // 64 bits
                uint64_t a = dst.qword();
                uint64_t b = src.qword();
                alu_core<uint64_t, AddOp>(vm, a, b, instr._signed_instruct, rdst);
                break;
            }
            default:
                vm->flags.bits.OF = 1;
                vm->flags.bits.CF = 1;
                vm->should_kill = true;
                return;
        }
    }

    void exec_instr_sub_reg(VM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.reg_data.reg1;
        const int rsrc = instr.data_instruction.reg_data.reg2;

        auto &dst = vm->regs[rdst];
        auto &src = vm->regs[rsrc];

        switch (instr.mode) {
            case 0: alu_core<uint8_t, SubOp>(vm, (uint8_t) dst.qword(), (uint8_t) src.qword(), instr._signed_instruct,
                                             rdst);
                break;
            case 1: alu_core<uint16_t, SubOp>(vm, (uint16_t) dst.qword(), (uint16_t) src.qword(),
                                              instr._signed_instruct, rdst);
                break;
            case 2: alu_core<uint32_t, SubOp>(vm, (uint32_t) dst.qword(), (uint32_t) src.qword(),
                                              instr._signed_instruct, rdst);
                break;
            case 3: alu_core<uint64_t, SubOp>(vm, dst.qword(), src.qword(), instr._signed_instruct, rdst);
                break;
            default:
                vm->flags.bits.OF = 1;
                vm->flags.bits.CF = 1;
                vm->should_kill = true;
                return;
        }
    }

    void exec_instr_cmp_reg(VM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.reg_data.reg1;
        const int rsrc = instr.data_instruction.reg_data.reg2;

        auto &dst = vm->regs[rdst];
        auto &src = vm->regs[rsrc];

        switch (instr.mode) {
            case 0: alu_core<uint8_t, CmpOp>(vm, (uint8_t) dst.qword(), (uint8_t) src.qword(), instr._signed_instruct,
                                             rdst);
                break;
            case 1: alu_core<uint16_t, CmpOp>(vm, (uint16_t) dst.qword(), (uint16_t) src.qword(),
                                              instr._signed_instruct, rdst);
                break;
            case 2: alu_core<uint32_t, CmpOp>(vm, (uint32_t) dst.qword(), (uint32_t) src.qword(),
                                              instr._signed_instruct, rdst);
                break;
            case 3: alu_core<uint64_t, CmpOp>(vm, dst.qword(), src.qword(), instr._signed_instruct, rdst);
                break;
            default:
                vm->flags.bits.OF = 1;
                vm->flags.bits.CF = 1;
                vm->should_kill = true;
                return;
        }
    }

    void exec_instr_and_reg(VM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.reg_data.reg1;
        const int rsrc = instr.data_instruction.reg_data.reg2;

        auto &dst = vm->regs[rdst];
        auto &src = vm->regs[rsrc];

        switch (instr.mode) {
            case 0: alu_core<uint8_t, AndOp>(vm, (uint8_t) dst.qword(), (uint8_t) src.qword(), false, rdst);
                break;
            case 1: alu_core<uint16_t, AndOp>(vm, (uint16_t) dst.qword(), (uint16_t) src.qword(), false, rdst);
                break;
            case 2: alu_core<uint32_t, AndOp>(vm, (uint32_t) dst.qword(), (uint32_t) src.qword(), false, rdst);
                break;
            case 3: alu_core<uint64_t, AndOp>(vm, dst.qword(), src.qword(), false, rdst);
                break;
            default:
                vm->flags.bits.OF = 1;
                vm->flags.bits.CF = 1;
                vm->should_kill = true;
                return;
        }
    }

    void exec_instr_or_reg(VM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.reg_data.reg1;
        const int rsrc = instr.data_instruction.reg_data.reg2;

        auto &dst = vm->regs[rdst];
        auto &src = vm->regs[rsrc];

        switch (instr.mode) {
            case 0: alu_core<uint8_t, OrOp>(vm, (uint8_t) dst.qword(), (uint8_t) src.qword(), false, rdst);
                break;
            case 1: alu_core<uint16_t, OrOp>(vm, (uint16_t) dst.qword(), (uint16_t) src.qword(), false, rdst);
                break;
            case 2: alu_core<uint32_t, OrOp>(vm, (uint32_t) dst.qword(), (uint32_t) src.qword(), false, rdst);
                break;
            case 3: alu_core<uint64_t, OrOp>(vm, dst.qword(), src.qword(), false, rdst);
                break;
            default:
                vm->flags.bits.OF = 1;
                vm->flags.bits.CF = 1;
                vm->should_kill = true;
                return;
        }
    }

    void exec_instr_xor_reg(VM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.reg_data.reg1;
        const int rsrc = instr.data_instruction.reg_data.reg2;

        auto &dst = vm->regs[rdst];
        auto &src = vm->regs[rsrc];

        switch (instr.mode) {
            case 0: alu_core<uint8_t, XorOp>(vm, (uint8_t) dst.qword(), (uint8_t) src.qword(), false, rdst);
                break;
            case 1: alu_core<uint16_t, XorOp>(vm, (uint16_t) dst.qword(), (uint16_t) src.qword(), false, rdst);
                break;
            case 2: alu_core<uint32_t, XorOp>(vm, (uint32_t) dst.qword(), (uint32_t) src.qword(), false, rdst);
                break;
            case 3: alu_core<uint64_t, XorOp>(vm, dst.qword(), src.qword(), false, rdst);
                break;
            default:
                vm->flags.bits.OF = 1;
                vm->flags.bits.CF = 1;
                vm->should_kill = true;
                return;
        }
    }

    void exec_instr_div_reg(VM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.reg_data.reg1;
        const int rsrc = instr.data_instruction.reg_data.reg2;

        auto &dst = vm->regs[rdst];
        auto &src = vm->regs[rsrc];

        switch (instr.mode) {
            case 0: alu_core<uint8_t, DivOp>(vm, (uint8_t) dst.qword(), (uint8_t) src.qword(), instr._signed_instruct,
                                             rdst);
                break;
            case 1: alu_core<uint16_t, DivOp>(vm, (uint16_t) dst.qword(), (uint16_t) src.qword(),
                                              instr._signed_instruct, rdst);
                break;
            case 2: alu_core<uint32_t, DivOp>(vm, (uint32_t) dst.qword(), (uint32_t) src.qword(),
                                              instr._signed_instruct, rdst);
                break;
            case 3: alu_core<uint64_t, DivOp>(vm, dst.qword(), src.qword(), instr._signed_instruct, rdst);
                break;

            default:
                vm->flags.bits.OF = 1;
                vm->flags.bits.CF = 1;
                vm->should_kill = true;
                return;
        }
    }

    void exec_instr_shl_reg(VM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.reg_data.reg1;
        const int rsrc = instr.data_instruction.reg_data.reg2;

        auto &dst = vm->regs[rdst];
        auto &src = vm->regs[rsrc];

        switch (instr.mode) {
            case 0: alu_core<uint8_t, ShlOp>(vm, (uint8_t) dst.qword(), (uint8_t) src.qword(), false, rdst);
                break;
            case 1: alu_core<uint16_t, ShlOp>(vm, (uint16_t) dst.qword(), (uint16_t) src.qword(), false, rdst);
                break;
            case 2: alu_core<uint32_t, ShlOp>(vm, (uint32_t) dst.qword(), (uint32_t) src.qword(), false, rdst);
                break;
            case 3: alu_core<uint64_t, ShlOp>(vm, dst.qword(), src.qword(), false, rdst);
                break;
        }
    }

    void exec_instr_shr_reg(VM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.reg_data.reg1;
        const int rsrc = instr.data_instruction.reg_data.reg2;

        auto &dst = vm->regs[rdst];
        auto &src = vm->regs[rsrc];

        switch (instr.mode) {
            case 0: alu_core<uint8_t, ShrOp>(vm, (uint8_t) dst.qword(), (uint8_t) src.qword(), false, rdst);
                break;
            case 1: alu_core<uint16_t, ShrOp>(vm, (uint16_t) dst.qword(), (uint16_t) src.qword(), false, rdst);
                break;
            case 2: alu_core<uint32_t, ShrOp>(vm, (uint32_t) dst.qword(), (uint32_t) src.qword(), false, rdst);
                break;
            case 3: alu_core<uint64_t, ShrOp>(vm, dst.qword(), src.qword(), false, rdst);
                break;
        }
    }

    void exec_instr_sar_reg(VM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.reg_data.reg1;
        const int rsrc = instr.data_instruction.reg_data.reg2;

        auto &dst = vm->regs[rdst];
        auto &src = vm->regs[rsrc];

        switch (instr.mode) {
            case 0: alu_core<uint8_t, SarOp>(vm, (uint8_t) dst.qword(), (uint8_t) src.qword(), false, rdst);
                break;
            case 1: alu_core<uint16_t, SarOp>(vm, (uint16_t) dst.qword(), (uint16_t) src.qword(), false, rdst);
                break;
            case 2: alu_core<uint32_t, SarOp>(vm, (uint32_t) dst.qword(), (uint32_t) src.qword(), false, rdst);
                break;
            case 3: alu_core<uint64_t, SarOp>(vm, dst.qword(), src.qword(), false, rdst);
                break;
        }
    }

    void exec_instr_inc_reg(VM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.reg_data.reg1;
        auto &dst = vm->regs[rdst];

        switch (instr.mode) {
            case 0: alu_core<uint8_t, IncOp>(vm, (uint8_t) dst.qword(), 0, instr._signed_instruct, rdst);
                break;
            case 1: alu_core<uint16_t, IncOp>(vm, (uint16_t) dst.qword(), 0, instr._signed_instruct, rdst);
                break;
            case 2: alu_core<uint32_t, IncOp>(vm, (uint32_t) dst.qword(), 0, instr._signed_instruct, rdst);
                break;
            case 3: alu_core<uint64_t, IncOp>(vm, dst.qword(), 0, instr._signed_instruct, rdst);
                break;
            default:
                vm->flags.bits.OF = 1;
                vm->flags.bits.CF = 1;
                vm->should_kill = true;
                return;
        }
    }

    void exec_instr_dec_reg(VM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.reg_data.reg1;
        auto &dst = vm->regs[rdst];

        switch (instr.mode) {
            case 0: alu_core<uint8_t, DecOp>(vm, (uint8_t) dst.qword(), 0, instr._signed_instruct, rdst);
                break;
            case 1: alu_core<uint16_t, DecOp>(vm, (uint16_t) dst.qword(), 0, instr._signed_instruct, rdst);
                break;
            case 2: alu_core<uint32_t, DecOp>(vm, (uint32_t) dst.qword(), 0, instr._signed_instruct, rdst);
                break;
            case 3: alu_core<uint64_t, DecOp>(vm, dst.qword(), 0, instr._signed_instruct, rdst);
                break;
            default:
                vm->flags.bits.OF = 1;
                vm->flags.bits.CF = 1;
                vm->should_kill = true;
                return;
        }
    }
}
