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
        static inline T compute(T a) {
            return a + 1;
        }

        template<typename T>
        static inline void flags(VM *vm, T a, T result) {
            vm->flags.bits.CF = vm->flags.bits.CF; // no cambia
            vm->flags.bits.OF = 0; // INC unsigned no tiene overflow
            vm->flags.bits.ZF = (result == 0);
        }
    };

    struct DecOp {
        static constexpr bool is_compare = false;

        template<typename T>
        static inline T compute(T a) {
            return a - 1;
        }

        template<typename T>
        static inline void flags(VM *vm, T a, T result) {
            vm->flags.bits.CF = vm->flags.bits.CF; // no cambia
            vm->flags.bits.OF = 0; // DEC unsigned no tiene overflow
            vm->flags.bits.ZF = (result == 0);
        }
    };


    /**
     * @brief Núcleo del ALU para operaciones binarias (ADD, SUB, CMP, DIV, AND, OR, XOR, etc.).
     *
     * Ejecuta la operación definida por `Op` usando los operandos `a` y `b`, actualiza los
     * flags correspondientes y escribe el resultado en el registro destino, excepto cuando
     * la operación es de comparación (Op::is_compare = true).
     *
     * @tparam T   Tipo del operando (uint8_t, uint16_t, uint32_t, uint64_t).
     * @tparam Op  Estructura que define la operación (Op::compute y Op::flags).
     *
     * @param vm             Puntero a la máquina virtual.
     * @param a              Primer operando (normalmente el valor del registro destino).
     * @param b              Segundo operando (inmediato, registro o memoria).
     * @param is_signed      Indica si la operación debe interpretarse como signed o unsigned.
     * @param dst_reg_index  Índice del registro destino dentro de vm->regs.
     *
     * @note Esta función actualiza ZF y SF de forma genérica. Los flags CF, OF y cualquier
     *       otro flag específico se delegan a Op::flags().
     *
     * @warning No valida el índice del registro destino. Se asume que el decodificador ya
     *          verificó la instrucción.
     */
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

    template<typename T, typename Op>
    static void binary_mem_imm_wrapper(VM *vm, uint64_t addr, uint64_t imm, bool is_signed) {
        using UT = std::make_unsigned_t<T>;

        // Leer valor desde memoria virtual
        T val = vm->vm_mem.read_any<T>(addr);

        // Ejecutar operación ALU
        T res = Op::compute(val, (T) imm);

        // Flags comunes
        vm->flags.bits.ZF = (res == 0);
        vm->flags.bits.SF = (UT(res) >> (sizeof(T) * 8 - 1)) & 1;

        // Flags específicos de la operación
        Op::flags(vm, val, (T) imm, res, is_signed);

        // Escribir resultado en memoria virtual
        vm->vm_mem.write_any<T>(addr, res);
    }


    /**
     * @brief Núcleo del ALU para operaciones unarias (INC, DEC).
     *
     * Ejecuta una operación unaria definida por `Op` usando el operando `a`, actualiza los
     * flags correspondientes y escribe el resultado en el registro destino. Esta versión
     * está optimizada para instrucciones que no requieren un segundo operando ni semántica
     * signed/unsigned.
     *
     * @tparam T   Tipo del operando (uint8_t, uint16_t, uint32_t, uint64_t).
     * @tparam Op  Estructura que define la operación unaria (Op::compute y Op::flags).
     *
     * @param vm             Puntero a la máquina virtual.
     * @param a              Operando de entrada (valor actual del registro destino).
     * @param dst_reg_index  Índice del registro destino dentro de vm->regs.
     *
     * @note Esta función actualiza ZF y SF de forma genérica. Los flags CF, OF y cualquier
     *       otro flag específico se delegan a Op::flags().
     *
     * @warning No valida el índice del registro destino. Se asume que el decodificador ya
     *          verificó la instrucción.
     */
    template<typename T, typename Op>
    inline void alu_core_unary(VM *vm, T a, int dst_reg_index) {
        using UT = std::make_unsigned_t<T>;

        T result = Op::compute(a);

        vm->flags.bits.ZF = (result == 0);

        constexpr int SIGNBIT = sizeof(T) * 8 - 1;
        vm->flags.bits.SF = (static_cast<UT>(result) >> SIGNBIT) & 1;

        Op::flags(vm, a, result);

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


    typedef GeneralRegister &Reg;
    using BinaryFn = void(*)(VM *, Reg, Reg, bool, int);
    using UnaryFn = void(*)(VM *, Reg, int);

    template<typename T, typename Op>
    static void binary_imm_wrapper(VM *vm, Reg &dst, uint64_t imm, bool is_signed, int rdst) {
        alu_core<T, Op>(vm, dst.raw(), (T) imm, is_signed, rdst);
    }

    template<typename T>
    static void mov_wrapper(VM *vm, Reg dst, Reg src, bool /*unused*/, int rdst) {
        T value = src.raw(); // leer valor del registro origen
        auto &d = vm->regs[rdst];

        if constexpr (sizeof(T) == 1)
            d.byte_lo((uint8_t) value);
        else if constexpr (sizeof(T) == 2)
            d.word_lo((uint16_t) value);
        else if constexpr (sizeof(T) == 4)
            d.dword_lo((uint32_t) value);
        else
            d.qword((uint64_t) value);

        // MOV no toca flags
    }

    template<typename T, typename Op>
    static void binary_wrapper(VM *vm, Reg &dst, Reg &src, bool is_signed, int rdst) {
        alu_core<T, Op>(vm, dst.raw(), src.raw(), is_signed, rdst);
    }

    template<typename T, typename Op>
    static void unary_wrapper(VM *vm, Reg &dst, int rdst) {
        alu_core_unary<T, Op>(vm, dst.raw(), rdst);
    }

    static constexpr BinaryFn mov_table[] = {
        &mov_wrapper<uint8_t>,
        &mov_wrapper<uint16_t>,
        &mov_wrapper<uint32_t>,
        &mov_wrapper<uint64_t>
    };

    static constexpr BinaryFn add_table[] = {
        &binary_wrapper<uint8_t, AddOp>,
        &binary_wrapper<uint16_t, AddOp>,
        &binary_wrapper<uint32_t, AddOp>,
        &binary_wrapper<uint64_t, AddOp>
    };

    static constexpr BinaryFn sub_table[] = {
        &binary_wrapper<uint8_t, SubOp>,
        &binary_wrapper<uint16_t, SubOp>,
        &binary_wrapper<uint32_t, SubOp>,
        &binary_wrapper<uint64_t, SubOp>
    };

    static constexpr BinaryFn cmp_table[] = {
        &binary_wrapper<uint8_t, CmpOp>,
        &binary_wrapper<uint16_t, CmpOp>,
        &binary_wrapper<uint32_t, CmpOp>,
        &binary_wrapper<uint64_t, CmpOp>
    };

    static constexpr UnaryFn inc_table[] = {
        &unary_wrapper<uint8_t, IncOp>,
        &unary_wrapper<uint16_t, IncOp>,
        &unary_wrapper<uint32_t, IncOp>,
        &unary_wrapper<uint64_t, IncOp>
    };

    static constexpr UnaryFn dec_table[] = {
        &unary_wrapper<uint8_t, DecOp>,
        &unary_wrapper<uint16_t, DecOp>,
        &unary_wrapper<uint32_t, DecOp>,
        &unary_wrapper<uint64_t, DecOp>
    };

    static constexpr BinaryFn and_table[] = {
        &binary_wrapper<uint8_t, AndOp>,
        &binary_wrapper<uint16_t, AndOp>,
        &binary_wrapper<uint32_t, AndOp>,
        &binary_wrapper<uint64_t, AndOp>
    };

    static constexpr BinaryFn or_table[] = {
        &binary_wrapper<uint8_t, OrOp>,
        &binary_wrapper<uint16_t, OrOp>,
        &binary_wrapper<uint32_t, OrOp>,
        &binary_wrapper<uint64_t, OrOp>
    };

    static constexpr BinaryFn xor_table[] = {
        &binary_wrapper<uint8_t, XorOp>,
        &binary_wrapper<uint16_t, XorOp>,
        &binary_wrapper<uint32_t, XorOp>,
        &binary_wrapper<uint64_t, XorOp>
    };

    static constexpr BinaryFn div_table[] = {
        &binary_wrapper<uint8_t, DivOp>,
        &binary_wrapper<uint16_t, DivOp>,
        &binary_wrapper<uint32_t, DivOp>,
        &binary_wrapper<uint64_t, DivOp>
    };

    static constexpr BinaryFn shl_table[] = {
        &binary_wrapper<uint8_t, ShlOp>,
        &binary_wrapper<uint16_t, ShlOp>,
        &binary_wrapper<uint32_t, ShlOp>,
        &binary_wrapper<uint64_t, ShlOp>
    };

    static constexpr BinaryFn shr_table[] = {
        &binary_wrapper<uint8_t, ShrOp>,
        &binary_wrapper<uint16_t, ShrOp>,
        &binary_wrapper<uint32_t, ShrOp>,
        &binary_wrapper<uint64_t, ShrOp>
    };

    static constexpr BinaryFn sar_table[] = {
        &binary_wrapper<uint8_t, SarOp>,
        &binary_wrapper<uint16_t, SarOp>,
        &binary_wrapper<uint32_t, SarOp>,
        &binary_wrapper<uint64_t, SarOp>
    };

    using BinaryImmFn = void(*)(VM *, Reg &, uint64_t, bool, int);
    static constexpr BinaryImmFn add_imm_table[] = {
        &binary_imm_wrapper<uint8_t, AddOp>,
        &binary_imm_wrapper<uint16_t, AddOp>,
        &binary_imm_wrapper<uint32_t, AddOp>,
        &binary_imm_wrapper<uint64_t, AddOp>
    };

    using BinaryMemImmFn = void(*)(VM *, uint64_t, uint64_t, bool);

    static constexpr BinaryMemImmFn add_mem_imm_table[] = {
        &binary_mem_imm_wrapper<uint8_t, AddOp>,
        &binary_mem_imm_wrapper<uint16_t, AddOp>,
        &binary_mem_imm_wrapper<uint32_t, AddOp>,
        &binary_mem_imm_wrapper<uint64_t, AddOp>
    };


    void exec_instr_add_imm(VM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.inmmed_data.reg;
        uint64_t imm = instr.data_instruction.inmmed_data.inmmed;

        // Caso 1: destino es un registro
        if (instr.flags_info.direction == 0) {
            add_imm_table[instr.flags_info.mode](
                vm,
                vm->regs[rdst],
                imm,
                instr.flags_info._signed_instruct,
                rdst
            );
            return;
        }

        // Caso 2: destino es memoria
        if (instr.flags_info.direction == 1) {
            auto &base = vm->regs[instr.data_instruction.inmmed_data.reg];
            auto index = 0;
            uint8_t scale = 0;

            uint64_t addr = base.raw() + index/*.raw()*/ * scale;

            add_mem_imm_table[instr.flags_info.mode](
                vm,
                addr,
                imm,
                instr.flags_info._signed_instruct
            );
            return;
        }

        vm->should_kill = true;
    }


    void exec_instr_mov_reg(VM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.reg_data.reg1;
        const int rsrc = instr.data_instruction.reg_data.reg2;

        mov_table[instr.flags_info.mode](vm, vm->regs[rdst], vm->regs[rsrc], false, rdst);
    }

    void exec_instr_add_reg(VM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.reg_data.reg1;
        const int rsrc = instr.data_instruction.reg_data.reg2;

        add_table[instr.flags_info.mode](vm, vm->regs[rdst], vm->regs[rsrc], instr.flags_info._signed_instruct, rdst);
    }

    void exec_instr_sub_reg(VM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.reg_data.reg1;
        const int rsrc = instr.data_instruction.reg_data.reg2;

        sub_table[instr.flags_info.mode](vm, vm->regs[rdst], vm->regs[rsrc], instr.flags_info._signed_instruct, rdst);
    }

    void exec_instr_cmp_reg(VM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.reg_data.reg1;
        const int rsrc = instr.data_instruction.reg_data.reg2;

        cmp_table[instr.flags_info.mode](vm, vm->regs[rdst], vm->regs[rsrc], instr.flags_info._signed_instruct, rdst);
    }

    void exec_instr_and_reg(VM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.reg_data.reg1;
        const int rsrc = instr.data_instruction.reg_data.reg2;
        and_table[instr.flags_info.mode](vm, vm->regs[rdst], vm->regs[rsrc], false, rdst);
    }

    void exec_instr_or_reg(VM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.reg_data.reg1;
        const int rsrc = instr.data_instruction.reg_data.reg2;
        or_table[instr.flags_info.mode](vm, vm->regs[rdst], vm->regs[rsrc], false, rdst);
    }

    void exec_instr_xor_reg(VM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.reg_data.reg1;
        const int rsrc = instr.data_instruction.reg_data.reg2;
        xor_table[instr.flags_info.mode](vm, vm->regs[rdst], vm->regs[rsrc], false, rdst);
    }

    void exec_instr_div_reg(VM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.reg_data.reg1;
        const int rsrc = instr.data_instruction.reg_data.reg2;
        div_table[instr.flags_info.mode](vm, vm->regs[rdst], vm->regs[rsrc], instr.flags_info._signed_instruct, rdst);
    }

    void exec_instr_shl_reg(VM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.reg_data.reg1;
        const int rsrc = instr.data_instruction.reg_data.reg2;
        shl_table[instr.flags_info.mode](vm, vm->regs[rdst], vm->regs[rsrc], false, rdst);
    }

    void exec_instr_shr_reg(VM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.reg_data.reg1;
        const int rsrc = instr.data_instruction.reg_data.reg2;
        shr_table[instr.flags_info.mode](vm, vm->regs[rdst], vm->regs[rsrc], false, rdst);
    }

    void exec_instr_sar_reg(VM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.reg_data.reg1;
        const int rsrc = instr.data_instruction.reg_data.reg2;
        sar_table[instr.flags_info.mode](vm, vm->regs[rdst], vm->regs[rsrc], false, rdst);
    }

    void exec_instr_inc_dec_reg(VM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.reg_data.reg1;
        // aunque INC y DEC no tienen signo, se usa el campo _signed_instruct
        // en la descodificacion para almacenar si es la instruccion INC o DEC.
        if (instr.flags_info._signed_instruct == 0)
            inc_table[instr.flags_info.mode](vm, vm->regs[rdst], rdst);
        else
            dec_table[instr.flags_info.mode](vm, vm->regs[rdst], rdst);
    }
}
