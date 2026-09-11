/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file emmit/node_stream.h
 * @brief De donde saca el ensamblador los nodos que recorre.
 *
 * En una cabecera propia, y minima a proposito: quien PRODUCE nodos no tiene
 * por que arrastrar el ensamblador entero para decir que sabe producirlos.  El
 * emisor del intermedio es justo ese caso.
 */

#ifndef EMMIT_NODE_STREAM_H
#define EMMIT_NODE_STREAM_H

namespace vm {
struct ASTNode;
}

namespace emmit {

/**
 * @brief De donde salen los nodos que el ensamblador recorre.
 *
 * @par Por que existe
 * El ensamblador recorre el programa DOS veces -- una para colocar secciones y
 * etiquetas, otra para emitir bytes --, y para eso pedia el arbol entero de una
 * pieza.  Eso obliga a que el arbol exista, y el arbol entero es la mitad del
 * pico de memoria del compilador: 2,5 millones de nodos y 1,7 millones de
 * operandos en el monton, construidos a partir de un texto que el propio
 * compilador acababa de escribir.
 *
 * Lo que el ensamblador necesita no es TENER los nodos: es poder RECORRERLOS EN
 * ORDEN, dos veces.  Quien los tenga ya hechos los entrega; quien los pueda
 * fabricar los fabrica de uno en uno y reutiliza el sitio.
 *
 * @par El contrato
 * @ref rewind vuelve al principio y @ref next da el siguiente o @c nullptr al
 * acabar.  **Las dos vueltas tienen que dar la MISMA secuencia**: el
 * ensamblador calcula desplazamientos en la primera y los usa en la segunda,
 * asi que una diferencia entre pasadas no da un error -- da otro programa.
 *
 * El nodo devuelto tiene que seguir siendo valido hasta la siguiente llamada a
 * @ref next, no mas: quien fabrique puede tener UN nodo vivo.
 */
class NodeStream {
  public:
    virtual ~NodeStream() = default;

    /// Vuelve al principio.  Se llama antes de cada pasada.
    virtual void rewind() = 0;

    /// El siguiente nodo raiz, o @c nullptr si se acabo.
    virtual const vm::ASTNode *next() = 0;

    /**
     * @brief Las instrucciones de una etiqueta llegan SUELTAS detras de ella,
     *        en vez de dentro de su @c body.
     *
     * Una fuente que fabrica nodos de uno en uno no puede meterlos dentro de la
     * etiqueta: para eso tendria que tener a la vez la etiqueta y todo su
     * cuerpo, que es justo lo que se viene a evitar.  Los entrega en orden y
     * planos, y el ensamblador cierra cada etiqueta cuando llega la siguiente.
     *
     * SE DECLARA, no se adivina.  La tentacion es deducirlo de que el cuerpo
     * venga vacio, y esta mal: una etiqueta VACIA seguida de una anotacion
     * `@Section` es un caso real -- el propio `first_pass` lo contempla --, y
     * confundirla con el modo plano le daria el tamano de todo lo que viene
     * detras.
     *
     * @return true si el flujo es plano.  Por defecto no.
     */
    virtual bool labels_are_flat() const { return false; }
};

} // namespace emmit

#endif // EMMIT_NODE_STREAM_H
