/*
 * VestaVM - Máquina Virtual Distribuida
 *
 * Copyright © 2026 David López.T (DesmonHak) (Castilla y León, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribución obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */
#ifndef PID_H
#define PID_H
#include <cstdint>

/**
 * PID compuesto
 */
struct GlobalPID {
    uint32_t scheduler_id; // ID del scheduler
    uint64_t local_pid;    // PID generado por ese scheduler

    bool operator==(const GlobalPID &other) const {
        return scheduler_id == other.scheduler_id &&
                local_pid == other.local_pid;
    }
};

namespace std {
    template<>
    struct hash<GlobalPID> {
        size_t operator()(const GlobalPID &p) const noexcept {
            return (static_cast<size_t>(p.scheduler_id) << 48) ^
                    std::hash<uint64_t>()(p.local_pid);
        }
    };
}


#endif //PID_H
