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
 * @file asm_analyze.h
 * @brief Resumen de EFECTOS a nivel de BLOQUE de un cuerpo de inline asm.
 *
 * A partir del texto NASM/ARM de un @c asm { } se calcula un modelo de efecto
 * del bloque completo -- si toca memoria, si es atomico, si hace @c call, cuanto
 * marco de pila mueve EXPLICITAMENTE (push/pop/sub rsp) -- para que el compilador
 * sea CONSCIENTE de lo que hace el asm en vez de tratarlo como una caja negra.
 * Lo consume el analizador de huella (para dar @c @stack / @c @alloc /
 * @c @nothrow / @c @pure sobre una funcion con asm) y el servidor de lenguaje
 * (LSP): describir al IDE que hace un bloque de asm.
 *
 * Se apoya en la tabla PLANA por-instruccion @c asm_effects_for (asm_effects.h);
 * NO la duplica, solo la agrega a nivel de bloque.  Modular a proposito: este
 * modulo NO toca el path de inferencia de clobbers ni el ensamblado
 * (Keystone/Capstone); es analisis puro sobre el texto.
 *
 * Un mnemonico DESCONOCIDO para el arch NO se traga -- se acumula en
 * @c unknown_mnemonics para que el caller emita un ERROR CLARO nombrando cual.
 * La tabla de instrucciones reconocidas crece bajo demanda, no se modela todo
 * el juego de instrucciones de golpe.
 *
 * Limite conocido: el marco de pila IMPLICITO de los enlaces @c register()
 * (los spills que decide el asignador de registros del backend) NO es visible
 * en el texto del asm; aqui se cuenta solo el marco EXPLICITO (push/pop/sub
 * rsp/add rsp).  El implicito es una propiedad del codegen (JIT/AOT) y se
 * reportara desde el backend, no desde el texto.
 */

#ifndef VX_ASM_ANALYZE_H
#define VX_ASM_ANALYZE_H

#include <cstdint>
#include <string>
#include <vector>

namespace vx {

/**
 * @struct AsmBlockEffects
 * @brief Modelo de efecto AGREGADO de un cuerpo de inline asm.
 *
 * Todo lo que el analizador necesita para dar contratos sobre el bloque.  Los
 * campos de efecto son SOUND-conservadores: ante duda (mnemonico desconocido)
 * @c has_unknown queda true y el caller decide (error).
 */
struct AsmBlockEffects {
    bool touches_mem = false;   ///< algun operando @c [...] o instr que toca mem.
    /// El bloque LEE memoria.
    ///
    /// Se separa de @c writes_mem porque no es lo mismo: un `mov rax, [rdi]`
    /// solo lee, y decir que tambien escribe convierte el bloque en una barrera
    /// para todo lo que haya alrededor -- deja de poder moverse una escritura,
    /// de poder subir una lectura fuera de un bucle, de poder eliminar una
    /// escritura muerta.  La tabla ya dice QUE operandos escribe cada
    /// instruccion (@c AsmEffects::operand_write_mask), asi que la informacion
    /// estaba y se tiraba.
    ///
    /// Ante cualquier duda -- mnemonico desconocido, operandos no reconocibles,
    /// prefijo @c lock (lee y escribe) -- se marcan LAS DOS.
    bool reads_mem = false;
    /// El bloque ESCRIBE memoria.  Ver @c reads_mem.
    bool writes_mem = false;

    /**
     * @struct Acceso
     * @brief Un acceso a memoria del bloque: por que registro se llega y si se
     *        escribe.
     */
    /**
     * @struct Extension
     * @brief Hasta donde llega un acceso, como EXPRESION y no como numero.
     *
     * Un acceso casi nunca es "tantos bytes": es tantos bytes a tanta
     * distancia, quiza repetidos tantas veces, y esas cantidades salen de
     * operandos del propio bloque.  Decir "no se" porque una de ellas no es una
     * constante seria tirar lo que si se sabe -- que un `rep movsb` recorre
     * `[dst, dst + rcx)` es un hecho, aunque `rcx` no sea una constante --, y
     * quien pregunta SI puede acotarlo: el operando esta ligado a una variable
     * del programa, y de esa se conoce su rango.
     *
     * La expresion es:
     *
     *     [ base + const_off + indice*escala ,  + bytes * repeticion )
     *
     * donde @ref indice y @ref repeticion son NOMBRES de operandos (registro
     * canonico o marcador `$N`), no valores.  Resolverlos es cosa de quien
     * tiene las ligaduras y los rangos; describirlos, de aqui.
     */
    struct Extension {
        int64_t const_off = 0; ///< parte constante de la distancia, en bytes.
        /// Operando que aporta el resto de la distancia, o vacio si no lo hay.
        /// `[rbx + rcx*8]` -> @c "rcx" con @ref escala 8.
        std::string indice;
        int64_t escala = 1; ///< por cuanto se multiplica @ref indice.
        /// Bytes de UN acceso.  0 = no se pudo determinar cuantos.
        uint32_t bytes = 0;
        /// Operando que dice cuantas VECES se repite, o vacio si una sola.
        /// Un `rep movsb` lo repite `rcx` veces.
        std::string repeticion;

        /// @c true si la distancia es una constante (sin indice).
        bool distancia_constante() const { return indice.empty(); }
        /// @c true si el acceso es uno solo (sin repeticion).
        bool una_vez() const { return repeticion.empty(); }
        /// @c true si se conoce el tamano de un acceso.
        bool ancho_conocido() const { return bytes != 0; }
        /// @c true si la extension esta COMPLETAMENTE determinada aqui, sin
        /// necesidad de preguntar el rango de nada.
        bool cerrada() const {
            return ancho_conocido() && distancia_constante() && una_vez();
        }
    };

    /**
     * @struct Acceso
     * @brief Un acceso a memoria del bloque: por donde se llega, si escribe, y
     *        hasta donde llega.
     */
    /**
     * @struct Indireccion
     * @brief La base no es el operando, es lo que hay GUARDADO en el.
     *
     * `mov rdi, [rsi+8]` y luego `[rdi]` no deja a `rdi` sin explicacion: su
     * valor es el que estuviera en `[rsi+8]`.  Quien pregunta puede seguir esa
     * carga -- el bloque no esta solo, alrededor hay un programa que dice que
     * se guardo ahi --, asi que se describe en vez de rendirse.  Solo se queda
     * de verdad sin respuesta cuando el valor entra de fuera en ejecucion.
     */
    struct Indireccion {
        bool    hay = false; ///< la base se cargo de memoria.
        int64_t off = 0;     ///< de que distancia del operando se cargo.
    };

    struct Acceso {
        std::string base;    ///< registro base, CANONICO (vacio = no se supo).
        bool        escribe; ///< true si el acceso escribe.
        /// Si @c hay, @ref base no es la direccion: es donde estaba GUARDADA.
        Indireccion desde_memoria;
        /**
         * Hasta donde llega.  Ver @ref Extension.
         *
         * Sin esto, lo mas que se puede decir de un acceso es "toca ese
         * objeto", y entonces dos accesos al mismo objeto siempre parecen
         * estorbarse -- aunque uno vaya al principio y otro al final --.  Con
         * la extension, `mov [p], rax` y `mov [p+16], rax` son lo que de verdad
         * son: dos escrituras que no se pisan.
         *
         * @c valida en false es el unico "no se" que queda: ni siquiera se pudo
         * nombrar de que depende (una rama que deja la distancia en el aire,
         * una instruccion cuya memoria no esta acotada).  Entonces se toma el
         * objeto entero, que es lo que se hacia siempre.
         */
        Extension extension;
        bool      valida = false; ///< @ref extension describe algo.
    };
    /// Los accesos a memoria, en orden.  Sirven para decir QUE memoria toca el
    /// bloque en vez de "cualquiera": quien tenga las ligaduras puede llevar el
    /// registro base hasta el valor que contiene.
    std::vector<Acceso> accesos;
    /// Algun acceso no se pudo atribuir a un registro base (direccion absoluta,
    /// expresion rara, memoria implicita de la instruccion).  Con esto puesto,
    /// los @c accesos NO describen todo lo que el bloque toca y hay que
    /// suponer lo peor.
    bool accesos_incompletos = false;
    /// El bloque hace entrada/salida por PUERTO.  No es memoria, pero se ve
    /// desde fuera: ni se elimina ni se reordena, y quien lo haga no es codigo
    /// autonomo (deja de cumplir `freestanding`).
    bool has_port_io = false;
    /// Operandos que el bloque ESCRIBE: registros canonicos y marcadores `$N`.
    ///
    /// Un bloque puede cambiar el valor de una variable ligada SIN tocar
    /// memoria -- `inc rax` sobre un `register("rax") u64 v` --, y quien crea
    /// que ese valor sigue siendo el de antes se equivoca.  Sin esto, un
    /// programa que hace justo eso devolvia 41 en vez de 42.
    std::vector<std::string> escritos;
    bool has_atomic = false;    ///< prefijo @c lock o instr atomica (barrera).
    bool is_call = false;       ///< @c call / @c syscall alcanzable en el bloque.
    /// El bloque toca las banderas, en cualquier sentido.  Se mantiene porque es
    /// lo que pregunta quien solo quiere saber si son suyas o no; los dos
    /// sentidos por separado estan debajo.
    bool touches_flags = false;
    /**
     * @name Banderas por SENTIDO.
     *
     * Un bloque que solo las LEE (`setz al`) depende de la comparacion de antes
     * pero no destruye nada; uno que solo las ESCRIBE (`cmp`) destruye pero no
     * depende.  Con un bit unico los dos salen iguales y hay que suponer lo peor
     * de los dos, que es no mover nada ni a un lado ni al otro.
     * @{
     */
    bool reads_flags = false;  ///< consume banderas producidas fuera del bloque.
    bool writes_flags = false; ///< las modifica: quien las tuviera las pierde.
    /// @}
    bool has_branch = false;    ///< salto/rama dentro del bloque.
    int64_t explicit_stack_bytes = 0; ///< marco EXPLICITO (push/sub rsp), en bytes.
    std::vector<std::string> unknown_mnemonics; ///< desconocidos -> error claro.

    bool known() const { return unknown_mnemonics.empty(); }
};

/**
 * @brief Analiza un cuerpo NASM y devuelve su modelo de efecto de bloque.
 *
 * Tokeniza linea a linea (descarta comentarios @c ; y @c //, labels @c name:,
 * y separa el prefijo @c lock/@c rep/...), consulta @c asm_effects_for por
 * mnemonico y agrega los efectos.  El marco de pila explicito suma/resta segun
 * @c push/@c pop (8 bytes) y @c sub/@c add sobre @c rsp con inmediato literal.
 *
 * @param nasm_body Cuerpo verbatim del bloque @c asm.
 * @param arch      Arquitectura de destino (@c "x86_64" / @c "arm64" / ...);
 *                  selecciona la tabla de efectos.  Un mnemonico no tabulado
 *                  para ese arch cae en @c unknown_mnemonics.
 * @return Efectos agregados, SIN la extension de los accesos.
 *
 * @warning Se llama asi para que elegirla sea una decision con nombre y no un
 * descuido.  Sin las clases de operando no se puede saber cuantos bytes toca un
 * `$N`, con lo que cada acceso queda sin ancho y vale "el objeto entero" -- que
 * es justo lo que NUNCA puede violar un limite.  Eso ya paso: el analisis de
 * efectos llamaba aqui, un `asm` que escribia fuera del buffer del llamante no
 * levantaba un solo aviso, y lo que hay detras del buffer es la siguiente
 * variable.  Usala unicamente si lo que preguntas no depende de que memoria se
 * toca; si depende, pasa las clases.
 */
AsmBlockEffects asm_analyze_block_sin_clases(const std::string &nasm_body,
                                            const std::string &arch);

/**
 * @brief El analisis COMPLETO: sabiendo de que CLASE es cada operando.
 *
 * El ancho de un acceso lo dice el operando que NO son los corchetes, y cuando
 * ese operando lo eligio el compilador se llama `$N` en el cuerpo: entonces su
 * ancho solo lo sabe la clase con la que se declaro.  Sin el mapa, un
 * `movdqa [$0], $1` no puede decir que toca dieciseis bytes.
 *
 * @param nasm_body Cuerpo NASM/ARM.
 * @param arch      Arquitectura del cuerpo.
 * @param clases_operando Pares (marcador en el cuerpo, clase declarada).
 * @return Los mismos efectos, con la extension de cada acceso resuelta hasta
 *         donde el bloque permita.
 */
AsmBlockEffects asm_analyze_block(
    const std::string &nasm_body, const std::string &arch,
    const std::vector<std::pair<std::string, std::string>> &clases_operando);

} // namespace vx

#endif // VX_ASM_ANALYZE_H
