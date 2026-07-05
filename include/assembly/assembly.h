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
 * @file assembly.h
 * @brief Interfaz de ensamblado y desensamblado de codigo nativo usando
 * Keystone y Capstone.
 *
 * Proporciona funciones para:
 *   - Consultar las arquitecturas soportadas por Keystone (ensamblador) y
 * Capstone (desensamblador).
 *   - Ensamblar un fichero de texto con instrucciones nativas a binario.
 *   - Desensamblar un fichero binario a texto legible.
 */

#ifndef ASSEMBLY_H
#define ASSEMBLY_H

#include <capstone/capstone.h>
#include <keystone/keystone.h>
#include <vector>
#include <string>
#include <iostream>
#include <algorithm>
#include <iomanip>
#include <fstream>

/**
 * @brief Contiene los nombres de arquitecturas soportadas por Capstone y
 * Keystone.
 *
 * Cada vector almacena los identificadores de arquitectura en forma de cadena
 * tal como se esperan en los argumentos de linea de comandos (p.ej. "X86-64",
 * "ARM").
 */
struct ArchSupport {
    std::vector<std::string>
        capstone; ///< Arquitecturas disponibles en Capstone (desensamblador)
    std::vector<std::string>
        keystone; ///< Arquitecturas disponibles en Keystone (ensamblador)
};

/**
 * @brief Compatibilidad: define _CS_ARCH_ARM64 usando el valor correcto de
 * Capstone si esta disponible.
 *
 * Si CS_ARCH_ARM64 no esta definido por la version de Capstone enlazada se
 * asigna 1 como valor de reserva para evitar errores de compilacion.
 */
#ifndef CS_ARCH_ARM64
#define _CS_ARCH_ARM64 1
#else
#define _CS_ARCH_ARM64 CS_ARCH_ARM64
#endif

/**
 * @brief Devuelve las arquitecturas disponibles en la instalacion actual de
 * Capstone y Keystone.
 *
 * La estructura devuelta se construye una sola vez (estado estatico interno) y
 * se reutiliza en llamadas sucesivas.  Consulta las funciones de deteccion de
 * arquitectura de ambas bibliotecas para filtrar unicamente las que estan
 * realmente disponibles.
 *
 * @return Referencia constante a la estructura ArchSupport con los nombres de
 * arquitectura.
 */
const ArchSupport &get_available_architectures();

/**
 * @brief Ensambla el fichero de codigo fuente nativo indicado y opcionalmente
 * guarda el binario.
 *
 * Lee el fichero de texto en @p asmPath, invoca Keystone para la arquitectura
 * @p arch y produce el binario resultante.  Si @p save_output es true el
 * binario se escribe en un fichero con el prefijo @p prefix; en caso contrario
 * se vuelca por stdout (modo diagnostico).
 *
 * @param asmPath    Ruta al fichero de texto con instrucciones nativas.
 * @param arch       Nombre de la arquitectura destino (p.ej. "X86-64", "ARM",
 * "AArch64").
 * @param save_output Si true guarda el resultado en disco; si false lo imprime
 * por stdout.
 * @param prefix     Prefijo de nombre de fichero de salida (solo usado cuando
 * save_output es true).
 * @return true si el ensamblado fue exitoso; false si hubo algun error.
 */
bool assemble_file(const std::string &asmPath, const std::string &arch,
                   bool save_output, const std::string &prefix);

/**
 * @brief Desensambla el fichero binario indicado y opcionalmente guarda el
 * texto resultante.
 *
 * Lee el binario en @p binPath, invoca Capstone para la arquitectura @p arch y
 * produce el listado de instrucciones.  Si @p save_output es true el texto se
 * escribe en un fichero con el prefijo @p prefix; si no se vuelca por stdout.
 *
 * @param binPath    Ruta al fichero binario a desensamblar.
 * @param arch       Nombre de la arquitectura fuente (p.ej. "X86-64", "ARM",
 * "AArch64").
 * @param save_output Si true guarda el resultado en disco; si false lo imprime
 * por stdout.
 * @param prefix     Prefijo de nombre de fichero de salida (solo usado cuando
 * save_output es true).
 * @return true si el desensamblado fue exitoso; false si hubo algun error.
 */
bool disassemble_file(const std::string &binPath, const std::string &arch,
                      bool save_output, const std::string &prefix);

#endif // ASSEMBLY_H
