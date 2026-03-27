/**
 * VestaVM - Máquina Virtual Distribuida
 *
 * Copyright © 2026 David López.T (DesmonHak) (Castilla y León, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribución obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 *
 * @file ast.h
 * @brief Árboles de Sintaxis Abstracta (AST) para el lenguaje VMProject.
 *
 * Define la estructura jerárquica del AST que representa el código parseado.
 * Cada nodo hereda de `ASTNode` base y representa una construcción sintáctica
 * específica del lenguaje (etiquetas, asignaciones de registros, declaraciones de datos).
 *
 * Copyright (c) 2026 David López T.
 * Proyecto VMProject - Licencia MIT
 */

#ifndef TIMER_H
#define TIMER_H

#include <chrono>

/**
 * Timer de alta resolución para medir tiempos de ejecución.
 *
 * Unidades disponibles:
 *
 *  - ms  (milliseconds)  -> milisegundos
 *      1 ms = 1.000 microsegundos
 *      Resolución típica: útil para medir tareas "grandes" (lectura de archivos, parseo grande, etc.)
 *
 *  - us  (microseconds)  -> microsegundos
 *      1 us = 1.000 nanosegundos
 *      Resolución media: ideal para medir funciones rápidas, fases del parser, ensamblador, etc.
 *
 *  - ns  (nanoseconds)   -> nanosegundos
 *      1 ns = 1.000.000.000 por segundo
 *      Resolución muy fina: útil para medir operaciones extremadamente rápidas o micro-optimizaciones.
 *
 * Nota:
 *  - ms puede mostrar 0 si la operación tarda menos de 1 milisegundo.
 *  - us es más preciso y suele mostrar valores reales en la mayoría de fases.
 *  - ns es el nivel más detallado, pero puede ser ruidoso dependiendo del sistema.
 *
 *
 * Unidades:
 *  - ms -> milisegundos (1 ms = 1.000 us)
 *  - us -> microsegundos (1 us = 1.000 ns)
 *  - ns -> nanosegundos (1 ns = 1/1.000.000.000 s)
 *
 * steady_clock garantiza mediciones estables y monotónicas.
 */
typedef struct Timer {
    std::chrono::steady_clock::time_point start;

    Timer() {
        reset();
    }

    void reset() {
        start = std::chrono::steady_clock::now();
    }
    long long ms() const {
        auto end = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    }

    long long us() const {
        auto end = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    }

    long long ns() const {
        auto end = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    }
} Timer;


#endif //TIMER_H
