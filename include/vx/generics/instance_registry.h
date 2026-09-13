/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file vx/generics/instance_registry.h
 * @brief Quien se queda con cada instanciacion generica, para que se haga UNA
 *        vez en todo el proyecto.
 *
 * @par El problema
 * Cada modulo tiene su propio @c TypeChecker y su propio mapa de
 * deduplicacion, asi que `procesa<S0>` se CLONA, se COMPRUEBA y se BAJA A IR
 * una vez por cada modulo que la usa.  La fusion las deduplica por nombre al
 * final -- el propio codigo lo llama "modelo COMDAT de C++" y dice que las
 * copias son IDENTICAS por construccion --, o sea que **el trabajo se hace N
 * veces para quedarse con uno**.
 *
 * Medido sobre dieciseis modulos que instancian los mismos cien tipos: el
 * mismo programa final cuesta **2,3 veces la memoria** (212 MiB contra 92).
 * De ese sobrecoste, 43 MiB son de los clones y el resto lo pone compilar los
 * modulos a la vez.
 *
 * @par Lo que hace esto
 * Reparte cada instanciacion: el primer modulo que la pide se la queda y es
 * quien clona el cuerpo y la baja; los demas se limitan a registrar su FIRMA,
 * que es lo unico que necesitan para comprobar las llamadas.  El nombre
 * mangleado es el mismo, asi que el enlazado las une.
 *
 * @par Por que un cerrojo y no algo mas fino
 * Porque no esta en un camino caliente: se toma una vez por INSTANCIACION --
 * miles en un proyecto grande --, no por nodo ni por llamada.  Lo que si
 * importa es que el cerrojo NO envuelva el trabajo: se reclama, se suelta, y
 * clonar y bajar ocurren fuera.  El cerrojo del gestor de analisis ya es el
 * cuello de esta compilacion y no conviene anadir otro.
 *
 * @par Y la carrera es benigna
 * Dos hilos que piden la misma instancia a la vez producirian lo MISMO -- es
 * la afirmacion que la fusion ya hace hoy para deduplicarlas --, asi que basta
 * con que gane uno.
 */
#ifndef VESTA_VX_GENERICS_INSTANCE_REGISTRY_H
#define VESTA_VX_GENERICS_INSTANCE_REGISTRY_H

#include <mutex>
#include <string>
#include <unordered_set>

namespace vx {

/**
 * @brief Reparto de instanciaciones genericas entre los modulos de UN
 *        proyecto.
 *
 * Vive en la compilacion del proyecto y se le pasa a cada @c TypeChecker.  No
 * es global al proceso a proposito: `libvesta` deja compilar varios proyectos
 * en el mismo proceso, y un registro global los mezclaria.
 */
class GenericInstanceRegistry {
  public:
    /**
     * @brief Pide quedarse con la instanciacion @p mangled.
     *
     * @param mangled Nombre ya mangleado de la instancia (`procesa_S0`), que
     *        es lo que la identifica: mismo template y mismos argumentos dan
     *        el mismo nombre.
     * @return @c true si este es el PRIMERO que la pide, y por tanto quien
     *         debe clonar el cuerpo y bajarla.  @c false si ya se la quedo
     *         otro: entonces basta con registrar la firma.
     */
    bool claim(const std::string &mangled) {
        std::lock_guard<std::mutex> lk(m_);
        return claimed_.insert(mangled).second;
    }

    /// Cuantas se repartieron.  Para poder decir que el reparto funciono.
    size_t size() const {
        std::lock_guard<std::mutex> lk(m_);
        return claimed_.size();
    }

  private:
    mutable std::mutex m_;
    std::unordered_set<std::string> claimed_;
};

} // namespace vx

#endif // VESTA_VX_GENERICS_INSTANCE_REGISTRY_H
