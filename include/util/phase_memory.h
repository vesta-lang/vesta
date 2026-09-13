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
 * @file util/phase_memory.h
 * @brief La frontera de fase: lo que la anterior solto, de vuelta al reparto.
 */

#ifndef VESTA_UTIL_PHASE_MEMORY_H
#define VESTA_UTIL_PHASE_MEMORY_H

namespace util {

/**
 * @brief Devuelve al reparto comun lo que la fase que termina dejo de usar.
 *
 * POR QUE SE PIDE Y NO PASA SOLO.  El asignador no sabe que una fase ha
 * terminado con su conjunto de trabajo; el unico que lo sabe es quien la
 * cierra.  Y el barrido no es gratis -- recorre los trozos --, asi que hacerlo
 * por debajo y de forma invisible seria cobrarle a un bucle caliente lo que
 * solo hace falta cuatro veces en una compilacion.
 *
 * SON TRES COSAS Y EL ORDEN IMPORTA, que es la razon de que tengan un nombre
 * solo y no se escriban sueltas en cada sitio:
 *
 *   1. los tramos aparcados vuelven al reparto -- una fase que suelta mucho de
 *      un tamano que la siguiente no pide los dejaba fuera de circulacion;
 *   2. los trozos que se quedaron sin un solo bloque vivo dejan de pertenecer
 *      a su clase de tamano.  Es la mitad mayor: un trozo entregado a una
 *      clase no vuelve nunca por su cuenta, asi que una fase que llena 400 MiB
 *      de un tamano y lo suelta deja esos trozos siendo de una clase que la
 *      fase siguiente puede no pedir;
 *   3. y las paginas, al sistema.  Las dos de arriba hacen reutilizable la
 *      memoria DENTRO del asignador; esta es la unica que hace bajar lo que ve
 *      el sistema operativo, y por eso va la ultima: suelta lo que las otras
 *      acaban de dejar libre.
 *
 * Escritas sueltas se escribian en distinto orden y con distinta indentacion
 * en cada sitio, y anadir una cuarta frontera era copiar el bloque otra vez --
 * que es como una de las tres se queda fuera sin que nadie lo note.
 *
 * @param next_mark  Nombre de la fase que EMPIEZA aqui, para el eje del tiempo
 *                   del comprobador de memoria, o `nullptr` si la frontera no
 *                   abre ninguna.  Va una CLAVE, no una frase: el texto de cara
 *                   al usuario sale del catalogo multi-idioma, y este acaba
 *                   dentro del informe de OTRO proyecto, que no tiene por que
 *                   llevar nuestras palabras ni nuestro idioma.
 *
 * No cuesta nada sin comprobador: fuera de ese build la marca es un cuerpo
 * vacio en linea.
 */
void release_between_phases(const char *next_mark = nullptr);

/**
 * @par Por que clase se barre, y por que no es la que trae el asignador
 * El asignador empieza por la 8 y dice que quien tenga otro perfil de tamanos
 * pase el suyo; el del compilador es mas pequeno.  El numero, la tabla de
 * medidas y la advertencia de que CADUCA si cambia como reserva el compilador
 * estan en @c kCompilerReclaimFrom, en el `.cpp`.
 *
 * @par Y por que no hay una variante "a fondo" para el sitio del pico
 * Se probo barrer hasta la clase mas pequena SOLO en la frontera de antes de
 * montar la imagen del ejecutable, que es donde la curva senala el salto mas
 * grande.  No sirve: 625,7 MiB contra 621,7 sin ella, o sea nada, y +8% de
 * tiempo.  Lo que baja el pico es barrer PRONTO -- antes de que las paginas se
 * cojan --, no barrer fuerte al final.
 */

} // namespace util

#endif // VESTA_UTIL_PHASE_MEMORY_H
