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

#ifndef PARSER_TO_BYTECODE_H
#define PARSER_TO_BYTECODE_H

#include "emmit_decl.h"

/**
 * La conversion de AST generado por al parser a bytecode usa 3 etapas:
 *      - Primera pasada: recolectar símbolos (labels, offsets, tamaños)
 *      - Segunda pasada: evaluar expresiones y generar datos (expresiones es 2+3 Por ejemplo)
 *      - Tercera pasada: generar instrucciones y resolver saltos
 */



namespace Assembly::Bytecode {


    /**
     * Tabla de directivas, estas deben ir siempre en el inicio del programa
     */
    static const std::unordered_set<std::string> PseudoInstructions = {
        "global",
        "extern",
        "bits",
        "align",
        "org"
    };

    /**
     * Tabla de instrucciones con metadatos correspondientes
     */
    static const std::unordered_map<std::string, std::vector<InstrInfo> > InstrTable = {
        {
            "vminfo", {{0x01, 0x00, InstrSizeMode::FIXED_2}
            }
        },
        {"vminfomanager", {{0x02, 0x00, InstrSizeMode::FIXED_2}}},

        // INC y DEC usan la misma subrutina de emision por que se codifcan igual, cambiando solo el
        // segundo byte
        {"inc", {{0x04, 0x00, InstrSizeMode::FIXED_2, .emit = emit_inc}}},
        {"dec", {{0x04, 0x00, InstrSizeMode::FIXED_2, .emit = emit_inc}}},


        {
            "callvm", {
                {0x10, 0x00, InstrSizeMode::FIXED_8}, // CALLVM   <addr56bits>
                {0x00, 0x22, InstrSizeMode::FIXED_4}, // CALLVM   <reg>
            }
        },
        {
            "jmp", {
                {0x11, 0x00, InstrSizeMode::FIXED_8}, // jmp   <addr56bits>
                {0x00, 0x22, InstrSizeMode::FIXED_4}, // jmp   <reg>
            }
        },
        {"push", {{0x12, 0x00, InstrSizeMode::FIXED_2}}},
        {"pop", {{0x13, 0x00, InstrSizeMode::FIXED_2}}},

        {"nop1", {{0x33, 0x00, InstrSizeMode::FIXED_1}}},
        {"nop2", {{0x33, 0x00, InstrSizeMode::FIXED_2}}},
        {"ret", {{0xC3, 0x00, InstrSizeMode::FIXED_1}}},

        // Extensión (opcode1 = 0x00)
        {"edmw4", {{0x00, 0x00, InstrSizeMode::FIXED_4}}},
        {"edmw6", {{0x00, 0x01, InstrSizeMode::FIXED_4}}},
        {"edm", {{0x00, 0x02, InstrSizeMode::FIXED_2}}},
        {"hlt", {{0x00, 0x03, InstrSizeMode::FIXED_2}}},


        /**
         * ADD tiene 3 variantes.
         * buscar con:
         * auto& variants = InstrTable["add"];
         *
         * for (auto& v : variants) {
         *      if (matches_operands(v.sizeMode, operands))
         *      return v; // esta es la variante correcta
         * }
         */
        {
            "adds", {
                // addu/adds
                // reg, reg
                {0x00, 0x05, InstrSizeMode::FIXED_4},

                // reg, [mem] || [mem], reg
                {0x00, 0x06, InstrSizeMode::FIXED_8},

                // REG, SIB || SIB, REG
                {0x00, 0x07, InstrSizeMode::FIXED_4}
            }
        },
        {
            "addu", {
                // addu
                // reg, reg
                {0x00, 0x05, InstrSizeMode::FIXED_4},

                // reg, [mem] || [mem], reg
                {0x00, 0x06, InstrSizeMode::FIXED_8},

                // REG, SIB || SIB, REG
                {0x00, 0x07, InstrSizeMode::FIXED_4}
            }
        },

        {
            "subu", {
                // subu/subs
                // reg, reg
                {0x00, 0x08, InstrSizeMode::FIXED_4},

                // reg, [mem] || [mem], reg
                {0x00, 0x09, InstrSizeMode::FIXED_8},

                // REG, SIB || SIB, REG
                {0x00, 0x0A, InstrSizeMode::FIXED_4}
            }
        },
        {
            "subs", {
                // subu/subs
                // reg, reg
                {0x00, 0x08, InstrSizeMode::FIXED_4},

                // reg, [mem] || [mem], reg
                {0x00, 0x09, InstrSizeMode::FIXED_8},

                // REG, SIB || SIB, REG
                {0x00, 0x0A, InstrSizeMode::FIXED_4}
            }
        },
        {
            "muls", {
                // mulu/muls
                // reg, reg
                {0x00, 0x0B, InstrSizeMode::FIXED_4},

                // reg, [mem] || [mem], reg
                {0x00, 0x0C, InstrSizeMode::FIXED_8},

                // REG, SIB || SIB, REG
                {0x00, 0x0D, InstrSizeMode::FIXED_4}
            }
        },
        {
            "mulu", {
                // mulu/muls
                // reg, reg
                {0x00, 0x0B, InstrSizeMode::FIXED_4},

                // reg, [mem] || [mem], reg
                {0x00, 0x0C, InstrSizeMode::FIXED_8},

                // REG, SIB || SIB, REG
                {0x00, 0x0D, InstrSizeMode::FIXED_4}
            }
        },
        {
            "divu", {
                // divu/divs
                // reg, reg
                {0x00, 0x0E, InstrSizeMode::FIXED_4},

                // reg, [mem] || [mem], reg
                {0x00, 0x0F, InstrSizeMode::FIXED_8},

                // REG, SIB || SIB, REG
                {0x00, 0x10, InstrSizeMode::FIXED_4}
            }
        },
        {
            "divs", {
                // divu/divs
                // reg, reg
                {0x00, 0x0E, InstrSizeMode::FIXED_4},

                // reg, [mem] || [mem], reg
                {0x00, 0x0F, InstrSizeMode::FIXED_8},

                // REG, SIB || SIB, REG
                {0x00, 0x10, InstrSizeMode::FIXED_4}
            }
        },
        {
            "cmpu", {
                // cmpu/cmps
                // reg, reg
                {0x00, 0x11, InstrSizeMode::FIXED_4},

                // reg, [mem] || [mem], reg
                {0x00, 0x12, InstrSizeMode::FIXED_8},

                // REG, SIB || SIB, REG
                {0x00, 0x13, InstrSizeMode::FIXED_4},
            },
        },
        {
            "cmps", {
                // cmpu/cmps
                // reg, reg
                {0x00, 0x11, InstrSizeMode::FIXED_4},

                // reg, [mem] || [mem], reg
                {0x00, 0x12, InstrSizeMode::FIXED_8},

                // REG, SIB || SIB, REG
                {0x00, 0x13, InstrSizeMode::FIXED_4},
            },
        },

        {
            "mov", {
                // por definir
                // mov
                // reg, reg
                {0x00, 0x14, InstrSizeMode::FIXED_4},

                // reg, [mem] || [mem], reg
                {0x00, 0x15, InstrSizeMode::FIXED_8},

                // REG, SIB || SIB, REG
                {0x00, 0x16, InstrSizeMode::FIXED_4},
            },
        },
        {
            "loop", {
                {0x00, 0x31, InstrSizeMode::FIXED_8},
            },
        },

    };


    /**
     * @class Assembler
     * @brief Convierte un AST generado por el parser en bytecode ejecutable para la VM.
     *
     * El ensamblado se realiza en 3 fases:
     *  - Primera pasada: recolección de símbolos (labels, offsets, tamaños).
     *  - Segunda pasada: evaluación de expresiones y emisión de datos.
     *  - Tercera pasada: emisión de instrucciones y resolución de saltos.
     */
    class Assembler {
    public:
        /**
         * @brief Dirección base por defecto donde se ensamblará el código.
         *
         * Puede inicializarse con la dirección base del espacio de direcciones
         * de la VM
         */
        uint64_t default_address;

        /**
         * @brief Constructor del ensamblador.
         *
         * Inicializa las estructuras internas, limpia buffers y establece
         * la dirección base por defecto.
         */
        Assembler();

        /**
         * @brief Ensambla un AST completo en un buffer de bytecode.
         *
         * Ejecuta las 3 fases del ensamblado:
         *  1. Recolección de símbolos.
         *  2. Emisión de datos.
         *  3. Emisión de instrucciones.
         *
         * @param ast Lista de nodos raíz del AST.
         * @return Vector de bytes con el bytecode final.
         */
        std::vector<uint8_t> assemble(const std::vector<std::unique_ptr<vm::ASTNode> > &ast);

        /**
         * @brief Obtiene el tamaño en bytes de una directiva de datos.
         *
         * @param dir Nombre de la directiva (db, dw, dd, dq, ptr).
         * @return Tamaño en bytes de un elemento de esa directiva.
         */
        size_t size_of_directive(const std::string &dir) const;

        /**
         * @brief Emite un valor numérico según la directiva especificada.
         *
         * El valor se escribe en little-endian y se avanza el offset interno.
         *
         * @param dir Directiva de datos (db, dw, dd, dq, ptr).
         * @param value Valor numérico ya evaluado.
         */
        void emit_directive(const std::string &dir, uint64_t value);

        /**
         * @brief Emite los datos de una declaración de datos (DataDecl).
         *
         * Las cadenas se emiten carácter a carácter.
         * Las expresiones numéricas se evalúan y se emiten con emit_directive().
         *
         * @param data Nodo DataDecl del AST.
         */
        void emit_data(const vm::DataDecl *data);

        /**
         * @brief Selecciona la variante correcta de una instrucción según sus operandos.
         *
         * Cada mnemónico puede tener múltiples variantes (diferentes opcodes y tamaños).
         * Esta función elige la variante adecuada.
         *
         * @param mnemonic Nombre de la instrucción (add, sub, jmp, etc.).
         * @param ops Lista de operandos de la instrucción.
         * @return Referencia a la variante seleccionada.
         */
        const InstrInfo &select_variant(const std::string &                               mnemonic,
                                        const std::vector<std::unique_ptr<vm::ASTNode> > &ops);

        /**
         * @brief Calcula el tamaño en bytes de una instrucción.
         *
         * Usa la variante seleccionada y su modo de tamaño (FIXED_1, FIXED_2, etc.).
         *
         * @param instr Nodo Instruction del AST.
         * @return Tamaño en bytes de la instrucción.
         */
        size_t size_of_instruction(const vm::Instruction *instr) const;

        /**
         * @brief Emite una instrucción al buffer de salida.
         *
         * Escribe opcode1, opcionalmente opcode2, y luego los operandos
         * según el formato de la VM.
         *
         * @param instr Nodo Instruction del AST.
         */
        void emit_instruction(const vm::Instruction *instr);

        uint64_t eval_operand(const vm::ASTNode *op);

        /**
         * @brief Evalúa una expresión del AST y devuelve su valor numérico.
         *
         * Soporta:
         *  - Literales numéricos
         *  - Labels
         *  - Expresiones binarias (+, -, *, /)
         *
         * @param expr Nodo de expresión.
         * @return Valor numérico resultante.
         */
        uint64_t eval_expr(vm::ExprNode *expr);

    private:
        /**
         * @brief Primera pasada: recolección de símbolos.
         *
         * Asigna direcciones a labels, datos y bloques de código.
         *
         * @param node Nodo actual del AST.
         * @param offset Offset acumulado.
         */
        void first_pass(const vm::ASTNode *node, uint64_t &offset);

        void apply_directive(const vm::Instruction *instr);

        /**
         * @brief Segunda pasada: emisión de datos.
         *
         * Recorre el AST y emite todas las declaraciones de datos.
         *
         * @param node Nodo actual del AST.
         */
        void second_pass_data(const vm::ASTNode *node);

        /**
         * @brief Tercera pasada: emisión de instrucciones.
         *
         * Recorre el AST y emite todas las instrucciones.
         *
         * @param node Nodo actual del AST.
         */
        void third_pass_code(const vm::ASTNode *node);



        /// Tabla de símbolos generada en la primera pasada.
        std::unordered_map<std::string, uint64_t> symbol_table;

        /// Buffer de salida donde se escribe el bytecode final.
        std::vector<uint8_t> output;

        /// Offset actual dentro del buffer de salida.
        uint64_t current_offset = 0;
    };
}

#endif //PARSER_TO_BYTECODE_H
