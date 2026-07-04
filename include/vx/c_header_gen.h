/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file c_header_gen.h
 * @brief Generador del header C publico (`vex --emit-header`, Fase 4 interop C).
 *
 * Emite un `.h` C99 con la interfaz publica del modulo Vesta: typedefs de los
 * structs C-representables (named, layout C garantizado), prototipos de las
 * funciones cuya firma es C-representable, y punteros a funcion (`cfn`) inline.
 *
 * El header se genera a nivel FRONTEND (TypeChecker + AST), no del IR: es la
 * API Vesta tal cual la ve un programador C.  Es ABI-compatible con el `.c` del
 * port-C (los structs cruzan POR PUNTERO -- la ABI de agregados del port-C --
 * y un puntero a struct == void* a nivel ABI).
 *
 * Las funciones con tipos GESTIONADOS en la firma (string/clase/fn capturador/
 * unique/...) se OMITEN del header con un comentario: son internas, no cruzan
 * la frontera C por valor.
 */

#ifndef VEX_C_HEADER_GEN_H
#define VEX_C_HEADER_GEN_H

#include <string>

namespace vx {

namespace ast {
struct ModuleNode;
}
class TypeChecker;

/**
 * @brief Genera el texto del header C publico del modulo.
 * @param mod        AST del modulo (para nombres de funcion + params).
 * @param tc         TypeChecker ya corrido (firmas + categorias de struct).
 * @param guard_base nombre base para el include guard / prefijo (p.ej. el
 *                   nombre del modulo o del fichero de salida).
 * @return el contenido completo del `.h` (con include guard + stdint).
 */
std::string generate_c_header(const ast::ModuleNode &mod, const TypeChecker &tc,
                              const std::string &guard_base);

} // namespace vx

#endif // VEX_C_HEADER_GEN_H
