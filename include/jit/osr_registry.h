/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file jit/osr_registry.h
 * @brief Registro de los bucles preparados para RE-ENTRAR a mitad de vuelta.
 *
 * Cuando un bucle lleva muchas iteraciones no compensa esperar a que la funcion
 * vuelva a llamarse para darle una version mejor: hay que entrar en la nueva
 * MIENTRAS el bucle sigue girando.  Para eso, la version en curso cuenta sus
 * vueltas y, al pasar del umbral, llama a un disparador con el estado del
 * bucle; quien atiende decide si hay una version mejor a la que saltar.
 *
 * Aqui vive el LADO PERSISTENTE de eso -- que bucles hay, con que estado, y a
 * quien se le pregunta --, que es lo unico que sobrevive a la compilacion.
 * Emitir el contador y la captura corresponde a quien genera el codigo, porque
 * necesita el marco de pila; pero el registro no tenia por que vivir dentro
 * del reescritor de registros, que es donde estaba: son dos asuntos distintos,
 * y se notaba en que un consumidor tenia que incluir la cabecera del
 * reescritor para pedir algo que no tiene nada que ver con el.
 */

#ifndef VESTA_JIT_OSR_REGISTRY_H
#define VESTA_JIT_OSR_REGISTRY_H

#include <cstdint>
#include <string>
#include <vector>

namespace jit {
namespace osr {

/**
 * @struct Captura
 * @brief Una celda del estado que se traslada: el valor del IR y si es raiz
 *        del recolector.  El valor viaja en @c osr_buffer[vid].
 */
struct Captura {
    uint32_t vid;    ///< identificador del valor en el IR (indice del buffer).
    uint8_t  es_gc;  ///< 1 si apunta a un objeto del recolector.
};

/**
 * @brief ¿Se instrumentan los bucles con su contador de vueltas?
 *
 * `VESTA_OSR_COUNT=1` lo activa.  Es el prerrequisito de todo lo demas: sin
 * contador no hay umbral que cruzar.
 *
 * @return true si esta activado.
 */
bool contador_activado() noexcept;

/**
 * @brief Vueltas que tiene que dar un bucle para que se dispare el cambio.
 *
 * Por defecto 100000; `VESTA_OSR_THRESHOLD` lo cambia.
 *
 * @return El umbral.
 */
uint32_t umbral() noexcept;

/**
 * @brief Apunta un bucle instrumentado y le da su identificador.
 *
 * @param fn_name      Nombre de la funcion que lo contiene.
 * @param header_block Bloque por el que se entra al bucle.
 * @param capturas     Estado vivo a la entrada del bucle.
 * @param abortado     true si no se pudo capturar el estado; entonces el bucle
 *                     queda apuntado pero nadie podra saltar a el.
 * @return El identificador del bucle.
 */
uint64_t registrar_bucle(std::string fn_name, uint32_t header_block,
                         std::vector<Captura> capturas, bool abortado);

/**
 * @brief Direccion del contador global de vueltas, que el codigo emitido
 *        incrementa directamente.
 *
 * @return Puntero al contador.
 */
uint64_t *contador_de_vueltas() noexcept;

/**
 * @brief Direccion del disparador, al que el codigo emitido llama al cruzar el
 *        umbral.  Convencion C: (proc, id_bucle, buffer) -> direccion a la que
 *        saltar, o 0 para seguir como estaba.
 *
 * @return Puntero a la funcion.
 */
void *disparador() noexcept;

/**
 * @brief Deja registrado (una sola vez) el resumen que se imprime al terminar
 *        el programa: cuantos bucles, cuantas vueltas y cuantos disparos.
 */
void instalar_resumen_al_salir();

} // namespace osr

/* ===================================================================== */
/* Lo que consume quien decide a que version se salta                    */
/* ===================================================================== */

/**
 * @brief Instala quien atiende el disparo.  Recibe el identificador del bucle
 *        y devuelve la DIRECCION por la que entrar en la version mejor, o 0 si
 *        no hay ninguna y el bucle debe seguir como estaba.
 */
void set_osr_handler(uint64_t (*handler)(uint64_t loop_id));

/**
 * @brief Cuantos bucles hay instrumentados hasta ahora (== identificadores
 *        repartidos).  Sirve para recorrerlos y preparar sus versiones.
 */
uint32_t osr_loop_count();

/**
 * @brief Datos de un bucle: nombre de su funcion y bloque de entrada.
 *
 * @param loop_id          Identificador del bucle.
 * @param fn_name_out      Recibe el nombre de la funcion.
 * @param header_block_out Recibe el bloque de entrada.
 * @return false si el identificador no existe o el bucle se aborto.
 */
bool osr_loop_info(uint64_t loop_id, std::string &fn_name_out,
                   uint32_t &header_block_out);

/**
 * @brief Valores que se capturaron al buffer para @p loop_id.
 *
 * Es la red de seguridad de la version nueva: solo puede leer valores que
 * esten en esta lista.
 *
 * @param loop_id  Identificador del bucle.
 * @param out_vids Recibe los identificadores de valor.
 * @return false si el identificador no existe o el bucle se aborto.
 */
bool osr_loop_captures(uint64_t loop_id, std::vector<uint32_t> &out_vids);

} // namespace jit

#endif // VESTA_JIT_OSR_REGISTRY_H
