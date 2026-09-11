/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file ir/module_spill.h
 * @brief Bajar a disco los cuerpos de un modulo y volver a traerlos.
 *
 * @par Que problema resuelve
 * El pico de memoria del compilador es, medido, el conjunto de trabajo de UN
 * modulo multiplicado por CUANTOS modulos hay: 180 MiB x 21 + 24 = 3.804 MiB
 * observados contra 3.815.  Y no es que cada modulo necesite estar vivo -- una
 * vez compilado, de su intermedio no se vuelve a preguntar nada hasta que se
 * funden todos al final.
 *
 * El AST y el comprobador de tipos ya se sueltan en cuanto sobran.  Lo que
 * queda residente son los CUERPOS, y son la parte gorda.
 *
 * @par Desalojar no es borrar
 * Esta es la regla, y gobierna todo lo de aqui: **soltar la RAM nunca puede
 * perder conocimiento**.  Los bytes estan en disco ANTES de que la memoria se
 * libere, y volver a traerlos reconstruye exactamente lo que habia.  Un modulo
 * desalojado no sabe menos que uno residente: sabe lo mismo y tarda mas en
 * contestar.
 *
 * Por eso el formato es el de la cache de modulos (@c emit_ir_module_cache /
 * @c parse_ir_module_cache) y no uno propio: es el que ya lleva el `.vxir`, y
 * el que el camino de acierto de cache usa para alimentar la fusion.  Si un
 * modulo servido de cache produce el mismo programa -- y lo produce: su
 * contenido sale con el mismo hash en frio y en caliente --, un modulo
 * desalojado y recuperado tambien.
 *
 * @par Y se desaloja SOLO lo gordo
 * Se bajan los cuerpos (@c IrModule::functions) y nada mas.  Las clases, las
 * vistas `@overlay`, el nombre del modulo o los simbolos del asignador son
 * kilobytes y se quedan donde estan: bajarlos no ahorraria nada y el formato
 * de la cache no los lleva, asi que bajarlos SI perderia informacion.
 */

#ifndef VESTA_IR_MODULE_SPILL_H
#define VESTA_IR_MODULE_SPILL_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ir {

struct IrModule;
struct IrFunction;

/**
 * @brief Cuanta RAM ocupan estos cuerpos, aproximadamente.
 *
 * Suma las capacidades de los contenedores que pesan -- el pozo de valores, los
 * bloques, las instrucciones de cada uno -- y el monton que usan las cadenas
 * que no caben en el objeto.
 *
 * @par Que NO cuenta, y por que se dice
 * No cuenta el desperdicio del asignador (redondeo a clase de tamano, cabecera
 * por bloque), que es real y puede ser un tercio mas.  Ni los campos pequenos
 * por funcion -- contratos, ataduras de asm, la expresion de coste --, que son
 * decenas de bytes frente a los kilobytes de un cuerpo.
 *
 * O sea que devuelve una COTA INFERIOR, y quien ponga el techo tiene que
 * saberlo: el proceso ocupara algo mas que lo que aqui se cuenta.  Decirlo es
 * mejor que afinar un numero que de todas formas depende del asignador.
 *
 * @param fns Los cuerpos a medir.
 * @return Bytes, aproximados por abajo.
 */
size_t functions_footprint(const std::vector<IrFunction> &fns);

/**
 * @brief Deja los cuerpos de @p mod a salvo en disco y suelta su RAM.
 *
 * @param mod  El modulo.  Al volver, @c mod.functions queda VACIO y sin
 *             reserva; el resto del modulo no se toca.
 * @param path Donde tienen que quedar los bytes.
 * @param already_written Cierto si en @p path estan YA los bytes de @p mod tal
 *        como esta ahora -- que es el caso cuando la cache de modulos acaba de
 *        escribir su `.vxir` --.  Entonces no se vuelve a serializar.
 *
 *        INVARIANTE del llamante: entre escribir ese fichero y llamar aqui, el
 *        intermedio del modulo NO se modifica.  Si algun dia se modificara,
 *        quien lo haga tiene que pasar @c false.  No se comprueba aqui porque
 *        comprobarlo cuesta serializar, que es justo lo que este atajo evita --
 *        pero @ref restore_functions si grita si lo que vuelve no cuadra.
 *
 * @return true si los bytes quedaron a salvo y la RAM se solto.  Si algo fallo
 *         al escribir, NO se suelta nada y devuelve false: quedarse sin memoria
 *         es un problema, perder el programa es otro.
 */
bool spill_functions(IrModule &mod, const std::string &path,
                     bool already_written);

/**
 * @brief Vuelve a traer a @p mod los cuerpos que @ref spill_functions bajo.
 *
 * @param mod      El modulo, con @c functions vacio.
 * @param path     El fichero que dejo el desalojo.
 * @param expected Cuantas funciones habia al bajarlas.  Si vuelven otras
 *                 tantas distintas, se DICE y se devuelve false en vez de
 *                 seguir con un modulo a medias -- un cuerpo que falta no da
 *                 un error al fundir, da un simbolo sin resolver mucho
 *                 despues, o peor, otro programa.
 * @param err      Que paso, si fallo.
 * @return true si el modulo quedo como estaba.
 */
bool restore_functions(IrModule &mod, const std::string &path, size_t expected,
                       std::string &err);

} // namespace ir

#endif // VESTA_IR_MODULE_SPILL_H
