/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vxdbg_emit.h
 * @brief Traduce lo que sabe el frontend de Vesta a nodos de depuracion.
 *
 * El subsistema @c vxdbg no conoce Vesta -- ni ningun otro lenguaje -- a
 * proposito: guarda entidades con un @c kind que decide quien las declara.  La
 * correspondencia entre el vocabulario de Vesta (clase, struct, enum, campo,
 * metodo, variante) y ese modelo generico vive AQUI, en el frontend, que es
 * quien la conoce.  Un frontend de otro lenguaje escribiria su propio traductor
 * contra las mismas estructuras sin tocar nada de @c vxdbg.
 *
 * Solo cubre la capa SEMANTICA: que tipos hay, que miembros tienen y como se
 * relacionan.  Las sentencias, la bajada al intermedio y la correspondencia con
 * el codigo generado van en incrementos aparte, cada uno con su capa.
 */

#ifndef VX_VXDBG_EMIT_H
#define VX_VXDBG_EMIT_H

#include "vxdbg/ids.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace vx {

class TypeChecker;

/**
 * @brief Cuenta de lo emitido, para poder informar sin adivinar.
 */
struct VxdbgEmitStats {
    size_t entities = 0;   ///< entidades escritas (tipos y miembros)
    size_t members = 0;    ///< de las anteriores, campos/metodos/variantes
    /// Nombres referenciados que no corresponden a ningun tipo conocido.  La
    /// relacion se omite en vez de inventarse un destino: preferimos no decir
    /// nada a decir algo falso.  Que este contador no sea cero es informacion
    /// util, no necesariamente un fallo.
    size_t unresolved = 0;
    /// Los tipos emitidos, por su clave.  Un almacen direccionado por contenido
    /// no se puede recorrer: no hay listado, solo se llega a un nodo si ya se
    /// sabe su huella.  Quien acaba de emitir es el unico que las conoce, y
    /// esta es la lista que despues acaba en la unidad de compilacion como sus
    /// raices.
    std::vector<std::pair<std::string, vxdbg::LanguageEntityId>> roots;
};

/**
 * @brief Carpeta por defecto del almacen: dentro del cache del compilador.
 *
 * Va con el resto de artefactos intermedios porque es uno mas: se regenera al
 * recompilar y se borra con ellos.  Sacarlo aparte obligaria a acordarse de
 * limpiarlo, y un grafo de una version anterior mezclado con el actual es peor
 * que no tener ninguno.
 *
 * @return La ruta, respetando @c VX_CACHE_DIR si esta puesta.
 */
std::string default_vxdbg_dir();

/**
 * @brief Vuelca la capa semantica del modulo al almacen de depuracion.
 *
 * Toma los tipos del @ref TypeChecker y no del AST porque la tabla del checker
 * incluye tambien los IMPORTADOS: emitir desde el AST dejaria sin destino toda
 * relacion que apunte fuera del fichero, que es justo lo que hace util saber de
 * quien deriva algo.
 *
 * @param tc Checker ya ejecutado.
 * @param source_path Ruta del fuente principal.
 * @param source_text Su contenido, para resumirlo y poder detectar despues que
 *        el fichero de disco ya no es el que se compilo.  Vacio = sin resumen.
 * @param out_dir Carpeta del almacen; se crea si no existe.
 * @param stats Recibe la cuenta de lo emitido.
 * @param err Recibe el motivo si algo fallo.
 * @return @c true si se escribio todo.
 */
bool emit_vxdbg_source(const TypeChecker &tc, const std::string &source_path,
                       const std::string &source_text,
                       const std::string &out_dir, VxdbgEmitStats &stats,
                       std::string &err);

} // namespace vx

#endif // VX_VXDBG_EMIT_H
