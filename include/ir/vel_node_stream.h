/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file ir/vel_node_stream.h
 * @brief Da de comer al ensamblador lo que el emisor ya tenia, sin pasar por
 *        texto.
 *
 * @par El rodeo que quita
 * El emisor tiene cada instruccion TIPADA -- mnemonico y operandos, en
 * `emmit::Instr` --, la escribe como texto `.vel`, y el ensamblador lexa y
 * parsea esos 45 MB para reconstruir la misma estructura.  Medido sobre 21
 * modulos y 441.000 lineas: lexar y parsear es de 930 ms de los 1.873 que
 * cuesta compilar -- la MITAD --, y el arbol que produce son 2,5 millones de
 * nodos y 1,7 millones de operandos vivos a la vez, en la fase donde esta el
 * pico de memoria.
 *
 * Esta fuente entrega los mismos nodos SIN el rodeo, y de uno en uno: hay un
 * nodo vivo, no un arbol.
 *
 * @par Por que fabrica nodos y no algo mejor
 * Porque las 54 funciones de emision del ensamblador reciben
 * `const vm::Instruction *`, y cambiarles la firma es otra tanda.  Fabricar el
 * nodo que esperan cuesta una reserva por operando -- trafico, que se recicla
 * al momento -- y a cambio quita el texto, el lexer, el parser y el arbol
 * entero.  Cuando esas funciones hablen `emmit::Instr`, esto se queda en nada.
 */

#ifndef IR_VEL_NODE_STREAM_H
#define IR_VEL_NODE_STREAM_H

#include "emmit/instr.h"       // emmit::Operand, lo que se convierte
#include "emmit/node_stream.h" // el contrato que se implementa

#include <memory>
#include <string>
#include <vector>

namespace vm {
struct ASTNode;
struct ExprNode;
}

namespace ir {

class VelSink;

/**
 * @brief Fuente de nodos respaldada por los items de un @ref VelSink.
 *
 * @par Cuando NO puede
 * El emisor escribe la cabecera del `.vel` (`@Format`, `@SpaceAddress`,
 * `@Section`, `@Module`) como texto crudo, y esa parte se parsea igual que
 * antes: son quince lineas.  Lo que NO puede es texto crudo con contenido en
 * mitad del cuerpo, porque no hay item del que fabricar su nodo.
 *
 * Cuando pasa, @ref ok queda en falso y quien la use tiene que volver al
 * camino de texto.  **No se inventa nada ni se salta nada en silencio**: una
 * instruccion que no llegue al ensamblador no da un error, da otro programa.
 */
class VelNodeStream final : public emmit::NodeStream {
  public:
    /**
     * @param sink El emisor con los items ya escritos.  Tiene que seguir vivo
     *             mientras esta fuente se use.
     *
     * NO ES `const`, y antes lo era.  Leer no modifica nada -- y todo el
     * recorrido de ahi dentro sigue siendo por referencia constante --, pero
     * esta vista ademas puede SOLTAR lo que lee cuando se lo piden, y eso si lo
     * modifica.  Declararlo es lo honesto: quitar la constancia con un cast
     * seria mentir sobre lo que hace.  Ver @c release_source.
     */
    explicit VelNodeStream(VelSink &sink);
    ~VelNodeStream() override;

    void rewind() override;
    const vm::ASTNode *next() override;
    /// Las instrucciones llegan detras de su etiqueta, no dentro.
    bool labels_are_flat() const override { return true; }

    /**
     * @brief Suelta los items del emisor: 85,6 MiB que si no viven hasta el
     *        final del enlazado sin que nadie los mire.
     *
     * Lo pide el ensamblador cuando ya ha dado todas sus pasadas.  Despues de
     * esto el flujo esta AGOTADO -- ver @c emmit::NodeStream::release_source --,
     * asi que no se llama entre pasada y pasada.
     */
    void release_source() override;

    /// Si se pudo cubrir TODO lo emitido.  Con falso, hay que usar el texto.
    bool ok() const { return ok_; }
    /// Que fue lo que no se pudo cubrir, para decirlo.
    const std::string &why_not() const { return why_not_; }

  private:
    /// Un operando del emisor, como el nodo que el ensamblador espera.
    /// @param spare Nodos de registro reciclables; ver @c make_register en el
    ///              `.cpp`.  Sigue siendo estatica -- no mira el estado del
    ///              recorrido --, asi que el pozo entra por la puerta.
    static std::unique_ptr<vm::ASTNode>
    make_operand(const emmit::Operand &o,
                 std::vector<std::unique_ptr<vm::ASTNode>> &spare);
    /// Lo mismo DENTRO de un bloque de datos, donde el parser produce otra
    /// cosa para el mismo operando.
    static std::unique_ptr<vm::ExprNode>
    make_data_value(const emmit::Operand &o,
                    std::vector<std::unique_ptr<vm::ASTNode>> &spare);

    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool ok_ = true;
    std::string why_not_;
};

} // namespace ir

#endif // IR_VEL_NODE_STREAM_H
