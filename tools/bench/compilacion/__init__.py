#!/usr/bin/env python3
"""Banco de tiempos de COMPILACION, por modulos.

    comun         colores, compilador elegido, utilidades del arnes comun
    generadores   el mismo programa en cada lenguaje, del tamano que se pida
    multi         ese programa repartido en varios ficheros
    familias      que codigo se compila (genericos, comptime, tipos, anidado)
    topologia     forma de las dependencias, mutaciones y artefactos rehechos
    ordenes       como se invoca a cada compilador y donde tiene su cache
    medida        como se cronometra, y como se comprueba que compila de verdad
    informe       tablas
    contexto      lo que comparten las fases
    fases/        una fase por modulo, mas su registro

El punto de entrada es `tools/bench/compile_bench.py`.
"""
