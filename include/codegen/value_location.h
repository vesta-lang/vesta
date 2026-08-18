/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file codegen/value_location.h
 * @brief @c ValueLocation: la ubicacion RESUELTA de un valor materializado,
 * como ABSTRACCION -- el consumidor (Rewrite) pregunta @c is_register() / @c
 * is_memory(), NUNCA lee un enum ni un campo a pelo.
 *
 * POR QUE: hoy una ubicacion es REG o STACK-slot.  Cuando crezca
 * (rematerializable, constante, home location, spill-cache, TLS, frame
 * temporal...) esos casos se anyaden como predicados AQUI y ningun consumidor
 * cambia -- el Rewrite ya preguntaba "¿es registro? ¿es memoria?", no "¿que
 * campo tiene la struct?".  Mantiene al Rewrite ignorante de COMO se representa
 * una ubicacion, igual que @c AllocationTimeline lo mantiene ignorante de la
 * representacion temporal (segments).
 *
 * ENCAPSULACION: el enum y los campos son PRIVADOS -- nadie puede @c loc.kind =
 * ... y romper el invariante.  Se construye con tags (@c Register / @c Stack);
 * ese es el UNICO modo de crear una ubicacion valida.  @c is_stack no dice
 * "spill" a proposito: la memoria podria ser home / remat / frame temporal.
 * (Evolucion futura probable: renombrar a
 * @c Storage -- responde "¿donde se ALMACENA el valor?", no "¿que tipo de sitio
 * es?".)
 */

#ifndef VESTA_CODEGEN_VALUE_LOCATION_H
#define VESTA_CODEGEN_VALUE_LOCATION_H

#include <cstdint>

namespace codegen {

/**
 * @class ValueLocation
 * @brief Donde vive un valor materializado, expuesto SOLO por predicados.
 */
class ValueLocation {
  public:
    /// Tags de construccion: el productor (el timeline) elige uno.  Un sitio
    /// nuevo (Constant, Home, Tls...) anyade un TAG, no un verbo en el
    /// namespace del tipo.
    struct Register {
        uint8_t id;
    };
    struct Stack {
        uint32_t slot;
    };

    constexpr ValueLocation() noexcept =
        default; ///< None (el valor no vive aqui).
    constexpr ValueLocation(Register r) noexcept
        : kind_(Kind::Register), reg_(r.id) {}
    constexpr ValueLocation(Stack s) noexcept
        : kind_(Kind::Stack), slot_(s.slot) {}

    constexpr bool is_none() const noexcept { return kind_ == Kind::None; }

    constexpr bool is_register() const noexcept {
        return kind_ == Kind::Register;
    }
    constexpr uint8_t register_id() const noexcept { return reg_; }

    /// ¿Vive en MEMORIA? (hoy = stack; manana tambien home / TLS / ...).  El
    /// consumidor que solo quiere saber "¿es memoria?" usa esto y NO enumera
    /// cada tipo de memoria.
    constexpr bool is_memory() const noexcept { return kind_ == Kind::Stack; }
    /// Sub-caso de memoria: stack.  NO "is_spill": la memoria podria ser
    /// home/remat/temp.
    constexpr bool is_stack() const noexcept { return kind_ == Kind::Stack; }
    constexpr uint32_t stack_slot() const noexcept { return slot_; }

    /// ¿Es la MISMA ubicacion?  Lo necesita quien detecta que un valor CAMBIA
    /// de sitio entre dos tramos (si no cambia, no hay nada que mover).
    constexpr bool operator==(const ValueLocation &o) const noexcept {
        return kind_ == o.kind_ && reg_ == o.reg_ && slot_ == o.slot_;
    }
    constexpr bool operator!=(const ValueLocation &o) const noexcept {
        return !(*this == o);
    }

  private:
    enum class Kind : uint8_t { None = 0, Register, Stack };
    Kind kind_ = Kind::None;
    uint8_t reg_ = 0;
    uint32_t slot_ = 0;
};

} // namespace codegen

#endif // VESTA_CODEGEN_VALUE_LOCATION_H
