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
#include "annotations.h"

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
        {"vminfo", {{0x01, 0x00, InstrSizeMode::FIXED_2, AddressingMode::NONE, nullptr}}},
        {"vminfomanager", {{0x02, 0x00, InstrSizeMode::FIXED_2, AddressingMode::NONE, nullptr}}},

        // INC y DEC usan la misma subrutina de emision por que se codifcan igual, cambiando solo el
        // segundo byte
        {"inc", {{0x04, 0x00, InstrSizeMode::FIXED_2, AddressingMode::REG, emit_inc_dec}}},
        {"dec", {{0x04, 0x00, InstrSizeMode::FIXED_2, AddressingMode::REG, emit_inc_dec}}},

        // callvm <label|addr> - llama a función interna empujando retorno en pila
        {"callvm",  {{0x10, 0x00, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        // callvmr <reg> - igual que callvm pero la dirección viene de un registro
        {"callvmr", {{0x16, 0x00, InstrSizeMode::FIXED_2,  AddressingMode::REG,   emit_pop_push}}},

        // jmp incondicional
        {"jmp",     {{0x11, 0x0F, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        // jmp condicionales (sufijo determina condición, opcode2 = código de condición)
        {"jmp.je",  {{0x11, 0x00, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        {"jmp.jz",  {{0x11, 0x00, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        {"jmp.jne", {{0x11, 0x01, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        {"jmp.jnz", {{0x11, 0x01, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        {"jmp.jcs", {{0x11, 0x02, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        {"jmp.jae", {{0x11, 0x02, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        {"jmp.jcc", {{0x11, 0x03, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        {"jmp.jb",  {{0x11, 0x03, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        {"jmp.jmi", {{0x11, 0x04, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        {"jmp.jpl", {{0x11, 0x05, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        {"jmp.jvs", {{0x11, 0x06, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        {"jmp.jvc", {{0x11, 0x07, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        {"jmp.jhi", {{0x11, 0x08, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        {"jmp.jls", {{0x11, 0x09, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        {"jmp.jge", {{0x11, 0x0A, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        {"jmp.jlt", {{0x11, 0x0B, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        {"jmp.jgt", {{0x11, 0x0C, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        {"jmp.jle", {{0x11, 0x0D, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},

        // jmpr <reg> - salto incondicional por registro
        {"jmpr",    {{0x15, 0x00, InstrSizeMode::FIXED_2,  AddressingMode::REG,   emit_pop_push}}},

        // jrel - salto relativo con desplazamiento de 32 bits (opcode extendido 0x00 0x2D)
        {"jrel",     {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},
        {"jrel.je",  {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},
        {"jrel.jz",  {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},
        {"jrel.jne", {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},
        {"jrel.jnz", {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},
        {"jrel.jcs", {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},
        {"jrel.jae", {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},
        {"jrel.jcc", {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},
        {"jrel.jb",  {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},
        {"jrel.jmi", {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},
        {"jrel.jpl", {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},
        {"jrel.jvs", {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},
        {"jrel.jvc", {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},
        {"jrel.jhi", {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},
        {"jrel.jls", {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},
        {"jrel.jge", {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},
        {"jrel.jlt", {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},
        {"jrel.jgt", {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},
        {"jrel.jle", {{0x00, 0x2D, InstrSizeMode::FIXED_8, AddressingMode::INMED, emit_jrel}}},

        // enter <frame_size> - crea stack frame reservando N bytes de locales
        {"enter", {{0x28, 0x00, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_abs64}}},
        // leave - destruye el frame actual (sin operandos)
        {"leave", {{0x29, 0x00, InstrSizeMode::FIXED_1,  AddressingMode::NONE,  nullptr}}},

        {"push", {{0x12, 0x00, InstrSizeMode::FIXED_2, AddressingMode::REG, emit_pop_push}}},
        {"pop", {{0x13, 0x00, InstrSizeMode::FIXED_2, AddressingMode::REG, emit_pop_push}}},

        {"xchg", {{0x14, 0x00, InstrSizeMode::FIXED_4, AddressingMode::REG, emit_xchg}}},

        // GC generacional (0x00 0xA0 .. 0xA4)
        {"newobj",   {{0x00, 0xA0, InstrSizeMode::FIXED_4, AddressingMode::REG,  emit_instr_one_reg}}},
        {"gcrun",    {{0x00, 0xA1, InstrSizeMode::FIXED_2, AddressingMode::NONE, nullptr}}},
        {"gcconfig", {{0x00, 0xA2, InstrSizeMode::FIXED_4, AddressingMode::REG,  emit_instr_one_reg}}},
        {"drop",     {{0x00, 0xA3, InstrSizeMode::FIXED_4, AddressingMode::REG,  emit_instr_one_reg}}},
        {"gcwb",     {{0x00, 0xA4, InstrSizeMode::FIXED_4, AddressingMode::REG,  emit_instr_one_reg}}},

        // Raw allocator (0x00 0xB0 .. 0xB2)
        {"alloc",    {{0x00, 0xB0, InstrSizeMode::FIXED_4, AddressingMode::REG,  emit_instr_one_reg}}},
        {"free",     {{0x00, 0xB1, InstrSizeMode::FIXED_4, AddressingMode::REG,  emit_instr_one_reg}}},
        {"realloc",  {{0x00, 0xB2, InstrSizeMode::FIXED_4, AddressingMode::REG,  emit_instr_reg}}},

        // Cursor - acceso a memoria real (0x00 0xC0 .. 0xC2)
        {"readcur",  {{0x00, 0xC0, InstrSizeMode::FIXED_4, AddressingMode::REG, emit_cursor_rw}}},
        {"writecur", {{0x00, 0xC1, InstrSizeMode::FIXED_4, AddressingMode::REG, emit_cursor_rw}}},
        {"gcderef",  {{0x00, 0xC2, InstrSizeMode::FIXED_4, AddressingMode::REG, emit_gcderef}}},

        // OOP - sistema de objetos (0x00 0xD0 .. 0xDC)
        {"newobjraw",  {{0x00, 0xD0, InstrSizeMode::FIXED_4, AddressingMode::REG,  emit_instr_reg}}},
        {"callvirt",   {{0x00, 0xD1, InstrSizeMode::FIXED_4, AddressingMode::REG,  emit_instr_reg_imm8}}},
        {"callsuper",  {{0x00, 0xD2, InstrSizeMode::FIXED_4, AddressingMode::REG,  emit_instr_reg_imm8}}},
        {"throw",      {{0x00, 0xD3, InstrSizeMode::FIXED_4, AddressingMode::REG,  emit_instr_one_reg}}},
        {"rethrow",    {{0x00, 0xD4, InstrSizeMode::FIXED_2, AddressingMode::NONE, nullptr}}},
        {"getclass",   {{0x00, 0xD5, InstrSizeMode::FIXED_4, AddressingMode::REG,  emit_instr_one_reg}}},
        {"instanceof", {{0x00, 0xD6, InstrSizeMode::FIXED_4, AddressingMode::REG,  emit_instr_reg}}},
        {"checkcast",  {{0x00, 0xD7, InstrSizeMode::FIXED_4, AddressingMode::REG,  emit_instr_reg}}},
        {"getfield",   {{0x00, 0xD8, InstrSizeMode::FIXED_4, AddressingMode::REG,  emit_instr_reg_imm8}}},
        {"getmethod",  {{0x00, 0xD9, InstrSizeMode::FIXED_4, AddressingMode::REG,  emit_instr_reg_imm8}}},
        {"fieldcount", {{0x00, 0xDA, InstrSizeMode::FIXED_4, AddressingMode::REG,  emit_instr_one_reg}}},
        {"methodcount",{{0x00, 0xDB, InstrSizeMode::FIXED_4, AddressingMode::REG,  emit_instr_one_reg}}},
        {"classname",  {{0x00, 0xDC, InstrSizeMode::FIXED_4, AddressingMode::REG,  emit_instr_one_reg}}},

        // estas instrucciones no necesitan emitir mas que sus opcodes
        {"nop1", {{0x33, 0x00, InstrSizeMode::FIXED_1, AddressingMode::NONE, nullptr}}},
        {"nop2", {{0x00, 0x33, InstrSizeMode::FIXED_2, AddressingMode::NONE, nullptr}}},

        {
            "callnr",
            {
                {0x55, 0x00, InstrSizeMode::FIXED_1, AddressingMode::REG, nullptr},
            },
        },

        {"ret", {{0xC3, 0x00, InstrSizeMode::FIXED_1, AddressingMode::NONE, nullptr}}},

        // Extensión (opcode1 = 0x00)
        {"edmw4", {{0x00, 0x00, InstrSizeMode::FIXED_4, AddressingMode::NONE, nullptr}}},
        {"edmw6", {{0x00, 0x01, InstrSizeMode::FIXED_4, AddressingMode::NONE, nullptr}}},
        {"edm", {{0x00, 0x02, InstrSizeMode::FIXED_2, AddressingMode::NONE, nullptr}}},
        {"hlt", {{0x00, 0x03, InstrSizeMode::FIXED_2, AddressingMode::NONE, nullptr}}},

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
                {0x00, 0x05, InstrSizeMode::FIXED_4, AddressingMode::REG, emit_instr_reg},

                // reg, [mem] || [mem], reg
                {0x00, 0x06, InstrSizeMode::MIXED_SIZE, AddressingMode::INMED, emit_instr_inmed},

                // REG, SIB || SIB, REG
                {0x00, 0x07, InstrSizeMode::FIXED_4, AddressingMode::SIB, emit_instr_sib}
            }
        },
        {
            "addu", {
                // addu
                // reg, reg
                {0x00, 0x05, InstrSizeMode::FIXED_4, AddressingMode::REG, emit_instr_reg},

                // reg, [mem] || [mem], reg
                {0x00, 0x06, InstrSizeMode::MIXED_SIZE, AddressingMode::INMED, emit_instr_inmed},

                // REG, SIB || SIB, REG
                {0x00, 0x07, InstrSizeMode::FIXED_4, AddressingMode::SIB, emit_instr_sib}
            }
        },

        {
            "subu", {
                // subu/subs
                // reg, reg
                {0x00, 0x08, InstrSizeMode::FIXED_4, AddressingMode::REG, emit_instr_reg},

                // reg, [mem] || [mem], reg
                {0x00, 0x09, InstrSizeMode::MIXED_SIZE, AddressingMode::INMED, emit_instr_inmed},

                // REG, SIB || SIB, REG
                {0x00, 0x0A, InstrSizeMode::FIXED_4, AddressingMode::SIB, emit_instr_sib}
            }
        },
        {
            "subs", {
                // subu/subs
                // reg, reg
                {0x00, 0x08, InstrSizeMode::FIXED_4, AddressingMode::REG, emit_instr_reg},

                // reg, [mem] || [mem], reg
                {0x00, 0x09, InstrSizeMode::MIXED_SIZE, AddressingMode::INMED, emit_instr_inmed},

                // REG, SIB || SIB, REG
                {0x00, 0x0A, InstrSizeMode::FIXED_4, AddressingMode::SIB, emit_instr_sib}
            }
        },
        {
            "muls", {
                // mulu/muls
                // reg, reg
                {0x00, 0x0B, InstrSizeMode::FIXED_4, AddressingMode::REG, emit_instr_reg},

                // reg, [mem] || [mem], reg
                {0x00, 0x0C, InstrSizeMode::MIXED_SIZE, AddressingMode::INMED, emit_instr_inmed},

                // REG, SIB || SIB, REG
                {0x00, 0x0D, InstrSizeMode::FIXED_4, AddressingMode::SIB, emit_instr_sib}
            }
        },
        {
            "mulu", {
                // mulu/muls
                // reg, reg
                {0x00, 0x0B, InstrSizeMode::FIXED_4, AddressingMode::REG, emit_instr_reg},

                // reg, [mem] || [mem], reg
                {0x00, 0x0C, InstrSizeMode::MIXED_SIZE, AddressingMode::INMED, emit_instr_inmed},

                // REG, SIB || SIB, REG
                {0x00, 0x0D, InstrSizeMode::FIXED_4, AddressingMode::SIB, emit_instr_sib}
            }
        },
        {
            "divu", {
                // divu/divs
                // reg, reg
                {0x00, 0x0E, InstrSizeMode::FIXED_4, AddressingMode::REG, emit_instr_reg},

                // reg, [mem] || [mem], reg
                {0x00, 0x0F, InstrSizeMode::MIXED_SIZE, AddressingMode::INMED, emit_instr_inmed},

                // REG, SIB || SIB, REG
                {0x00, 0x10, InstrSizeMode::FIXED_4, AddressingMode::SIB, emit_instr_sib}
            }
        },
        {
            "divs", {
                // divu/divs
                // reg, reg
                {0x00, 0x0E, InstrSizeMode::FIXED_4, AddressingMode::REG, emit_instr_reg},

                // reg, [mem] || [mem], reg
                {0x00, 0x0F, InstrSizeMode::MIXED_SIZE, AddressingMode::INMED, emit_instr_inmed},

                // REG, SIB || SIB, REG
                {0x00, 0x10, InstrSizeMode::FIXED_4, AddressingMode::SIB, emit_instr_sib}
            }
        },
        {
            "cmpu",
            {
                // cmpu/cmps
                // reg, reg
                {0x00, 0x11, InstrSizeMode::FIXED_4, AddressingMode::REG, emit_instr_reg},

                // reg, [mem] || [mem], reg
                {0x00, 0x12, InstrSizeMode::MIXED_SIZE, AddressingMode::INMED, emit_instr_inmed},

                // REG, SIB || SIB, REG
                {0x00, 0x13, InstrSizeMode::FIXED_4, AddressingMode::SIB, emit_instr_sib},
            },
        },
        {
            "cmps",
            {
                // cmpu/cmps
                // reg, reg
                {0x00, 0x11, InstrSizeMode::FIXED_4, AddressingMode::REG, emit_instr_reg},

                // reg, [mem] || [mem], reg
                {0x00, 0x12, InstrSizeMode::MIXED_SIZE, AddressingMode::INMED, emit_instr_inmed},

                // REG, SIB || SIB, REG
                {0x00, 0x13, InstrSizeMode::FIXED_4, AddressingMode::SIB, emit_instr_sib},
            },
        },

        {
            "mov",
            {
                // por definir
                // mov
                // reg, reg
                {0x00, 0x14, InstrSizeMode::FIXED_4, AddressingMode::REG, emit_instr_mov_reg},

                // reg, [mem] || [mem], reg
                {0x00, 0x15, InstrSizeMode::MIXED_SIZE, AddressingMode::INMED, emit_instr_mov_inmed},

                // REG, SIB || SIB, REG
                {0x00, 0x16, InstrSizeMode::FIXED_4, AddressingMode::SIB, emit_instr_mov_sib},
            },
        },
        {
            "loop",
            {
                {0x00, 0x31, InstrSizeMode::FIXED_8, AddressingMode::INMED, nullptr},
            },
        },

        {
            "calln",
            {
                {0x00, 0x55, InstrSizeMode::FIXED_10, AddressingMode::INMED, emit_instr_calln_inmmed},
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
        /// Tabla de símbolos generada en la primera pasada.
        std::unordered_map<std::string, Label *> symbol_table;

        /// Buffer de salida donde se escribe el bytecode final.
        ByteWriter output;

        /**
         * Contexto del ensamblador
         */
        Context ctx{};

        /**
         * Seccion que inspeccion actualmente, esto va cambiando a lo largo del programa
         */
        Section *current_section = nullptr;

        /**
         * Label analizada actualmente por el ensamblador, esta variable va cambiando a lo largo de la ejecuccion
         * del ensamblador y solo se uso de cursor interno
         */
        Label *current_label = nullptr;

        /**
         * @brief Constructor del ensamblador.
         *
         * Inicializa las estructuras internas, limpia buffers y establece
         * la dirección base por defecto.
         */
        Assembler();

        void compute_label_sizes();

        /**
         * @brief Ensambla un AST completo en un buffer de bytecode.
         *
         * Ejecuta las 3 fases del ensamblado:
         *  1. Recolección de símbolos.
         *  2. Emisión de datos.
         *  3. Emisión de instrucciones.
         *
         *  la 2 y 3 ahora se han fusionado
         *
         * @param ast Lista de nodos raíz del AST.
         * @return Vector de bytes con el bytecode final.
         */
        std::vector<uint8_t> assemble(const std::vector<std::unique_ptr<vm::ASTNode> > &ast);

        /**
         * Aqui se han fusionado la segunda y tercera fase. La primera debe
         * hacerse aparte ya que primero hay que saber el tamaño de las cosas
         * para poder calcular los desplazamientos y offsets
         * @param node Nodo actual del AST.
         */
        void emit_pass(const vm::ASTNode *node);

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
                                        const std::vector<std::unique_ptr<vm::ASTNode> > &ops) const;

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

        /**
         * @brief Primera pasada: recolección de símbolos.
         *
         * Asigna direcciones a labels, datos y bloques de código.
         *
         * @param node Nodo actual del AST.
         * @param offset Offset acumulado.
         */
        void first_pass(const vm::ASTNode *node, uint64_t &offset);

        void apply_annotation(const vm::AnnotationNode *annotation);

        void apply_directive(const vm::Instruction *instr);

    private:
    };

    /**
     * Crea una ast nuevo, combinando el ast original y el de los archivos importados. Para
     * esto
     *    - Solo movimientos.
     *    - No se duplica memoria, no se duplican nodos, solo se mueven.
     *    - Los imports desaparecen y son reemplazados por los nodos reales
     *          del archivo importado.
     *
     *  Significa:
     *      - Toma el AST original.
     *      - Toma el AST importado.
     *      - Los fusiona en un nuevo vector result.
     *      - Mueve todos los nodos al nuevo AST.
     *      - Reemplaza el AST original por el nuevo.
     *
     *      AST original:
     *          [A, B, import C, D]
     *
     *      AST de C:
     *          [X, Y, Z]
     *
     *      AST final:
     *          [A, B, X, Y, Z, D]
     *
     * Si algún nodo del AST guarda punteros a otros nodos, este metodo rompe esas referencias.
     * Pero si el AST es puramente jerárquico (cada nodo solo conoce a sus hijos), entonces no hay problema
     *
     * @param ast archivo original que import
     * @param imported
     */
    static void resolve_imports(std::vector<std::unique_ptr<vm::ASTNode> > &ast,
                                std::unordered_set<std::string> &           imported) {
        std::vector<std::unique_ptr<vm::ASTNode> > result;

        for (auto &node: ast) {
            // si se encontro un nodo de tipo Import
            if (auto imp = dynamic_cast<vm::ImportNode *>(node.get())) {
                std::string file = imp->filename;

                // mirar si ya se añadio
                if (imported.find(file) != imported.end())
                    continue; // evitar múltiples inclusiones

                imported.insert(file);

                // Leer archivo
                std::ifstream f(file);
                std::string   code((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());

                // Lex + parse del codigo importado
                vm::Lexer  lx(code);
                vm::Parser px(lx);
                auto       imported_ast = px.parse();

                // Expandir imports recursivamente
                resolve_imports(imported_ast, imported);

                // Insertar nodos importados
                for (auto &n: imported_ast)
                    result.push_back(std::move(n));
            }
            // Si el nodo NO es un import, lo copia al AST final.
            else {
                result.push_back(std::move(node));
            }
        }

        // Reemplazar AST original
        ast = std::move(result);
    }
}

#endif // PARSER_TO_BYTECODE_H
