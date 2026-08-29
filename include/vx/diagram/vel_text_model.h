/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file vel_text_model.h
 * @brief Lo que un diagrama sabe LEER de un texto @c .vel, con independencia
 *        del formato en el que luego lo dibuje.
 *
 * Los generadores de diagramas (@c graphviz_diagrams.cpp, @c
 * mermaid_diagrams.cpp) responden a dos preguntas muy distintas, y conviene
 * no mezclarlas:
 *
 *   1. QUE dice el programa -- cual es el mnemonico de una linea, si esa
 *      linea es una etiqueta, si la instruccion es una llamada, a donde
 *      salta, si es una de las instrucciones FUSIONADAS.  La respuesta no
 *      depende en absoluto de si el dibujo acabara siendo DOT o Mermaid.
 *   2. COMO se dibuja -- forma del nodo, color, sintaxis de la arista.  Eso
 *      si es propio de cada backend.
 *
 * Este fichero es la respuesta a la PRIMERA pregunta, escrita una sola vez.
 * Estaba duplicada en los dos backends, y la duplicacion ya habia costado:
 * @c is_fused_instr solo existia en el generador DOT -- el de Mermaid nunca
 * resaltaba un bloque con instrucciones fusionadas, pese a que su cabecera
 * promete "misma topologia, misma cantidad de informacion" -- y ademas se
 * habia quedado en las seis fusiones que existian cuando se escribio.
 */

#ifndef VX_DIAGRAM_VEL_TEXT_MODEL_H
#define VX_DIAGRAM_VEL_TEXT_MODEL_H

#include <string>

namespace vx {

/**
 * @brief Mnemonico de una linea @c .vel: la primera palabra, sin la sangria.
 *
 * @param line Linea de texto tal cual aparece en el fuente.
 * @return El mnemonico, o cadena vacia si la linea no tiene ninguno.
 */
std::string get_mnemonic(const std::string &line);

/**
 * @brief Indica si el mnemonico transfiere el control a otro cuerpo de
 *        codigo: llamada, despacho de metodo, cierre, spawn o carga de modulo.
 *
 * Sirve para dar a esos nodos un realce propio en el diagrama, porque son los
 * puntos donde el flujo se va del bloque que se esta mirando.
 *
 * @param mn Mnemonico ya extraido con @c get_mnemonic.
 * @return true si la instruccion salta a otro cuerpo de codigo.
 */
bool is_call_mnemonic(const std::string &mn);

/**
 * @brief Indica si la instruccion es una de las FUSIONADAS: una sola
 *        instruccion de la maquina virtual que hace el trabajo de dos o tres.
 *
 * El diagrama las resalta para que se vea de un vistazo donde el emisor de IR
 * consiguio fusionar.  La lista hay que mantenerla a mano porque la tabla de
 * instrucciones (@c include/emmit/parser_to_bytecode.h) no marca cuales lo
 * son; al anadir una fusion nueva al lenguaje, se anade aqui.  Ese es
 * justamente el motivo de que exista este fichero: la lista estaba escrita en
 * un solo backend y se habia quedado en las seis primeras, asi que las
 * fusiones MAS frecuentes -- las de tres operandos y las cargas con extension
 * a cero, que el emisor produce en casi cualquier bucle -- se dibujaban como
 * un bloque cualquiera.
 *
 * @param mn Mnemonico ya extraido con @c get_mnemonic.
 * @return true si la instruccion es una fusion.
 */
bool is_fused_instr(const std::string &mn);

/**
 * @brief Reconoce una linea que solo declara una etiqueta (@c "nombre:").
 *
 * Exige que tras los dos puntos no quede mas que espacios o un comentario:
 * una linea con codigo detras no es una etiqueta suelta y no debe abrir un
 * bloque nuevo en el diagrama.
 *
 * @param line     Linea de texto a examinar.
 * @param name_out Recibe el nombre de la etiqueta si la linea lo es.
 * @return true si la linea es unicamente una etiqueta.
 */
bool is_label_line(const std::string &line, std::string &name_out);

/**
 * @brief Extrae el destino de un salto escrito como @c @Absolute("code.X").
 *
 * @param line Linea de texto de la instruccion de salto.
 * @return El nombre de la etiqueta destino, o cadena vacia si la linea no
 *         lleva esa forma.
 */
std::string extract_abs_target(const std::string &line);

} // namespace vx

#endif // VX_DIAGRAM_VEL_TEXT_MODEL_H
