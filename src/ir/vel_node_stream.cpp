/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file ir/vel_node_stream.cpp
 * @brief Fabrica los nodos del ensamblador desde los items del emisor.  El
 *        porque esta en la cabecera.
 */

#include "ir/vel_node_stream.h"

/* `annotations.h` va PRIMERO y no es un capricho de orden: declara adelantado
 * `AnnotationNode`, que `parser/parser.h` necesita y que `parser/ast.h` define
 * DESPUES de incluirlo.  Es el orden por el que entran los demas. */
#include "emmit/annotations.h"
#include "ir/vel_sink.h"
#include "lexer/lexer.h"
#include "parser/ast.h"
#include "parser/parser.h"

#include <iomanip>
#include <sstream>

namespace ir {

namespace {

/**
 * @brief El texto con el que el `.vel` escribe un inmediato.
 *
 * Tiene que salir EXACTAMENTE como lo escribe el renderizador: el numero
 * acaba en un `NumberOperand`, que lo guarda como cadena, y el ensamblador lo
 * vuelve a leer de ahi.  Si aqui saliera `255` donde el texto ponia `0x00ff`,
 * el valor seria el mismo pero el `.velb` no tiene por que serlo -- y lo que
 * se quiere poder afirmar es que es identico byte a byte.
 */
std::string immediate_text(const emmit::Operand &o) {
    std::ostringstream os;
    if (o.imm_digitos_hex > 0) {
        os << "0x" << std::hex << std::setw(o.imm_digitos_hex)
           << std::setfill('0') << static_cast<uint64_t>(o.imm);
    } else if (o.imm_sin_signo) {
        os << static_cast<uint64_t>(o.imm);
    } else {
        os << o.imm;
    }
    return os.str();
}

/// El nombre con sufijo de ancho, tal como lo escribe el `.vel` (`r15d`).
std::string register_text(const ir::Reg &r) {
    std::ostringstream os;
    os << r;
    return os.str();
}

/// Cuantos bits ve esa vista del registro.  Es lo que el AST llama
/// @c size_bits y lo que el ensamblador usa para el byte de control.
int register_bits(const ir::Reg &r) {
    switch (r.width) {
    case ir::Reg::Width::B: return 8;
    case ir::Reg::Width::W: return 16;
    case ir::Reg::Width::D: return 32;
    case ir::Reg::Width::Q: return 64;
    }
    return 64;
}

/**
 * @brief Un registro, como nodo, REUSANDO uno de la vuelta anterior si lo hay.
 *
 * POR QUE MERECE UN POZO.  Es el nodo que mas se fabrica de todo el
 * compilador: 1.151.996 reservas de 48 bytes al ensamblar 144.000 lineas, una
 * por cada operando de registro de cada instruccion.  Y todas viven lo mismo
 * -- hasta la llamada siguiente a @c next --, que es justo lo que hace que se
 * puedan reciclar en vez de pedirlos y devolverlos.
 *
 * NO CAMBIA QUIEN LOS POSEE.  Siguen siendo `unique_ptr` dentro del nodo de la
 * instruccion; lo unico que cambia es que al fabricar la siguiente se sacan de
 * ahi en vez de destruirse.  El contrato que ya prometia el flujo -- el nodo
 * entregado vale hasta la proxima llamada -- es exactamente el que hace esto
 * legitimo: reusar la memoria del anterior no acorta ninguna vida que alguien
 * pudiera estar mirando.
 *
 * @param r     El registro.
 * @param spare Nodos de vueltas anteriores.  Se saca del final si hay.
 */
std::unique_ptr<vm::ASTNode>
make_register(const ir::Reg &r,
              std::vector<std::unique_ptr<vm::ASTNode>> &spare) {
    if (!spare.empty()) {
        std::unique_ptr<vm::ASTNode> n = std::move(spare.back());
        spare.pop_back();
        auto *reg = static_cast<vm::RegisterOperand *>(n.get());
        /* Se ASIGNA, no se construye: la cadena del nombre conserva su reserva
         * y un nombre de registro cabe de sobra en ella. */
        reg->name = register_text(r);
        reg->size_bits = register_bits(r);
        return n;
    }
    return std::make_unique<vm::RegisterOperand>(register_text(r),
                                                 register_bits(r));
}

/// Un numero, como nodo.
///
/// El tipo de token tiene que ser el que el lexer le habria puesto al leer ese
/// mismo texto: decimal o hexadecimal segun como se escribio.  No es
/// cosmetica -- hay quien lo mira para decidir el ancho del inmediato --.
std::unique_ptr<vm::ASTNode> make_number(const std::string &text) {
    const bool hex = text.size() > 2 && text[0] == '0' &&
                     (text[1] == 'x' || text[1] == 'X');
    return std::make_unique<vm::NumberOperand>(
        text, hex ? vm::TokenType::NUMBER_HEX : vm::TokenType::NUMBER_DEC);
}

/// Junta dos con un operador, como hace `parse_mem_expression`.
///
/// El molde `BinaryExpr` pide `ExprNode`, y los operandos no lo son -- el
/// parser hace el MISMO reinterpretado al construirlos --.  Se reproduce tal
/// cual a proposito: lo que el ensamblador mira es la ETIQUETA de tipo del
/// nodo, no por donde le llego el puntero, y cualquier otra forma daria un
/// arbol distinto del que produce el texto.
std::unique_ptr<vm::ASTNode> join(char op, std::unique_ptr<vm::ASTNode> a,
                                  std::unique_ptr<vm::ASTNode> b) {
    return std::make_unique<vm::BinaryExpr>(
        op,
        std::unique_ptr<vm::ExprNode>(
            static_cast<vm::ExprNode *>(a.release())),
        std::unique_ptr<vm::ExprNode>(
            static_cast<vm::ExprNode *>(b.release())));
}

/**
 * @brief Un acceso a memoria, con la forma que produce el parser.
 *
 * `ir::Mem` solo tiene tres formas, asi que esto las cubre todas:
 *   `[base]`                    -> el registro pelado
 *   `[base + index * escala]`   -> suma, y el producto dentro si la escala
 *                                  no es uno
 *   `[base +/- desplazamiento]` -> suma o resta con el valor SIN signo, que es
 *                                  como lo escribe el renderizador
 *
 * La asociatividad es a la IZQUIERDA porque el parser junta en un bucle: con
 * indice y desplazamiento a la vez sale `((base + indice) + desp)`.
 */
std::unique_ptr<vm::ASTNode>
make_memory(const ir::Mem &m,
            std::vector<std::unique_ptr<vm::ASTNode>> &spare) {
    std::unique_ptr<vm::ASTNode> expr = make_register(m.base, spare);
    if (m.hay_index) {
        std::unique_ptr<vm::ASTNode> idx = make_register(m.index, spare);
        if (m.scale != 1)
            idx = join('*', std::move(idx),
                       make_number(std::to_string(m.scale)));
        expr = join('+', std::move(expr), std::move(idx));
    }
    if (m.disp > 0) {
        expr = join('+', std::move(expr), make_number(std::to_string(m.disp)));
    } else if (m.disp < 0) {
        expr = join('-', std::move(expr), make_number(std::to_string(-m.disp)));
    }
    return std::make_unique<vm::MemoryOperand>(std::move(expr));
}

} // namespace

/**
 * @brief El estado del recorrido.
 *
 * Va en un `Impl` para que la cabecera no tenga que ver ni el AST ni el
 * modelo de items: quien la incluya no arrastra ninguno de los dos.
 */
struct VelNodeStream::Impl {
    /* NO es `const` porque esta vista puede SOLTAR lo que lee cuando el
     * consumidor termina; todo el recorrido de aqui abajo sigue leyendolo por
     * referencia constante.  Ver `release_source`. */
    VelSink &sink;
    /// Los nodos de la cabecera (`@Format`, `@Section`...), parseados UNA vez:
    /// son quince lineas y no tienen item del que fabricarlos.
    std::vector<std::unique_ptr<vm::ASTNode>> header;
    /// Por donde va el recorrido: primero la cabecera, luego los items.
    size_t at_header = 0;
    size_t at_item = 0;
    /**
     * @brief Nodos de registro de vueltas anteriores, para no volver a pedirlos.
     *
     * Es el nodo que mas se fabrica de todo el compilador -- 1.151.996 al
     * ensamblar 144.000 lineas -- y todos viven lo mismo: hasta la llamada
     * siguiente.  Ver @c make_register.
     */
    std::vector<std::unique_ptr<vm::ASTNode>> spare_regs;
    /// El nodo que se entrego ultimo.  Vive hasta la siguiente llamada, que es
    /// lo que promete el contrato.
    std::unique_ptr<vm::ASTNode> current;
    /// La marca pendiente, que se aplica a la instruccion SIGUIENTE.
    int pending_line = 0;
    int pending_column = 0;
    std::string pending_stackmap;

    explicit Impl(VelSink &s) : sink(s) {}
};

VelNodeStream::VelNodeStream(VelSink &sink)
    : impl_(new Impl(sink)) {
    /* La cabecera: los crudos que van ANTES del primer item tipado.  Se juntan
     * y se parsean de una vez.  Lo que venga despues tiene que ser comentario
     * o espacio; cualquier otra cosa es algo que no sabemos fabricar, y
     * entonces se renuncia ENTERA en vez de entregar un programa incompleto. */
    std::string header_text;
    bool seen_typed = false;
    for (const VelSink::Ref &r : sink.orden_) {
        if (r.tipo == VelSink::TipoItem::Crudo) {
            const std::string &raw = sink.crudos_[r.idx];
            if (!seen_typed) {
                header_text += raw;
                continue;
            }
            /* Un crudo en mitad del cuerpo.  Si solo son comentarios y lineas
             * en blanco no aporta nada al programa y se puede ignorar; el
             * emisor escribe asi las funciones nativas.  Si lleva algo mas, no
             * se puede. */
            size_t p = 0;
            while (p < raw.size()) {
                const size_t nl = raw.find('\n', p);
                const std::string ln =
                    raw.substr(p, nl == std::string::npos ? std::string::npos
                                                          : nl - p);
                const size_t a = ln.find_first_not_of(" \t\r");
                if (a != std::string::npos && ln.compare(a, 2, "//") != 0) {
                    ok_ = false;
                    why_not_ = "texto sin forma propia en el cuerpo: " +
                               ln.substr(a);
                    return;
                }
                if (nl == std::string::npos) break;
                p = nl + 1;
            }
            continue;
        }
        seen_typed = true;
    }

    if (!header_text.empty()) {
        try {
            vm::Lexer lexer(header_text);
            vm::Parser parser(lexer);
            impl_->header = parser.parse();
        } catch (const std::exception &e) {
            ok_ = false;
            why_not_ = std::string("la cabecera del .vel no se pudo leer: ") +
                       e.what();
        }
    }
}

VelNodeStream::~VelNodeStream() = default;

void VelNodeStream::release_source() {
    /* Lo del emisor, que es lo que pesa.  Y AL DEJAR EL RECORRIDO AL FINAL:
     * sin eso, un `rewind()` posterior empezaria a andar sobre unos vectores
     * que ya no tienen nada y entregaria la cabecera como si fuera el programa.
     * El contrato dice que despues de esto el flujo esta agotado, y esto es lo
     * que lo hace cierto en vez de dejarlo escrito. */
    impl_->sink.release();
    /* La cabecera tambien: son quince lineas, pero sus nodos ya no van a salir
     * y quedarse con ellos es quedarse con un arbol que no sirve. */
    std::vector<std::unique_ptr<vm::ASTNode>>().swap(impl_->header);
    impl_->current.reset();
    impl_->at_header = 0;
    impl_->at_item = 0;
}

void VelNodeStream::rewind() {
    impl_->at_header = 0;
    impl_->at_item = 0;
    impl_->current.reset();
    impl_->pending_line = 0;
    impl_->pending_column = 0;
    impl_->pending_stackmap.clear();
}

const vm::ASTNode *VelNodeStream::next() {
    /* La cabecera primero, entera.  Sus nodos son de `impl_->header`, que vive
     * lo que la fuente, asi que no hace falta guardarlos en `current`. */
    if (impl_->at_header < impl_->header.size())
        return impl_->header[impl_->at_header++].get();

    const VelSink &s = impl_->sink;
    while (impl_->at_item < s.orden_.size()) {
        const VelSink::Ref r = s.orden_[impl_->at_item++];
        switch (r.tipo) {
        case VelSink::TipoItem::Crudo:
            /* Cabecera ya consumida, o comentario: no produce nodo.  Que se
             * puede ignorar se comprobo al construir; aqui ya es seguro. */
            continue;

        case VelSink::TipoItem::Marca: {
            /* No es un nodo: es lo que le toca a la instruccion siguiente.  Y
             * por eso es un item aparte -- hay instrucciones del IR que no
             * producen bytecode, asi que la marca cae ENTRE dos emitidas y su
             * posicion es un dato. */
            const VelSink::Marca &m = s.marcas_[r.idx];
            if (m.linea != 0) {
                impl_->pending_line = m.linea;
                impl_->pending_column = m.columna;
            }
            if (!m.stackmap.empty()) impl_->pending_stackmap = m.stackmap;
            continue;
        }

        case VelSink::TipoItem::Etiqueta: {
            /* Sin cuerpo: las instrucciones llegan detras.  Ver
             * `labels_are_flat`. */
            impl_->current = std::make_unique<vm::LabelNode>(
                s.etiquetas_[r.idx],
                std::vector<std::unique_ptr<vm::ASTNode>>{});
            return impl_->current.get();
        }

        case VelSink::TipoItem::Instr: {
            const emmit::Instr &in = s.instrs_[r.idx];
            /* AL POZO LO DE LA VUELTA ANTERIOR, antes de fabricar nada.
             * El nodo que se entrego la vez pasada muere aqui por contrato, y
             * sus operandos de registro son el sitio que mas reserva de todo
             * el compilador -- 1.151.996 veces --, asi que en vez de
             * destruirlos se guardan para rellenarlos otra vez.  Solo los de
             * REGISTRO: los demas no compensan el rodeo, y anadirlos "por
             * simetria" seria codigo sin medida que lo respalde. */
            if (impl_->current != nullptr &&
                impl_->current->node_kind() == vm::NodeKind::InstructionK) {
                auto *prev =
                    static_cast<vm::Instruction *>(impl_->current.get());
                for (std::unique_ptr<vm::ASTNode> &op : prev->operands)
                    if (op != nullptr &&
                        op->node_kind() == vm::NodeKind::Register)
                        impl_->spare_regs.push_back(std::move(op));
            }
            std::vector<std::unique_ptr<vm::ASTNode>> ops;
            ops.reserve(in.n_ops);
            for (unsigned i = 0; i < in.n_ops; ++i)
                ops.push_back(
                    make_operand(s.ops_pool_[in.ops_off + i], impl_->spare_regs));

            auto node = std::make_unique<vm::Instruction>(
                emmit::text_of(in.mnem), std::move(ops));
            node->source_line = impl_->pending_line;
            node->source_column = impl_->pending_column;
            node->stackmap_hex = impl_->pending_stackmap;
            /* LA LINEA NO SE CONSUME, el stackmap SI.  No es una eleccion:
             * es lo que hace el parser, que lee `lexer.last_src_line` sin
             * borrarlo y solo limpia `last_src_stackmap`.  O sea que una linea
             * vale para todas las instrucciones hasta el marcador siguiente.
             *
             * Consumirla parecia lo correcto -- y costo 16 entradas de 25 en
             * la tabla de depuracion del primer ejemplo que lo destapo --.
             * Aqui no se trata de decidir que esta bien, sino de dar lo MISMO
             * que el texto: el dia que se cambie, se cambia en los dos. */
            impl_->pending_stackmap.clear();

            impl_->current = std::move(node);
            return impl_->current.get();
        }

        case VelSink::TipoItem::Datos: {
            const VelSink::Datos &d = s.datos_[r.idx];
            std::vector<std::unique_ptr<vm::ExprNode>> valores;
            if (d.es_texto) {
                valores.push_back(std::make_unique<vm::StringExpr>(d.texto));
            } else {
                valores.reserve(d.valores.size());
                for (const emmit::Operand &o : d.valores)
                    valores.push_back(make_data_value(o, impl_->spare_regs));
            }
            /* La directiva es TEXTO en el nodo (`db`, `dq`), que es como la
             * deja el parser al leerla del `.vel`.  Las mismas dos palabras
             * que escribe el renderizador. */
            impl_->current = std::make_unique<vm::DataDecl>(
                d.nombre, d.ancho_q ? "dq" : "db", std::move(valores));
            return impl_->current.get();
        }
        }
    }
    return nullptr;
}

std::unique_ptr<vm::ExprNode>
VelNodeStream::make_data_value(const emmit::Operand &o,
                               std::vector<std::unique_ptr<vm::ASTNode>> &spare) {
    /* Dentro de un bloque de datos el parser produce OTRA cosa para lo mismo:
     * un `@Absolute("x")` es aqui un `AbsRefExpr`, no una anotacion.  Son dos
     * caminos distintos del parser (`parse_data_values` frente a
     * `parse_operand`) y el ensamblador mira la etiqueta del nodo, asi que
     * entregar el de al lado no da un error -- da otro dato. */
    if (o.kind == emmit::OperandKind::SymRef)
        return std::make_unique<vm::AbsRefExpr>(o.name_text());
    return std::unique_ptr<vm::ExprNode>(
        static_cast<vm::ExprNode *>(make_operand(o, spare).release()));
}

std::unique_ptr<vm::ASTNode>
VelNodeStream::make_operand(const emmit::Operand &o,
                            std::vector<std::unique_ptr<vm::ASTNode>> &spare) {
    switch (o.kind) {
    case emmit::OperandKind::Reg: return make_register(o.reg, spare);
    case emmit::OperandKind::Mem: return make_memory(o.mem, spare);
    case emmit::OperandKind::Label:
        return std::make_unique<vm::LabelOperand>(o.name_text());
    case emmit::OperandKind::SymRef:
        /* Una anotacion, no un `AbsRefExpr`.  Los dos existen y NO son
         * intercambiables: en un operando de instruccion el parser devuelve lo
         * que da `parse_annotation` -- un `AnnotationNode` con la clave y el
         * simbolo --, y `AbsRefExpr` solo aparece dentro de un bloque de
         * datos, que es otro camino del parser.  Ver `make_data_value`. */
        return std::make_unique<vm::AnnotationNode>(
            emmit::text_of(o.sym_kind), o.name_text());
    case emmit::OperandKind::Imm: return make_number(immediate_text(o));
    case emmit::OperandKind::None: break;
    }
    return nullptr;
}

} // namespace ir
