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

/**                                                                            \
 * @file pid.h                                                                 \
 * @brief Tipo e identificador de proceso virtual de VestaVM.                  \
 *                                                                             \
 * Define @c pid_t como alias de @c uint32_t y la constante                    \
 * @c INVALID_PID = 0 para representar un identificador de proceso invalido.   \
 */                                                                            \
#ifndef PID_H
#define PID_H

#include <cstdint>

/**
 * @brief Identificador global de proceso compuesto.
 *
 * Combina el ID del scheduler propietario con el PID local asignado
 * por ese scheduler.  El par (scheduler_id, local_pid) es unico dentro
 * de una instancia VM.
 *
 * La especializacion de std::hash permite usar GlobalPID como clave de
 * std::unordered_map en los indices de procesos del scheduler.
 */
struct GlobalPID {
    uint32_t scheduler_id; ///< ID del scheduler que creo y posee este proceso
    uint64_t local_pid;    ///< PID local generado por ese scheduler
                           ///< (monotonicamente creciente)

    /**
     * @brief Operador de igualdad: dos PIDs son iguales si ambos campos
     * coinciden.
     * @param other PID con el que comparar.
     * @return true si scheduler_id y local_pid son iguales en ambos objetos.
     */
    bool operator==(const GlobalPID &other) const {
        return scheduler_id == other.scheduler_id && // mismo scheduler
               local_pid == other.local_pid;         // mismo PID local
    }
};

namespace std {
/**
 * @brief Especializacion de std::hash para GlobalPID.
 *
 * Combina los dos campos en un unico hash de tipo size_t desplazando
 * scheduler_id a los bits altos y haciendo XOR con el hash de local_pid.
 * Esto reduce las colisiones cuando se usan muchos PIDs del mismo scheduler.
 */
template <> struct hash<GlobalPID> {
    /**
     * @brief Calcula el hash del GlobalPID indicado.
     * @param p PID a hashear.
     * @return  Valor hash de tipo size_t.
     */
    size_t operator()(const GlobalPID &p) const noexcept {
        // desplazar scheduler_id a los bits 48..63 y combinar con el hash de
        // local_pid
        return (static_cast<size_t>(p.scheduler_id) << 48) ^
               std::hash<uint64_t>()(p.local_pid);
    }
};
} // namespace std

#endif // PID_H
