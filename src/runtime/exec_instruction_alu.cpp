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
        static inline void flags(ProcessVM *vm, T a, T result) {
            vm->registers.flags.bits.CF = vm->registers.flags.bits.CF; // no cambia
            vm->registers.flags.bits.OF = 0;                           // INC unsigned no tiene overflow
            vm->registers.flags.bits.ZF = (result == 0);
        }
    };

    struct DecOp {
        static constexpr bool is_compare = false;

        template<typename T>
        static inline T compute(T a) {
            return a - 1;
        }

        template<typename T>
        static inline void flags(ProcessVM *vm, T a, T result) {
            vm->registers.flags.bits.CF = vm->registers.flags.bits.CF; // no cambia
            vm->registers.flags.bits.OF = 0;                           // DEC unsigned no tiene overflow
            vm->registers.flags.bits.ZF = (result == 0);
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
    inline void alu_core(ProcessVM *vm, T a, T b, bool is_signed, int dst_reg_index) {
        using ST = std::make_signed_t<T>;
        using UT = std::make_unsigned_t<T>;

        // Calcular resultado usando la operación
        T result = Op::compute(a, b);

        // Flags comunes
        vm->registers.flags.bits.ZF = (result == 0);

        constexpr int SIGNBIT       = sizeof(T) * 8 - 1;
        vm->registers.flags.bits.SF = (static_cast<UT>(result) >> SIGNBIT) & 1;

        // Flags específicos de la operación
        Op::flags(vm, a, b, result, is_signed);

        // Escribir resultado (excepto CMP)
        if constexpr (!Op::is_compare) {
            auto &dst = vm->registers.regs[dst_reg_index];

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
    static void binary_mem_imm_wrapper(ProcessVM *vm, uint64_t addr, uint64_t imm, bool is_signed) {
        using UT = std::make_unsigned_t<T>;

        // Leer valor desde memoria virtual
        T val = vm->vm_mem.read_any<T>(addr);

        // Ejecutar operación ALU
        T res = Op::compute(val, (T) imm);

        // Flags comunes
        vm->registers.flags.bits.ZF = (res == 0);
        vm->registers.flags.bits.SF = (UT(res) >> (sizeof(T) * 8 - 1)) & 1;

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
    inline void alu_core_unary(ProcessVM *vm, T a, int dst_reg_index) {
        using UT = std::make_unsigned_t<T>;

        T result = Op::compute(a);

        vm->registers.flags.bits.ZF = (result == 0);

        constexpr int SIGNBIT       = sizeof(T) * 8 - 1;
        vm->registers.flags.bits.SF = (static_cast<UT>(result) >> SIGNBIT) & 1;

        Op::flags(vm, a, result);

        auto &dst = vm->registers.regs[dst_reg_index];

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
        static inline void flags(ProcessVM *vm, T a, T b, T result, bool is_signed) {
            using ST = std::make_signed_t<T>;
            using UT = std::make_unsigned_t<T>;

            if (is_signed) {
                ST sa                       = (ST) a, sb = (ST) b, sres = (ST) result;
                vm->registers.flags.bits.OF = ((sa ^ sres) & (sb ^ sres)) < 0;
                vm->registers.flags.bits.CF = 0;
            } else {
                UT ua                       = (UT) a, ub = (UT) b, ures = (UT) result;
                vm->registers.flags.bits.CF = (ua + ub) < ua;
                vm->registers.flags.bits.OF = 0;
            }
        }
    };


    struct MulOp {
        static constexpr bool is_compare = false;

        template<typename T>
        static inline T compute(T a, T b) {
            return a * b;
        }

        template<typename T>
        static inline void flags(ProcessVM *vm, T a, T b, T result, bool is_signed) {
            using UT = std::make_unsigned_t<T>;
            using ST = std::make_signed_t<T>;

            bool overflow = false;

            if (is_signed) {
                ST sa = (ST) a;
                ST sb = (ST) b;

                constexpr ST S_MIN = std::numeric_limits<ST>::min();
                constexpr ST S_MAX = std::numeric_limits<ST>::max();

                if (sa == 0 || sb == 0) {
                    overflow = false;
                } else if (sa == -1) {
                    overflow = (sb == S_MIN);
                } else if (sb == -1) {
                    overflow = (sa == S_MIN);
                } else {
                    overflow = (sa > 0)
                                   ? (sb > 0
                                          ? sa > S_MAX / sb
                                          : sb < S_MIN / sa)
                                   : (sb > 0
                                          ? sa < S_MIN / sb
                                          : sa < S_MAX / sb);
                }

                vm->registers.flags.bits.OF = overflow;
                vm->registers.flags.bits.CF = 0; // IMUL
            } else {
                UT ua = (UT) a;
                UT ub = (UT) b;

                if (ua == 0 || ub == 0) {
                    overflow = false;
                } else {
                    constexpr UT U_MAX = std::numeric_limits<UT>::max();
                    overflow           = ua > (U_MAX / ub);
                }

                vm->registers.flags.bits.CF = overflow;
                vm->registers.flags.bits.OF = overflow; // MUL
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
        static inline void flags(ProcessVM *vm, T a, T b, T result, bool is_signed) {
            using ST = std::make_signed_t<T>;
            using UT = std::make_unsigned_t<T>;

            if (is_signed) {
                ST sa                       = (ST) a, sb = (ST) b, sres = (ST) result;
                vm->registers.flags.bits.OF = ((sa ^ sb) & (sa ^ sres)) < 0;
                vm->registers.flags.bits.CF = 0;
            } else {
                UT ua                       = (UT) a, ub = (UT) b;
                vm->registers.flags.bits.CF = ua < ub;
                vm->registers.flags.bits.OF = 0;
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
        static inline void flags(ProcessVM *vm, T a, T b, T result, bool is_signed) {
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
        static inline void flags(ProcessVM *vm, T a, T b, T result, bool is_signed) {
            using ST = std::make_signed_t<T>;
            using UT = std::make_unsigned_t<T>;

            // División por cero
            if (b == 0) {
                vm->registers.flags.bits.OF = 1;
                vm->registers.flags.bits.CF = 1;
                //vm->registers.should_kill   = true;
                return;
            }

            if (is_signed) {
                ST sa = (ST) a;
                ST sb = (ST) b;

                // Overflow en signed DIV: INT_MIN / -1
                if (sa == std::numeric_limits<ST>::min() && sb == -1) {
                    vm->registers.flags.bits.OF = 1;
                    vm->registers.flags.bits.CF = 1;
                    //vm->should_kill   = true;
                    return;
                }

                vm->registers.flags.bits.OF = 0;
                vm->registers.flags.bits.CF = 0;
            } else {
                // Unsigned DIV nunca tiene overflow excepto por división por cero
                vm->registers.flags.bits.OF = 0;
                vm->registers.flags.bits.CF = 0;
            }
        }
    };


    // AND
    struct AndOp {
        static constexpr bool is_compare = false;

        template<typename T>
        static inline T compute(T a, T b) {
            return a & b;
        }

        template<typename T>
        static inline void flags(ProcessVM *vm, T, T, T, bool) {
            vm->registers.flags.bits.CF = 0;
            vm->registers.flags.bits.OF = 0;
        }
    };

    struct OrOp {
        static constexpr bool is_compare = false;

        template<typename T>
        static inline T compute(T a, T b) {
            return a | b;
        }

        template<typename T>
        static inline void flags(ProcessVM *vm, T, T, T, bool) {
            vm->registers.flags.bits.CF = 0;
            vm->registers.flags.bits.OF = 0;
        }
    };

    struct XorOp {
        static constexpr bool is_compare = false;

        template<typename T>
        static inline T compute(T a, T b) {
            return a ^ b;
        }

        template<typename T>
        static inline void flags(ProcessVM *vm, T, T, T, bool) {
            vm->registers.flags.bits.CF = 0;
            vm->registers.flags.bits.OF = 0;
        }
    };

    struct ShlOp {
        static constexpr bool is_compare = false;

        template<typename T>
        static inline T compute(T a, T b) {
            using UT       = std::make_unsigned_t<T>;
            uint32_t shift = (uint32_t) b & (sizeof(T) * 8 - 1);
            return (T) ((UT) a << shift);
        }

        template<typename T>
        static inline void flags(ProcessVM *vm, T a, T b, T result, bool) {
            using UT       = std::make_unsigned_t<T>;
            uint32_t shift = (uint32_t) b & (sizeof(T) * 8 - 1);

            if (shift == 0) {
                vm->registers.flags.bits.CF = 0;
                vm->registers.flags.bits.OF = 0;
                return;
            }

            UT ua                       = (UT) a;
            UT cf_bit                   = (ua >> (sizeof(T) * 8 - shift)) & 1;
            vm->registers.flags.bits.CF = cf_bit;

            // Overflow: SHL OF = XOR(CF, MSB(result))
            UT msb                      = (result >> (sizeof(T) * 8 - 1)) & 1;
            vm->registers.flags.bits.OF = (cf_bit ^ msb);
        }
    };

    struct ShrOp {
        static constexpr bool is_compare = false;

        template<typename T>
        static inline T compute(T a, T b) {
            using UT       = std::make_unsigned_t<T>;
            uint32_t shift = (uint32_t) b & (sizeof(T) * 8 - 1);
            return (T) ((UT) a >> shift);
        }

        template<typename T>
        static inline void flags(ProcessVM *vm, T a, T b, T result, bool) {
            using UT       = std::make_unsigned_t<T>;
            uint32_t shift = (uint32_t) b & (sizeof(T) * 8 - 1);

            if (shift == 0) {
                vm->registers.flags.bits.CF = 0;
                vm->registers.flags.bits.OF = 0;
                return;
            }

            UT ua                       = (UT) a;
            vm->registers.flags.bits.CF = (ua >> (shift - 1)) & 1;
            vm->registers.flags.bits.OF = 0;
        }
    };

    struct SarOp {
        static constexpr bool is_compare = false;

        template<typename T>
        static inline T compute(T a, T b) {
            using ST       = std::make_signed_t<T>;
            uint32_t shift = (uint32_t) b & (sizeof(T) * 8 - 1);
            return (T) ((ST) a >> shift);
        }

        template<typename T>
        static inline void flags(ProcessVM *vm, T a, T b, T result, bool) {
            using ST = std::make_signed_t<T>;
            using UT = std::make_unsigned_t<T>;

            uint32_t shift = (uint32_t) b & (sizeof(T) * 8 - 1);

            if (shift == 0) {
                vm->registers.flags.bits.CF = 0;
                vm->registers.flags.bits.OF = 0;
                return;
            }

            UT ua                       = (UT) a;
            vm->registers.flags.bits.CF = (ua >> (shift - 1)) & 1;
            vm->registers.flags.bits.OF = 0;
        }
    };


    typedef GeneralRegister &Reg;
    using BinaryFn = void(*)(ProcessVM *, Reg, Reg, bool, int);
    using UnaryFn  = void(*)(ProcessVM *, Reg, int);

    template<typename T, typename Op>
    static void binary_imm_wrapper(ProcessVM *vm, Reg &dst, uint64_t imm, bool is_signed, int rdst) {
        alu_core<T, Op>(vm, dst.raw(), (T) imm, is_signed, rdst);
    }

    template<typename T>
    static void mov_wrapper(ProcessVM *vm, Reg dst, Reg src, bool /*unused*/, int rdst) {
        T     value = src.raw(); // leer valor del registro origen
        auto &d     = vm->registers.regs[rdst];

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
    static void binary_wrapper(ProcessVM *vm, Reg &dst, Reg &src, bool is_signed, int rdst) {
        alu_core<T, Op>(vm, dst.raw(), src.raw(), is_signed, rdst);
    }

    template<typename T, typename Op>
    static void unary_wrapper(ProcessVM *vm, Reg &dst, int rdst) {
        alu_core_unary<T, Op>(vm, dst.raw(), rdst);
    }

    static constexpr BinaryFn mov_table[] = {
        &mov_wrapper<uint8_t>,
        &mov_wrapper<uint16_t>,
        &mov_wrapper<uint32_t>,
        &mov_wrapper<uint64_t>
    };

    static constexpr BinaryFn mul_table[] = {
        &binary_wrapper<uint8_t, MulOp>,
        &binary_wrapper<uint16_t, MulOp>,
        &binary_wrapper<uint32_t, MulOp>,
        &binary_wrapper<uint64_t, MulOp>
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

    using BinaryImmFn                            = void(*)(ProcessVM *, Reg &, uint64_t, bool, int);
    static constexpr BinaryImmFn add_imm_table[] = {
        &binary_imm_wrapper<uint8_t, AddOp>,
        &binary_imm_wrapper<uint16_t, AddOp>,
        &binary_imm_wrapper<uint32_t, AddOp>,
        &binary_imm_wrapper<uint64_t, AddOp>
    };

    using BinaryMemImmFn = void(*)(ProcessVM *, uint64_t, uint64_t, bool);

    static constexpr BinaryMemImmFn add_mem_imm_table[] = {
        &binary_mem_imm_wrapper<uint8_t, AddOp>,
        &binary_mem_imm_wrapper<uint16_t, AddOp>,
        &binary_mem_imm_wrapper<uint32_t, AddOp>,
        &binary_mem_imm_wrapper<uint64_t, AddOp>
    };

    static constexpr BinaryImmFn sub_imm_table[] = {
        &binary_imm_wrapper<uint8_t, SubOp>,
        &binary_imm_wrapper<uint16_t, SubOp>,
        &binary_imm_wrapper<uint32_t, SubOp>,
        &binary_imm_wrapper<uint64_t, SubOp>
    };
    static constexpr BinaryMemImmFn sub_mem_imm_table[] = {
        &binary_mem_imm_wrapper<uint8_t, SubOp>,
        &binary_mem_imm_wrapper<uint16_t, SubOp>,
        &binary_mem_imm_wrapper<uint32_t, SubOp>,
        &binary_mem_imm_wrapper<uint64_t, SubOp>
    };

    static constexpr BinaryImmFn mul_imm_table[] = {
        &binary_imm_wrapper<uint8_t, MulOp>,
        &binary_imm_wrapper<uint16_t, MulOp>,
        &binary_imm_wrapper<uint32_t, MulOp>,
        &binary_imm_wrapper<uint64_t, MulOp>
    };
    static constexpr BinaryMemImmFn mul_mem_imm_table[] = {
        &binary_mem_imm_wrapper<uint8_t, MulOp>,
        &binary_mem_imm_wrapper<uint16_t, MulOp>,
        &binary_mem_imm_wrapper<uint32_t, MulOp>,
        &binary_mem_imm_wrapper<uint64_t, MulOp>
    };

    static constexpr BinaryImmFn div_imm_table[] = {
        &binary_imm_wrapper<uint8_t, DivOp>,
        &binary_imm_wrapper<uint16_t, DivOp>,
        &binary_imm_wrapper<uint32_t, DivOp>,
        &binary_imm_wrapper<uint64_t, DivOp>
    };
    static constexpr BinaryMemImmFn div_mem_imm_table[] = {
        &binary_mem_imm_wrapper<uint8_t, DivOp>,
        &binary_mem_imm_wrapper<uint16_t, DivOp>,
        &binary_mem_imm_wrapper<uint32_t, DivOp>,
        &binary_mem_imm_wrapper<uint64_t, DivOp>
    };

    static constexpr BinaryImmFn cmp_imm_table[] = {
        &binary_imm_wrapper<uint8_t, CmpOp>,
        &binary_imm_wrapper<uint16_t, CmpOp>,
        &binary_imm_wrapper<uint32_t, CmpOp>,
        &binary_imm_wrapper<uint64_t, CmpOp>
    };
    static constexpr BinaryMemImmFn cmp_mem_imm_table[] = {
        &binary_mem_imm_wrapper<uint8_t, CmpOp>,
        &binary_mem_imm_wrapper<uint16_t, CmpOp>,
        &binary_mem_imm_wrapper<uint32_t, CmpOp>,
        &binary_mem_imm_wrapper<uint64_t, CmpOp>
    };


    void exec_instr_add_imm(ProcessVM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.inmmed_data.reg;
        uint64_t  imm  = instr.data_instruction.inmmed_data.inmmed;

        // Caso 1: destino es un registro
        if (instr.flags_info.direction == 0) {
            add_imm_table[instr.flags_info.mode](
                vm,
                vm->registers.regs[rdst],
                imm,
                instr.flags_info._signed_instruct,
                rdst
            );
            return;
        }

        // Caso 2: destino es memoria
        if (instr.flags_info.direction == 1) {
            auto &  base  = vm->registers.regs[instr.data_instruction.inmmed_data.reg];
            auto    index = 0;
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

    }



#define DEFINE_IMM_EXEC(name, imm_tbl, mem_tbl)                                                      \
    void exec_instr_##name##_imm(ProcessVM *vm, const DecodedInstr &instr) {                         \
        const int rdst = instr.data_instruction.inmmed_data.reg;                                     \
        uint64_t  imm  = instr.data_instruction.inmmed_data.inmmed;                                  \
        if (instr.flags_info.direction == 0) {                                                       \
            imm_tbl[instr.flags_info.mode](vm, vm->registers.regs[rdst], imm,                        \
                instr.flags_info._signed_instruct, rdst);                                            \
            return;                                                                                  \
        }                                                                                            \
        uint64_t addr = vm->registers.regs[rdst].raw();                                              \
        mem_tbl[instr.flags_info.mode](vm, addr, imm, instr.flags_info._signed_instruct);            \
    }

    DEFINE_IMM_EXEC(sub, sub_imm_table, sub_mem_imm_table)
    DEFINE_IMM_EXEC(mul, mul_imm_table, mul_mem_imm_table)
    DEFINE_IMM_EXEC(div, div_imm_table, div_mem_imm_table)
    DEFINE_IMM_EXEC(cmp, cmp_imm_table, cmp_mem_imm_table)

#undef DEFINE_IMM_EXEC

    void exec_instr_mov_reg(ProcessVM *vm, const DecodedInstr &instr) {
        uint8_t s    = instr.flags_info._signed_instruct;
        uint8_t d    = instr.flags_info.direction;
        uint8_t mode = instr.flags_info.mode;
        uint8_t reg1 = instr.data_instruction.reg_data.reg1;
        uint8_t reg2 = instr.data_instruction.reg_data.reg2;

        if (s == 0) {
            // standard reg, reg
            mov_table[mode](vm, vm->registers.regs[reg1], vm->registers.regs[reg2], false, reg1);
            return;
        }

        // s=1: reg_ext variant - special = (mode<<4) | reg2
        uint8_t special = (uint8_t)((mode << 4) | (reg2 & 0xF));
        if (d == 0) {
            // MOV reg_ext, reg  (write special <- general)
            write_special(vm, special, vm->registers.regs[reg1].raw());
        } else {
            // MOV reg, reg_ext  (write general <- special)
            uint64_t val = read_special(vm, special);
            GeneralRegister tmp; tmp.qword(val);
            mov_table[mode](vm, vm->registers.regs[reg1], tmp, false, reg1);
        }
    }

    void exec_instr_add_reg(ProcessVM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.reg_data.reg1;
        const int rsrc = instr.data_instruction.reg_data.reg2;

        add_table[instr.flags_info.mode](vm, vm->registers.regs[rdst], vm->registers.regs[rsrc], instr.flags_info._signed_instruct, rdst);
    }

    void exec_instr_mul_reg(ProcessVM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.reg_data.reg1;
        const int rsrc = instr.data_instruction.reg_data.reg2;

        mul_table[instr.flags_info.mode](vm, vm->registers.regs[rdst], vm->registers.regs[rsrc], instr.flags_info._signed_instruct, rdst);
    }

    void exec_instr_sub_reg(ProcessVM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.reg_data.reg1;
        const int rsrc = instr.data_instruction.reg_data.reg2;

        sub_table[instr.flags_info.mode](vm, vm->registers.regs[rdst], vm->registers.regs[rsrc], instr.flags_info._signed_instruct, rdst);
    }

    void exec_instr_cmp_reg(ProcessVM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.reg_data.reg1;
        const int rsrc = instr.data_instruction.reg_data.reg2;

        cmp_table[instr.flags_info.mode](vm, vm->registers.regs[rdst], vm->registers.regs[rsrc], instr.flags_info._signed_instruct, rdst);
    }

    void exec_instr_and_reg(ProcessVM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.reg_data.reg1;
        const int rsrc = instr.data_instruction.reg_data.reg2;
        and_table[instr.flags_info.mode](vm, vm->registers.regs[rdst], vm->registers.regs[rsrc], false, rdst);
    }

    void exec_instr_or_reg(ProcessVM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.reg_data.reg1;
        const int rsrc = instr.data_instruction.reg_data.reg2;
        or_table[instr.flags_info.mode](vm, vm->registers.regs[rdst], vm->registers.regs[rsrc], false, rdst);
    }

    void exec_instr_xor_reg(ProcessVM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.reg_data.reg1;
        const int rsrc = instr.data_instruction.reg_data.reg2;
        xor_table[instr.flags_info.mode](vm, vm->registers.regs[rdst], vm->registers.regs[rsrc], false, rdst);
    }

    void exec_instr_div_reg(ProcessVM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.reg_data.reg1;
        const int rsrc = instr.data_instruction.reg_data.reg2;
        div_table[instr.flags_info.mode](vm, vm->registers.regs[rdst], vm->registers.regs[rsrc], instr.flags_info._signed_instruct, rdst);
    }

    void exec_instr_shl_reg(ProcessVM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.reg_data.reg1;
        const int rsrc = instr.data_instruction.reg_data.reg2;
        shl_table[instr.flags_info.mode](vm, vm->registers.regs[rdst], vm->registers.regs[rsrc], false, rdst);
    }

    void exec_instr_shr_reg(ProcessVM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.reg_data.reg1;
        const int rsrc = instr.data_instruction.reg_data.reg2;
        shr_table[instr.flags_info.mode](vm, vm->registers.regs[rdst], vm->registers.regs[rsrc], false, rdst);
    }

    void exec_instr_sar_reg(ProcessVM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.reg_data.reg1;
        const int rsrc = instr.data_instruction.reg_data.reg2;
        sar_table[instr.flags_info.mode](vm, vm->registers.regs[rdst], vm->registers.regs[rsrc], false, rdst);
    }

    struct NotOp {
        static constexpr bool is_compare = false;

        template<typename T>
        static inline T compute(T a) {
            return ~a;
        }

        template<typename T>
        static inline void flags(ProcessVM *vm, T /*a*/, T /*result*/) {
            vm->registers.flags.bits.CF = 0;
            vm->registers.flags.bits.OF = 0;
        }
    };

    static constexpr UnaryFn not_table[] = {
        &unary_wrapper<uint8_t,  NotOp>,
        &unary_wrapper<uint16_t, NotOp>,
        &unary_wrapper<uint32_t, NotOp>,
        &unary_wrapper<uint64_t, NotOp>
    };

    void exec_instr_not_reg(ProcessVM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.reg_data.reg1;
        not_table[instr.flags_info.mode](vm, vm->registers.regs[rdst], rdst);
    }

    // -------------------------------------------------------------------------
    // SIB helpers
    // -------------------------------------------------------------------------

    inline uint64_t sib_effective_addr(ProcessVM *vm, const DecodedInstr &instr) {
        uint64_t base      = vm->registers.regs[instr.data_instruction.mem_data.reg_base].raw();
        uint8_t  has_index = (instr.data_instruction.mem_data.scale >> 2) & 1;
        if (!has_index) return base;
        uint64_t index = vm->registers.regs[instr.data_instruction.mem_data.reg_index].raw();
        uint8_t  scale = instr.data_instruction.mem_data.scale & 0x3; // 0=*1, 1=*2, 2=*4, 3=*8
        return base + (index << scale);
    }

    // direction=0: dst_reg = dst_reg Op mem[sib]
    template<typename T, typename Op>
    static void sib_reg_dst_wrapper(ProcessVM *vm, uint64_t addr, uint64_t dst_val, bool is_signed, int dst_reg) {
        T src = vm->vm_mem.read_any<T>(addr);
        alu_core<T, Op>(vm, (T)dst_val, src, is_signed, dst_reg);
    }

    // direction=1: mem[sib] = mem[sib] Op reg
    template<typename T, typename Op>
    static void sib_mem_dst_wrapper(ProcessVM *vm, uint64_t addr, uint64_t reg_val, bool is_signed, int) {
        using UT = std::make_unsigned_t<T>;
        T mem_val = vm->vm_mem.read_any<T>(addr);
        T res     = Op::compute(mem_val, (T)reg_val);
        vm->registers.flags.bits.ZF = (res == 0);
        vm->registers.flags.bits.SF = (UT(res) >> (sizeof(T)*8-1)) & 1;
        Op::flags(vm, mem_val, (T)reg_val, res, is_signed);
        if constexpr (!Op::is_compare)
            vm->vm_mem.write_any<T>(addr, res);
    }

    using SIBFn = void(*)(ProcessVM *, uint64_t, uint64_t, bool, int);

    // direction=0: mov dst_reg = mem[sib]
    template<typename T>
    static void sib_mov_to_reg(ProcessVM *vm, uint64_t addr, uint64_t, bool, int dst_reg) {
        T val   = vm->vm_mem.read_any<T>(addr);
        auto &d = vm->registers.regs[dst_reg];
        if constexpr (sizeof(T)==1) d.byte_lo((uint8_t)val);
        else if constexpr (sizeof(T)==2) d.word_lo((uint16_t)val);
        else if constexpr (sizeof(T)==4) d.dword_lo((uint32_t)val);
        else d.qword((uint64_t)val);
    }

    // direction=1: mov mem[sib] = reg
    template<typename T>
    static void sib_mov_to_mem(ProcessVM *vm, uint64_t addr, uint64_t reg_val, bool, int) {
        vm->vm_mem.write_any<T>(addr, (T)reg_val);
    }

    template<typename Op>
    static void exec_sib_generic(ProcessVM *vm, const DecodedInstr &instr,
                                  const SIBFn reg_dst[4], const SIBFn mem_dst[4]) {
        const int     dst  = instr.data_instruction.mem_data.reg_final;
        uint64_t      addr = sib_effective_addr(vm, instr);
        uint64_t      rval = vm->registers.regs[dst].raw();
        bool          sign = instr.flags_info._signed_instruct;
        int           mode = instr.flags_info.mode;

        if (instr.flags_info.direction == 0)
            reg_dst[mode](vm, addr, rval, sign, dst);
        else
            mem_dst[mode](vm, addr, rval, sign, dst);
    }

#define DECL_SIB_TABLES(name, Op) \
    static constexpr SIBFn name##_sib_reg_dst[] = { \
        &sib_reg_dst_wrapper<uint8_t,  Op>, &sib_reg_dst_wrapper<uint16_t, Op>, \
        &sib_reg_dst_wrapper<uint32_t, Op>, &sib_reg_dst_wrapper<uint64_t, Op>  \
    }; \
    static constexpr SIBFn name##_sib_mem_dst[] = { \
        &sib_mem_dst_wrapper<uint8_t,  Op>, &sib_mem_dst_wrapper<uint16_t, Op>, \
        &sib_mem_dst_wrapper<uint32_t, Op>, &sib_mem_dst_wrapper<uint64_t, Op>  \
    }

    DECL_SIB_TABLES(add, AddOp);
    DECL_SIB_TABLES(sub, SubOp);
    DECL_SIB_TABLES(mul, MulOp);
    DECL_SIB_TABLES(div, DivOp);
    DECL_SIB_TABLES(cmp, CmpOp);

    static constexpr SIBFn mov_sib_to_reg_table[] = {
        &sib_mov_to_reg<uint8_t>, &sib_mov_to_reg<uint16_t>,
        &sib_mov_to_reg<uint32_t>, &sib_mov_to_reg<uint64_t>
    };
    static constexpr SIBFn mov_sib_to_mem_table[] = {
        &sib_mov_to_mem<uint8_t>, &sib_mov_to_mem<uint16_t>,
        &sib_mov_to_mem<uint32_t>, &sib_mov_to_mem<uint64_t>
    };

    // MOVH: acceso a memoria del host (puntero nativo, no memoria virtual de la VM)
    template<typename T>
    static void movh_to_reg(ProcessVM *vm, uint64_t addr, uint64_t, bool, int dst_reg) {
        T val   = *reinterpret_cast<const T*>(addr);
        auto &d = vm->registers.regs[dst_reg];
        if constexpr (sizeof(T)==1) d.byte_lo((uint8_t)val);
        else if constexpr (sizeof(T)==2) d.word_lo((uint16_t)val);
        else if constexpr (sizeof(T)==4) d.dword_lo((uint32_t)val);
        else d.qword((uint64_t)val);
    }

    template<typename T>
    static void movh_to_mem(ProcessVM *, uint64_t addr, uint64_t reg_val, bool, int) {
        *reinterpret_cast<T*>(addr) = static_cast<T>(reg_val);
    }

    static constexpr SIBFn movh_to_reg_table[] = {
        &movh_to_reg<uint8_t>, &movh_to_reg<uint16_t>,
        &movh_to_reg<uint32_t>, &movh_to_reg<uint64_t>
    };
    static constexpr SIBFn movh_to_mem_table[] = {
        &movh_to_mem<uint8_t>, &movh_to_mem<uint16_t>,
        &movh_to_mem<uint32_t>, &movh_to_mem<uint64_t>
    };

    // Lee el flag indicado por flag_code (0=SF,1=ZF,2=CF,3=OF,4=DM)
    inline bool read_flag(ProcessVM *vm, uint8_t flag_code) {
        switch (flag_code & 0x7) {
            case 0: return vm->registers.flags.bits.SF;
            case 1: return vm->registers.flags.bits.ZF;
            case 2: return vm->registers.flags.bits.CF;
            case 3: return vm->registers.flags.bits.OF;
            case 4: return vm->registers.flags.bits.DM;
            default: return false;
        }
    }

    void exec_instr_add_sib(ProcessVM *vm, const DecodedInstr &instr) {
        exec_sib_generic<AddOp>(vm, instr, add_sib_reg_dst, add_sib_mem_dst);
    }
    void exec_instr_sub_sib(ProcessVM *vm, const DecodedInstr &instr) {
        exec_sib_generic<SubOp>(vm, instr, sub_sib_reg_dst, sub_sib_mem_dst);
    }
    void exec_instr_mul_sib(ProcessVM *vm, const DecodedInstr &instr) {
        exec_sib_generic<MulOp>(vm, instr, mul_sib_reg_dst, mul_sib_mem_dst);
    }
    void exec_instr_div_sib(ProcessVM *vm, const DecodedInstr &instr) {
        exec_sib_generic<DivOp>(vm, instr, div_sib_reg_dst, div_sib_mem_dst);
    }
    void exec_instr_cmp_sib(ProcessVM *vm, const DecodedInstr &instr) {
        exec_sib_generic<CmpOp>(vm, instr, cmp_sib_reg_dst, cmp_sib_mem_dst);
    }
    void exec_instr_mov_sib(ProcessVM *vm, const DecodedInstr &instr) {
        const int dst  = instr.data_instruction.mem_data.reg_final;
        uint64_t  addr = sib_effective_addr(vm, instr);
        uint64_t  rval = vm->registers.regs[dst].raw();
        int       mode = instr.flags_info.mode;
        bool      host = instr.flags_info._signed_instruct; // s=1 -> MOVH

        if (!host) {
            if (instr.flags_info.direction == 0)
                mov_sib_to_reg_table[mode](vm, addr, rval, false, dst);
            else
                mov_sib_to_mem_table[mode](vm, addr, rval, false, dst);
        } else {
            if (instr.flags_info.direction == 0)
                movh_to_reg_table[mode](vm, addr, rval, false, dst);
            else
                movh_to_mem_table[mode](vm, addr, rval, false, dst);
        }
    }

    // MOVC/MOVCH con operando memoria: [0x00][0x1E][ctrl][byte4]
    // ctrl: host(1)|host(1)|d(1)|reg1(5)   (bits 7-6 = 0b10 -> MOVCH, 0b00 -> MOVC)
    // byte4: flag(3)|reg2(5)
    void exec_instr_movc_mem(ProcessVM *vm, const DecodedInstr &instr) {
        uint8_t flag_code = instr.data_instruction.reg_data.reg2;   // campo flag
        uint8_t reg1      = instr.data_instruction.reg_data.reg1;
        uint8_t reg2      = instr.data_instruction.inmmed_data.reg; // reg2 fuente
        bool    host      = instr.flags_info._signed_instruct;      // 0=MOVC 1=MOVCH
        bool    dir       = instr.flags_info.direction;             // 0=[reg2]->reg1, 1=reg2->[reg1]

        if (!read_flag(vm, flag_code)) return;

        uint64_t addr = vm->registers.regs[dir ? reg1 : reg2].raw();
        uint64_t val  = vm->registers.regs[dir ? reg2 : reg1].raw();

        if (!host) {
            if (!dir) {
                uint64_t v = vm->vm_mem.read_u64(addr);
                vm->registers.regs[reg1].qword(v);
            } else {
                vm->vm_mem.write_u64(addr, val);
            }
        } else {
            if (!dir) {
                uint64_t v = *reinterpret_cast<const uint64_t*>(addr);
                vm->registers.regs[reg1].qword(v);
            } else {
                *reinterpret_cast<uint64_t*>(addr) = val;
            }
        }
    }

    // MOVC reg1, reg2, flag: [0x00][0x1F][ctrl][byte4]
    // ctrl: 0b00|0|reg1   byte4: flag(3)|reg2(5)
    void exec_instr_movc_reg(ProcessVM *vm, const DecodedInstr &instr) {
        uint8_t flag_code = instr.data_instruction.reg_data.reg2;
        uint8_t reg1      = instr.data_instruction.reg_data.reg1;
        uint8_t reg2      = instr.data_instruction.inmmed_data.reg;

        if (!read_flag(vm, flag_code)) return;

        GeneralRegister tmp; tmp.qword(vm->registers.regs[reg2].raw());
        mov_table[instr.flags_info.mode](vm, vm->registers.regs[reg1], tmp, false, reg1);
    }

    void exec_instr_inc_dec_reg(ProcessVM *vm, const DecodedInstr &instr) {
        const int rdst = instr.data_instruction.reg_data.reg1;
        // aunque INC y DEC no tienen signo, se usa el campo _signed_instruct
        // en la descodificacion para almacenar si es la instruccion INC o DEC.
        if (instr.flags_info._signed_instruct == 0)
            inc_table[instr.flags_info.mode](vm, vm->registers.regs[rdst], rdst);
        else
            dec_table[instr.flags_info.mode](vm, vm->registers.regs[rdst], rdst);
    }
}
