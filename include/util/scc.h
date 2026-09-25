/**
 * @file scc.h
 * @brief Componentes fuertemente conexas de un grafo dirigido, por indices.
 *
 * Vive en `util/` y no junto a quien la usa porque la necesitan dos capas que
 * no se pueden ver entre si: la cache incremental (`vx/`), que agrupa los
 * simbolos que dependen unos de otros en ciclo, y el optimizador del intermedio
 * (`ir/`), que necesita saber que funciones se llaman en ciclo para no
 * desbocarse al inlinar.  Estaba escrita dentro de la primera, y la segunda iba
 * a necesitar la suya: dos copias del mismo algoritmo acaban diciendo cosas
 * distintas en cuanto una se arregla y la otra no.
 *
 * @par El grafo, PLANO
 * Se recibe en forma de filas comprimidas: un array de desplazamientos y otro
 * de destinos, los dos contiguos.  Los vecinos de @c v son
 * @c targets[offsets[v] .. offsets[v+1]).
 *
 * No una lista de listas, por dos razones.  Una lista por nodo es una reserva
 * por nodo -- decenas de miles en un modulo grande, cada una de pocos bytes --,
 * y recorrerla es saltar de una a otra por el monton.  Y la firma no fija
 * ningun contenedor: quien llama construye el grafo en los suyos, con el
 * nombre que diga de quien son, y aqui solo se reciben punteros.
 *
 * @par Los tipos DICEN lo que son
 * Enums sin `class`, como @c ir::IrValueId y por lo mismo: un entero suelto no
 * dice si es un nodo, una componente o la posicion de una arista, y en el
 * simbolo de un contenedor sale como `unsigned int`, igual que otras decenas.
 * Convierten HACIA entero, asi que indexar se escribe igual; construir uno
 * desde un entero cualquiera pide el cast, que es donde conviene que se vea.
 */
#ifndef VESTA_UTIL_SCC_H
#define VESTA_UTIL_SCC_H

#include <cstddef>
#include <cstdint>

namespace util {

/// Un nodo del grafo: su posicion.
enum SccNode : uint32_t {};
/// La posicion de una arista dentro del array de destinos.
enum SccEdge : uint32_t {};
/// Una componente fuertemente conexa: su numero, en orden topologico inverso.
enum SccComp : uint32_t {};
/// Componente de un nodo que el recorrido todavia no ha cerrado.
static constexpr SccComp SCC_NO_COMP = SccComp(0xFFFFFFFFu);

/**
 * @brief Tarjan: las componentes fuertemente conexas de un grafo dirigido.
 *
 * Lineal en nodos y aristas, y ITERATIVO: un modulo grande tiene decenas de
 * miles de nodos, y una recursion por nodo desbordaria la pila del proceso.
 *
 * Las componentes se numeran en orden topologico INVERSO: una recibe un numero
 * MENOR que las de las que depende.  Recorrerlas por numero ascendente da
 * "dependencias primero", que es lo que quiere quien las pide.
 *
 * @param offsets  @p n + 1 posiciones dentro de @p targets.
 * @param targets  Los destinos de todas las aristas, nodo tras nodo.
 * @param n        Cuantos nodos hay.
 * @param out_comp Salida, @p n entradas: la componente de cada nodo.
 * @return Cuantas componentes hay.
 */
size_t tarjan_scc(const SccEdge *offsets, const SccNode *targets, size_t n,
                  SccComp *out_comp);

} // namespace util

#endif // VESTA_UTIL_SCC_H
