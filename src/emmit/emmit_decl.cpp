#include "emmit/emmit_decl.h"

#include "emmit/parser_to_bytecode.h"

#ifdef DEBUG_EMIT
#define DEBUG_PRINT(...) printf(__VA_ARGS__)
#else
#define DEBUG_PRINT(...) if (0) {printf(__VA_ARGS__);}
#endif


/**
 * La isntruccion de emision emiten el resto de bytes faltantes,
 * el opcode/s ya debio ser emitido por a funcion llamadora del metodo de emision.
 */
namespace Assembly::Bytecode {
    /**
     * Permite codificar el registro unicamente, independientemente del "modo"/tamaño de este,
     * ya que r00, r00d, r00w y r00b usando el mismo valor para coficarse, 0, pero cambia el modo.
     *
     * @param name nombre del registro a codear
     * @return registro coficado, si es de proposito general, no se incluye el modo
     */
    uint8_t encode_reg_general(const char *const name) {
        uint8_t n_reg = 0;

        if (name[0] == 'r' || name[0] == 'R') {
            // si despues de la r va un digito, entonces puede ser una instruccion
            // con un solo digito
            if (name[1] && isdigit(static_cast<unsigned char>(name[1]))) {
                n_reg = name[1] - '0'; // obtenmos el primer digito

                // en este caso de que haya segundo digito, se analiza
                if (name[2] && isdigit(static_cast<unsigned char>(name[2]))) {
                    // evitamos si codifican el registro como r01
                    if (n_reg != 0) {
                        // obtenmos el segundo digito y multiplicamos al anterior
                        // para obtener el valor final
                        return (n_reg * 10) + (name[2] - '0');
                    }

                    // al ser r01, retornamo el segundo digito unicamente
                    return (name[2] - '0');
                }
                return n_reg;
            }
            throw std::runtime_error("Registro inválido: falta número tras 'r'");
        }
        throw std::runtime_error("Registro inválido: debe comenzar por 'r'");
    }


    void emit_inc_dec(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    ) {
        // si el opcode es un dec, entonces, el segundo byte tiene el bit dos activo
        uint8_t reg = (instruction_parser->opcode == "inc") ? 0 : 0b01000000;

        // INC solo usa un operando de tipo registro
        if (auto s = dynamic_cast<vm::RegisterOperand *>(instruction_parser->operands[0].get())) {
            // 0b0 b mode reg
            // b -> INC / DEC
            // mode = 0b00 -> dos bits para tamaño de red
            // reg =  0b0000 -> 4 bits para registro
            uint8_t mode = encode_mode(s->size_bits);
            reg += mode << 4;                           // se desplaza 4 bits
            reg += encode_reg_general(s->name.c_str()); // codificamos el registro
            code_final.emit8(reg);
        } else {
            throw std::runtime_error("Registro inválido: esta instruccion INC / DEC espera un registro");
        }
        DEBUG_PRINT("Emitiendo INC / DEC 0x%x REG: 0x%x, Size: %llu\n",
                    now_instr->opcode1, reg,
                    instr_size(now_instr->sizeMode)
        );
    }

    void emit_instr_reg(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    ) {
        auto reg1 = dynamic_cast<vm::RegisterOperand *>(instruction_parser->operands[0].get());
        auto reg2 = dynamic_cast<vm::RegisterOperand *>(instruction_parser->operands[1].get());

        if (reg1 == nullptr || reg2 == nullptr) {
            throw std::runtime_error(
                "Error, la instruccion " + instruction_parser->opcode + " esperaba dos registros"
            );
        }

        if (reg1->size_bits != reg2->size_bits) {
            throw std::runtime_error(
                "Error, la instruccion " + instruction_parser->opcode + " " +
                reg1->name + " " + reg2->name + " tiene sizes distintos en los " +
                "registros, deben tener el mismo size."
            );
        }
        bool is_a_signed = is_signed(instruction_parser->opcode);

        uint8_t mode = encode_mode(reg1->size_bits);

        // 0b`mode`sd0000 -> modo ocupa el los primeros 2 bits
        code_final.emit8((mode << 6) | ((is_a_signed) ? 1 : 0) << 5);

        // los dos registros se codifica en el mismo byte (en el cuarto normalmente), el modo en el tercero
        code_final.emit8(
            encode_reg_general(reg2->name.c_str()) << 4 | encode_reg_general(reg1->name.c_str())
        );

        DEBUG_PRINT("Emitiendo %s 0x%02x 0x%02x REG1(%s): 0x%02x, REG2(%s): 0x%02x MODE: %d\n",
                    instruction_parser->opcode.c_str(),
                    now_instr->opcode1,
                    now_instr->opcode2,
                    reg1->name.c_str(), encode_reg_general(reg2->name.c_str()) << 4,
                    reg2->name.c_str(), encode_reg_general(reg1->name.c_str()),
                    mode
        );
    }

    void emit_instr_inmed_with_annotation(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    ) {
        bool is_a_signed = is_signed(instruction_parser->opcode);

        auto reg              = dynamic_cast<vm::RegisterOperand *>(instruction_parser->operands[0].get());
        auto label_annotacion = instruction_parser->operands[1].get();

        auto annotacion = dynamic_cast<vm::AnnotationNode *>(label_annotacion);

        if (annotacion == nullptr) {
            throw std::runtime_error(
                "emit_instr_inmed_with_annotation(): Se debe implementar: " + instruction_parser->opcode + " ");
        }


        if (annotacion->key == "Method") {
            // el modo siempre es del tamaño de palabra de la cpu para direcciones,
            // se multiplica * 8 para obtener la cantidad de bit que ocupa un puntero.
            uint8_t mode     = encode_mode(sizeof(void *) * 8);
            bool    mem_dest = false; // el destino siempre es un registro, por eso false
            code_final.emit8(
                (mode << 6) |                    // mode
                (((is_a_signed) ? 1 : 0) << 5) | // s -> signed
                mem_dest << 4 |
                encode_reg_general(reg->name.c_str()) // reg
            );


            // creamos el valor
            Relocation rel;
            rel.symbol  = annotacion->value;
            rel.section = assembly_ctx->current_section->name;
            rel.offset  = code_final.offset /*- 8*/; // el -8 se pone si se emite antes la direccion
            rel.type    = size_ptr_in_this_machine;  // relocalizacion absoluta segun el tamaño de palabra de la maquina

            assembly_ctx->ctx.add_relocation(rel);

            // siempre se emite 64 bits si la cpu es de 64 bits, ya que se parcheara la instruccion
            // con un puntero real dependiente del tamaño de palabra de la cpu
            if (size_ptr_in_this_machine == Type::Absolute64) {
                code_final.emit64(0);
            } else if (size_ptr_in_this_machine == Type::Absolute32) {
                code_final.emit32(0);
            } else if (size_ptr_in_this_machine == Type::Absolute16) {
                code_final.emit16(0);
            } else if (size_ptr_in_this_machine == Type::Absolute8) {
                code_final.emit8(0);
            } else {
                throw std::runtime_error("emit_instr_inmed_with_annotation(): No se sopora la "
                    "emision de instrucciones inmediatas con direcciones para tamaños de: " + sizeof(void *));
            }
        } else {
            // si no es una notacion Method, entonces debe ser una
            // Absolute o una Relative
            uint8_t mode = encode_mode(reg->size_bits);

            // tenemos en cuenta el tamaño del registro usado
            int  instr_mode_bits = mode_to_bits(mode);
            bool mem_dest        = false; // el destino siempre es un registro, por eso false


            code_final.emit8(
                (mode << 6) |                    // mode
                (((is_a_signed) ? 1 : 0) << 5) | // s -> signed
                mem_dest << 4 |
                encode_reg_general(reg->name.c_str()) // reg
            );

            // creamos el valor
            Relocation rel;
            rel.symbol  = annotacion->value;
            rel.section = assembly_ctx->current_section->name;
            rel.offset  = code_final.offset /*- 8*/; // el -8 se pone si se emite antes la direccion

            // indicamos el tipo de relocalizacion que es
            if (annotacion->key == "Relative") {
                switch (mode_to_bytes(mode)) {
                    case 1: {
                        rel.type = Type::Relative8;
                        break;
                    }
                    case 2: {
                        rel.type = Type::Relative16;
                        break;
                    }
                    case 4: {
                        rel.type = Type::Relative32;
                        break;
                    }
                    case 8: {
                        rel.type = Type::Relative64;
                        break;
                    }
                    default: {
                        goto error_notation;
                    }
                }
            } else if (annotacion->key == "Absolute") {
                switch (mode_to_bytes(mode)) {
                    case 1: {
                        rel.type = Type::Absolute8;
                        break;
                    }
                    case 2: {
                        rel.type = Type::Absolute16;
                        break;
                    }
                    case 4: {
                        rel.type = Type::Absolute32;
                        break;
                    }
                    case 8: {
                        rel.type = Type::Absolute64;
                        break;
                    }
                    default: {
                        goto error_notation;
                    }
                }
            } else {
            error_notation:
                throw std::runtime_error("emit_instr_inmed_with_annotation(): No se sopora la "
                    "notacion: " + annotacion->key);
            }

            assembly_ctx->ctx.add_relocation(rel);

            uint64_t val_inmmed = 0; // siempre 0 por que sera remplazado por el
            // linker

            size_t size_in_bytes = mode_to_bytes(mode);
            // emitimos el resto de bytes de la instruccion dependiendo del modo/tamaño
            code_final.emit_bytes(&val_inmmed, size_in_bytes);
        }
    }

    void emit_instr_inmed(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    ) {
        if (now_instr->opcode1 != 0x00) {
            throw std::runtime_error("Error instruccion != 0x00 no implementada");
        }
        bool is_a_signed = is_signed(instruction_parser->opcode);

        auto n0 = instruction_parser->operands[0].get();
        auto n1 = instruction_parser->operands[1].get();

        auto inmmed_str = dynamic_cast<vm::NumberOperand *>(n1);

        auto reg      = dynamic_cast<vm::RegisterOperand *>(n0);
        auto mem      = dynamic_cast<vm::MemoryOperand *>(n0);
        bool mem_dest = reg == nullptr; // si no se obtuvo un registro, y se obtuvo un operando memoria esto es true.

        if ((mem == nullptr) || inmmed_str == nullptr) {
            // si no es un operando de tipo memoria, ni registro, entonces puede ser uno de tipo anotacion:
            if (auto annotacion = dynamic_cast<vm::AnnotationNode *>(n1)) {
                emit_instr_inmed_with_annotation(instruction_parser, code_final, now_instr, assembly_ctx);
                return;
            }

            // si ni es una notacion, ni es memoria ni es un inmediato:
            if (mem == nullptr && inmmed_str == nullptr) {
                throw std::runtime_error("Error instruccion: " + instruction_parser->opcode +
                    " no pudo situar un registro, inmediato por alguna razon, posiblemente usted cometio un error de sintaxis o logico");
            }
        }

        auto inmmed_opt = vm::parse_number_safe(inmmed_str->value);
        if (inmmed_opt == std::nullopt) {
            throw std::runtime_error("Error el valor: " + inmmed_str->value +
                " no pudo convertirse en un numero.");
        }
        uint64_t val_inmmed = inmmed_opt.value();
        // detectamos el tipo de inmediato que es
        ImmType type = detect_imm_type((int64_t) val_inmmed, is_a_signed);
        if (mem_dest) {
            reg = static_cast<vm::RegisterOperand *>(mem->expr.get());
            if (reg == nullptr) {
                std::cout << "Error instruccion: " + instruction_parser->opcode +
                        " esperaba un registro para acceder a memoria pero obtuvo algo distinto: " << std::endl;
                mem->expr->print(0);
                throw std::runtime_error("Error instruccion: " + instruction_parser->opcode);
            }
        }
        uint8_t mode = encode_mode(reg->size_bits);


        // validamos si los tamaños del modo y el inmediato son iguales
        int instr_imm_bits  = immtype_bits(type);
        int instr_mode_bits = mode_to_bits(mode);

        if (instr_imm_bits > instr_mode_bits) {
            throw std::runtime_error(
                "emit_instr_inmed(): Error: el inmediato " + std::to_string(val_inmmed) +
                " (" + std::to_string(instr_imm_bits) + " bits) no cabe en el modo " +
                std::to_string(mode) + " (" + std::to_string(instr_mode_bits) + " bits)."
            );
        }

        // 0b `mode` s d reg -> modo ocupa el los primeros 2 bits
        code_final.emit8(
            (mode << 6) |                    // mode
            (((is_a_signed) ? 1 : 0) << 5) | // s -> signed
            mem_dest << 4 |
            encode_reg_general(reg->name.c_str()) // reg
        );
        // el campo d siempre es 0 ya que se usa para codificar otro tipo de inmediatos.

        // emitimos el resto de bytes de la instruccion dependiendo del modo/tamaño
        code_final.emit_bytes(&val_inmmed, mode_to_bytes(mode));

        DEBUG_PRINT("Emitiendo %s 0x%02x 0x%02x REG1(%s): 0x%02x, INMED 0x%llx TYPE_INMED: %d MODE: %d\n",
                    instruction_parser->opcode.c_str(),
                    now_instr->opcode1,
                    now_instr->opcode2,
                    reg->name.c_str(), encode_reg_general(reg->name.c_str()),
                    val_inmmed,
                    type,
                    mode
        );
    }

    void emit_instr_mem(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    ) {
        if (now_instr->opcode1 != 0x00) {
            throw std::runtime_error("Error instruccion != 0x00 no implementada");
        }
        bool is_a_signed = is_signed(instruction_parser->opcode);

        auto n0 = instruction_parser->operands[0].get();
        auto n1 = instruction_parser->operands[1].get();

        uint8_t direccion = 1; // [mem], reg
        auto    mem       = dynamic_cast<vm::MemoryOperand *>(n0);
        auto    reg       = dynamic_cast<vm::RegisterOperand *>(n1);

        // si el op1 no era de memoria o el op2 no era de tipo registro, probar al reves:
        if (mem == nullptr || reg == nullptr) {
            direccion = 0; // reg, [mem]
            mem       = dynamic_cast<vm::MemoryOperand *>(n1);
            reg       = dynamic_cast<vm::RegisterOperand *>(n0);
        }

        if (mem == nullptr || reg == nullptr) {
            throw std::runtime_error("Error instruccion: " + instruction_parser->opcode +
                " no pudo situar un operando de tipo memoria, mas de otro operando de tipo registro.");
        }
        uint8_t mode = encode_mode(reg->size_bits);

        /**
         * mode       = 01 << 6 = 0100 0000
         * signed     = 1  << 5 = 0010 0000
         * direccion  = 0  << 4 = 0000 0000
         * reg (7)    = 0000 0111
         *
         * Resultado:   0110 0111   = 0x67
         * [ mode(2) | signed(1) | direccion(1) | reg(4) ]
         */
        uint8_t byte = (mode << 6) |
                (is_a_signed << 5) |
                (direccion << 4) |
                encode_reg_general(reg->name.c_str());
        code_final.emit8(byte);

        // por ahora supondremos que solo se introdujo operando de memoria de un solo label tipo [my_label]
        auto lalbel = dynamic_cast<vm::LabelOperand *>(mem->expr.get());

        Relocation rel;
        rel.symbol  = lalbel->name;
        rel.section = assembly_ctx->current_section->name;
        rel.offset  = code_final.offset /*- 5*/; // el -5 se pone si se emite antes la direccion
        rel.type    = Type::Relative40;

        assembly_ctx->ctx.add_relocation(rel);

        // esto es un placeholder que luego el linker debe corregir realizando una relocalizacion.
        code_final.emit40(0x1122334455);
    }

    void emit_instr_sib(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    ) {
        bool is_a_signed = is_signed(instruction_parser->opcode);
    }

    void emit_xchg(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    ) {
        auto reg1 = dynamic_cast<vm::RegisterOperand *>(instruction_parser->operands[0].get());
        auto reg2 = dynamic_cast<vm::RegisterOperand *>(instruction_parser->operands[1].get());

        if (reg1 == nullptr) {
            throw std::runtime_error("Error la instruccion: " + instruction_parser->opcode +
                " no encontro un registro 1");
        }

        if (reg2 == nullptr) {
            throw std::runtime_error("Error la instruccion: " + instruction_parser->opcode +
                " no encontro un registro 2");
        }

        uint8_t flags = 0; // aun no se usa, pero se debe emitir.
        code_final.emit8(flags);

        // miramos si algunos de los registros son especiales, si no los son, o si lo son ambos
        std::optional<uint8_t> opt_special1 = encode_special_register(reg1->name);
        std::optional<uint8_t> opt_special2 = encode_special_register(reg2->name);

        // codificamos el primer byte de registros
        uint8_t byte1 = 0b0000'0000;
        if (opt_special1) {
            // si es un registro especial, se debe indicar con el segundo bit, en caso
            // de ser registro general, no se activa este bit
            byte1 = 0b0100'0000;
            byte1 = byte1 | opt_special1.value(); // 6 bits para indicar el registro
        } else {
            uint8_t mode     = encode_mode(reg1->size_bits);           // 2 bits
            uint8_t reg_code = encode_reg_general(reg1->name.c_str()); // 4 bits
            byte1            = byte1 | (mode << 4 | reg_code);
        }

        // codificamos el segundo byte de registros
        uint8_t byte2 = 0b0000'0000;
        if (opt_special2) {
            // si es un registro especial, se debe indicar con el segundo bit, en caso
            // de ser registro general, no se activa este bit
            byte2 = 0b0100'0000;
            byte2 = byte2 | opt_special2.value(); // 6 bits para indicar el registro
        } else {
            uint8_t mode     = encode_mode(reg2->size_bits);           // 2 bits
            uint8_t reg_code = encode_reg_general(reg2->name.c_str()); // 4 bits
            byte2            = byte2 | (mode << 4 | reg_code);
        }

        // emitimos los bytes de los registros
        code_final.emit8(byte1);
        code_final.emit8(byte2);
    }

    void emit_pop_push(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    ) {
        //bool is_push = instruction_parser->opcode == "push";
        auto n0  = instruction_parser->operands[0].get();
        auto reg = dynamic_cast<vm::RegisterOperand *>(n0);

        if (reg == nullptr) {
            throw std::runtime_error("Error instruccion: " + instruction_parser->opcode +
                " no encontro un registro cuando se esperaba uno.");
        }

        std::optional<uint8_t> opt_special = encode_special_register(reg->name);
        uint8_t                reg_ext = 0; // registro extendido usado
        uint8_t                mode = 0; // modo/tamaño del registro general, si reg_ext = 1, este campo no se usa
        uint8_t                byte = 0x0; // byte final a emitir
        uint8_t                reg_code = 0;

        if (opt_special) {
            // ------------------------------
            //   REGISTRO ESPECIAL/EXTENDIDO (reg_ext=1)
            // ------------------------------
            reg_code = opt_special.value(); // 6 bits
            reg_ext  = 1;                   // bit 7

            byte = (reg_ext << 6) | reg_code;
        } else {
            // ------------------------------
            //   REGISTRO GENERAL (reg_ext=0)
            // ------------------------------
            reg_ext = 0; // bit 7

            mode     = encode_mode(reg->size_bits);           // 2 bits
            reg_code = encode_reg_general(reg->name.c_str()); // 4 bits

            // Formato: [reg_ext][mode:2][reg:4]
            byte = (reg_ext << 6) | (mode << 4) | reg_code;
        }

        code_final.emit8(byte);
    }

    void emit_instr_mov_reg(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    ) {
        auto reg1 = dynamic_cast<vm::RegisterOperand *>(instruction_parser->operands[0].get());
        auto reg2 = dynamic_cast<vm::RegisterOperand *>(instruction_parser->operands[1].get());

        if (reg1 == nullptr || reg2 == nullptr) {
            throw std::runtime_error(
                "Error, la instruccion " + instruction_parser->opcode + " esperaba dos registros"
            );
        }

        if (reg1->size_bits != reg2->size_bits) {
            throw std::runtime_error(
                "Error, la instruccion " + instruction_parser->opcode + " " +
                reg1->name + " " + reg2->name + " tiene sizes distintos en los " +
                "registros, deben tener el mismo size."
            );
        }

        // miramos si los registros son especiales/extendidos
        std::optional<uint8_t> opt_reg1_special = encode_special_register(reg1->name);
        std::optional<uint8_t> opt_reg2_special = encode_special_register(reg2->name);

        bool r1_special = opt_reg1_special.has_value();
        bool r2_special = opt_reg2_special.has_value();

        if (r1_special && r2_special) {
            throw std::runtime_error(
                "emit_instr_mov_reg() Error, la instruccion " + instruction_parser->opcode +
                " no permite usar dos registros extendidos/especiales"
            );
        }

        uint8_t mode        = 0;
        uint8_t is_a_signed = 0;
        uint8_t direction   = 0;
        uint8_t reg_special = 0; // guarda cual es el registro especial/extendido usado
        // ninguno de los dos registros era ext/especial
        uint8_t reg_general = 0; // en caso de usar un reg especial, debemos almacenar tmb el reg general.

        if (r1_special == false && r2_special == false) {
            // sacamos el tamaño de los registros:
            mode = encode_mode(reg1->size_bits);

            // ``MOV reg1, reg2``, is_a_signed siempre es 0 aqui.
            // direction << 4, direction = 0 por que la direccion
            // siempre es 0 al no haber direccion.

            // 0b`mode`sd0000 -> modo ocupa el los primeros 2 bits
            code_final.emit8((mode << 6) | (((is_a_signed) ? 1 : 0) << 5) | (direction << 4));

            // los dos registros se codifica en el mismo byte (en el cuarto normalmente), el modo en el tercero
            code_final.emit8(
                encode_reg_general(reg2->name.c_str()) << 4 | encode_reg_general(reg1->name.c_str())
            );
            DEBUG_PRINT("Emitiendo %s 0x%02x 0x%02x REG1(%s): 0x%02x, REG2(%s): 0x%02x MODE: %d\n",
                        instruction_parser->opcode.c_str(),
                        now_instr->opcode1,
                        now_instr->opcode2,
                        reg1->name.c_str(), encode_reg_general(reg2->name.c_str()) << 4,
                        reg2->name.c_str(), encode_reg_general(reg1->name.c_str()),
                        mode
            );
            return;
        }
        /**
         * | MOV reg_ext, reg    |   0x0   |  0x14   |  0b`mode`1d0000    d = 0  |
         * | :------------------ | :-----: | :-----: | :-----------------------: |
         * | MOV reg, reg_ext    |   0x0   |  0x14   |  0b`mode`1d0000    d = 1  |
         *
         * Estas instrucciones siempre tienen is_a_signed = 1 que es el bit entre "mode" y "d".
         * El modo en este caso, es el modo del registro especial, mas no indica tamaño del registro
         * general en este caso.
         */
        is_a_signed = 1;

        reg_general = r1_special
                          ? encode_reg_general(reg2->name.c_str())
                          : encode_reg_general(reg1->name.c_str());
        reg_special = r1_special ? opt_reg1_special.value() : opt_reg2_special.value();

        // direction   = 0; // MOV reg_ext, reg
        // direction   = 1; // MOV reg, reg_ext
        direction = r1_special ? 0 : 1;

        // un registro espcial o extendido normalmente codifica el campo modo
        // como bits extras del registro, lo que hace que ocupe 2 + 4 = 6bits,
        // debemos separa el modo y el registro
        mode = (reg_special >> 4) & 0b11;

        // 0b`mode`sd0000 -> modo ocupa el los primeros 2 bits
        code_final.emit8((mode << 6) | (((is_a_signed) ? 1 : 0) << 5) | (direction << 4));

        // los dos registros se codifica en el mismo byte (en el cuarto normalmente), el modo en el tercero
        code_final.emit8(
            (reg_special & 0x0F) << 4 | (reg_general & 0x0F)
        );

        DEBUG_PRINT("Emitiendo %s 0x%02x 0x%02x REG1(%s): 0x%02x, REG2(%s): 0x%02x MODE: %d\n",
                    instruction_parser->opcode.c_str(),
                    now_instr->opcode1,
                    now_instr->opcode2,
                    reg1->name.c_str(), reg_special,
                    reg2->name.c_str(), reg_general,
                    mode
        );
    }

    void emit_instr_mov_inmed(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    ) {
        uint8_t is_a_signed = 0;
        uint8_t mode        = 0;
        uint8_t direction   = 0;
        uint8_t size_bytes  = 0;

        // unicos modos permitidos:
        /**
         * | ``MOV reg1, inmmed``   |   0x0   |  0x15   | 0b`mode`0d`reg1`<br>d = 0 |
         * |                        |   0x0   |  0x15   | 0b`mode`0d`reg1`<br>d = 1 |
         * | ``MOV [reg], inmmed``  |   0x0   |  0x15   | 0b`mode`1d`reg1`<br>d = 0 |
         * | ``MOV reg_ext, inmmed``|   0x0   |  0x15   | 0b`mode`1d`reg1`<br>d = 1 |
         */

        auto n0 = instruction_parser->operands[0].get();
        auto n1 = instruction_parser->operands[1].get();

        // el segundo operando debe ser siempre un inmediato, si no es una notacion
        auto     inmmed_str = dynamic_cast<vm::NumberOperand *>(n1);
        uint64_t val_inmmed = 0;
        if (inmmed_str != nullptr) {
            auto inmmed_opt = vm::parse_number_safe(inmmed_str->value);
            if (inmmed_opt == std::nullopt) {
                throw std::runtime_error("Error el valor: " + inmmed_str->value +
                    " no pudo convertirse en un numero.");
            }
            val_inmmed = inmmed_opt.value();
        }

        // el primer operando puede ser un registro general, especial, o memoria.
        auto reg      = dynamic_cast<vm::RegisterOperand *>(n0);
        auto mem      = dynamic_cast<vm::MemoryOperand *>(n0);
        bool mem_dest = reg == nullptr; // si no se obtuvo un registro, y se obtuvo un operando memoria esto es true.

        if (auto annotacion = dynamic_cast<vm::AnnotationNode *>(n1)) {
            if (annotacion->key == "Method") {
                if (reg != nullptr) {
                    // si se usa una notacion Method, se debe usar un tamaño multiplo de
                    // palabra.
                    reg->size_bits = sizeof(void *) * 8;
                } else if (mem != nullptr) {
                    reg            = static_cast<vm::RegisterOperand *>(mem->expr.get());
                    reg->size_bits = sizeof(void *) * 8;
                }

                // creamos el valor
                Relocation rel;
                rel.symbol  = annotacion->value;
                rel.section = assembly_ctx->current_section->name;
                rel.offset  = code_final.offset + 1;
                // se debe sumar 1 ya que aun no se emitio el byte de data(informacion de campos)
                rel.type = size_ptr_in_this_machine; // relocalizacion absoluta segun el tamaño de palabra de la maquina

                assembly_ctx->ctx.add_relocation(rel);
            } else {
                uint8_t mode = 0;
                if (reg != nullptr) {
                    mode = encode_mode(reg->size_bits);
                } else if (mem != nullptr) {
                    reg  = static_cast<vm::RegisterOperand *>(mem->expr.get());
                    mode = encode_mode(reg->size_bits);
                }

                // creamos el valor
                Relocation rel;
                rel.symbol  = annotacion->value;
                rel.section = assembly_ctx->current_section->name;
                rel.offset  = code_final.offset + 1;
                // se debe sumar 1 ya que aun no se emitio el byte de data(informacion de campos)

                // obtener tipo de relocalizacion:
                if (annotacion->key == "Relative") {
                    rel.type = mode_to_type_relocation_rel(mode);
                } else if (annotacion->key == "Absolute") {
                    rel.type = mode_to_type_relocation_abs(mode);
                } else {
                error_notation:
                    throw std::runtime_error("emit_instr_inmed_with_annotation(): No se sopora la "
                        "notacion: " + annotacion->key);
                }

                if (rel.type == Type::NO_VALID) goto error_notation;

                assembly_ctx->ctx.add_relocation(rel);
                val_inmmed = 0; // siempre 0 por que sera remplazado por el
                // linker
            }
        }

        // si el destino es memoria:
        if (mem_dest) {
            // signed = 1 si es memoria o reg extendido
            is_a_signed = 1;

            // obtenemos el registro donde se apunta para escribir
            reg = static_cast<vm::RegisterOperand *>(mem->expr.get());
            if (reg == nullptr) {
                std::cout << "Error instruccion: " + instruction_parser->opcode +
                        " esperaba un registro para acceder a memoria pero obtuvo algo distinto: " << std::endl;
                mem->expr->print(0);
                throw std::runtime_error("Error instruccion: " + instruction_parser->opcode);
            }
        } else {
            std::optional<uint8_t> opt_reg_special = encode_special_register(reg->name);

            // si es un registro especial, signo es 1, sino, es 0 y es un registro general
            is_a_signed = opt_reg_special.has_value();


            // si fue 1, entonces es un registro especial
            if (is_a_signed == 1) {
                uint8_t reg_special = opt_reg_special.value();
                // un registro espcial o extendido normalmente codifica el campo modo
                // como bits extras del registro, lo que hace que ocupe 2 + 4 = 6bits,
                // debemos separa el modo y el registro
                mode = (reg_special >> 4) & 0b11;

                direction = 1; // si es un registro especial, se debe indicar 1.
                code_final.emit8(
                    (mode << 6) |
                    (((is_a_signed) ? 1 : 0) << 5) |
                    (direction << 4) |
                    (reg_special & 0x0F)
                );
                // Se emite una cantidad de bytes de 64bits para q siempre entre un puntero.
                // esto en un futuro puede dar problemas con SP y BP y otros registros especiales
                // al intentar usarlo en 32 bits tal vez.
                size_bytes = sizeof(uint64_t);
                code_final.emit_bytes(&val_inmmed, size_bytes);
                return;
            }
        }
        // si es registro general + inmediato

        // se debe sacar el modo del registro general.
        mode = encode_mode(reg->size_bits);

        // validamos si los tamaños del modo y el inmediato son iguales
        ImmType type            = detect_imm_type((int64_t) val_inmmed, is_a_signed);
        int     instr_imm_bits  = immtype_bits(type);
        int     instr_mode_bits = mode_to_bits(mode);

        if (instr_imm_bits > instr_mode_bits) {
            throw std::runtime_error(
                "emit_instr_mov_inmed() Error: el inmediato " + std::to_string(val_inmmed) +
                " (" + std::to_string(instr_imm_bits) + " bits) no cabe en el modo " +
                std::to_string(mode) + " (" + std::to_string(instr_mode_bits) + " bits)."
            );
        }

        // el signo y la direccion en este caso siempre es 0 segun la expecificacion.
        // is_a_signed = 0; // no descomentar, si se usa memoria, esto sera 1.
        direction = 0;

        code_final.emit8(
            (mode << 6) |
            (((is_a_signed) ? 1 : 0) << 5) |
            (direction << 4) |
            encode_reg_general(reg->name.c_str())
        );

        // emitimos tantos bytes de inmediato como se especificara segun el tamaño del registro general
        code_final.emit_bytes(&val_inmmed, mode_to_bytes(mode));
    }

    void emit_instr_calln_inmmed(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    ) {
        auto method_annotation_node = instruction_parser->operands[0].get();
        auto method                 = dynamic_cast<vm::AnnotationNode *>(method_annotation_node);
        if (method == nullptr) {
            throw std::runtime_error(
                "emit_instr_calln_inmmed() Error: la instruccion " + instruction_parser->opcode +
                " esperaba una notacion del tipo @Method con el metodo a llamar, pero no se pudo situar"
                "una notacion de este tipo."
            );
        }

        Relocation rel;
        rel.symbol = method->value; // nombre del metodo con la libreria que lo contiene
        // Ejemplo: @Method("kernel32.dll:GetTickCount"), value contiene "kernel32.dll:GetTickCount"
        rel.section = assembly_ctx->current_section->name;
        rel.type    = Type::Native_Method; // es un metodo nativo
        // el offset debe obtenerse antes de emitir los bytes del placeholder
        rel.offset = code_final.offset;

        uint64_t placeholder = 0x1122334455667788;
        code_final.emit_bytes(&placeholder, size_relocation_emmit(rel.type));

        assembly_ctx->ctx.add_relocation(rel);
    }

    void emit_instr_mov_sib(
        const vm::Instruction *instruction_parser,
        ByteWriter &           code_final,
        const InstrInfo *      now_instr,
        Assembler *            assembly_ctx
    ) {}
}
