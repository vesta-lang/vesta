/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 *
 * Software libre bajo GPLv2.  La salida del compilador (programas
 * escritos en Vesta) NO queda sujeta a la GPL (excepcion de runtime).
 */

/**
 * @file vx/generics/generic_infer.h
 * @brief Deducir los argumentos de tipo de una generica a partir de los suyos.
 *
 * @par Que resuelve
 * Escribir `id(5)` en vez de `id<i64>(5)`: el compilador tiene que averiguar
 * que `T` es `i64` mirando los argumentos.  Y lo mismo desde la otra grafia,
 * `5.id()`, que es la misma llamada escrita del otro modo.
 *
 * @par Por que no vale comparar nombres
 * La forma barata -- "busca un parametro cuyo tipo declarado SEA la letra `T`"
 * -- solo funciona cuando la variable aparece DESNUDA.  En cuanto algo la
 * envuelve deja de ver nada:
 *
 * | declaracion | deduce |
 * | :-- | :-- |
 * | `T f<T>(T x)` | si |
 * | `T f<T>(Caja<T> c)` | no: el nodo es `Caja` |
 * | `T f<T>(T* p)` | no: es un puntero |
 * | `R f<T,R>(T x, fn(T) -> R cb)` | no: `R` no esta desnudo en ningun sitio |
 *
 * Y ese ultimo es el caso que importa de verdad, porque es la forma de las
 * funciones que hacen comodo el codigo generico: en
 * `Lista<R> map<T,R>(Lista<T> xs, fn(T) -> R f)` NI UNA variable aparece
 * desnuda.  Sin deduccion estructural no existe `xs.map(f)`.
 *
 * @par El trabajo se hace al DECLARAR, no al llamar
 * La firma de una plantilla no cambia, asi que recorrerla en cada llamada es
 * repetir la misma cuenta.  Se recorre UNA vez y se guarda un PLAN.
 *
 * Es la misma decision que gobierna el indice de la llamada uniforme -- la
 * correlacion se hace al declarar -- y la regla de la casa sobre el
 * conocimiento del programa: un hecho se produce UNA vez y se guarda; ningun
 * consumidor vuelve a descubrirlo por su cuenta.
 *
 * @par Lo que el plan ahorra de verdad
 * No es recorrer el tipo, que son dos o tres nodos: es **COMPROBAR los
 * argumentos**.  Tipar una expresion es recorrerla entera, y ademas el camino
 * normal de la llamada la va a comprobar otra vez -- con lo que cualquier
 * argumento tipado de mas se paga DOS veces, y anidado se multiplica --.  El
 * plan dice exactamente cuales hacen falta, asi que los demas ni se tocan.
 *
 * Por eso el plan guarda QUE ARGUMENTOS hay que comprobar y no el camino
 * completo hasta cada variable: el camino ahorraria recorrer dos nodos, que no
 * es donde esta el coste, a cambio de mucho mas codigo donde equivocarse.
 *
 * @par Por que mascaras de bits y no tablas por nombre
 * Las variables de tipo de una plantilla son UNA, dos o tres.  Un
 * @c unordered_map por nombre para eso cuesta mas en construirse que lo que
 * ahorra: hashear cadenas cortas, reservar nodos y perseguir punteros para
 * responder lo que un indice contesta con un desplazamiento.
 *
 * Asi que las variables se identifican por su POSICION en la declaracion, las
 * ligaduras viven en un array paralelo y "cual esta ligada" es una mascara.
 * Nada de esto reserva memoria, y el conjunto entero cabe en un registro.
 */

#ifndef VX_GENERICS_GENERIC_INFER_H
#define VX_GENERICS_GENERIC_INFER_H

#include "util/alloc/small_vector.h"
#include "vx/ast.h"
#include "vx/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vx {

class TypeChecker;

namespace generics {

/// Cuantas variables de tipo caben en la mascara.  Una plantilla declara una o
/// dos; pasar de aqui no se planifica y hay que escribir los type-args, que es
/// ruidoso a proposito -- lo contrario seria deducir de menos en silencio.
constexpr uint16_t kMaxTypeParams = 32;

/// @brief La mascara con @p n bits bajos a uno, sin desbordar el
/// desplazamiento.
inline uint32_t full_type_param_mask(uint16_t n) noexcept {
    return (n >= 32) ? ~0u : ((1u << n) - 1u);
}

/**
 * @struct DeductionPlan
 * @brief De donde salen las variables de tipo de una plantilla.  Una vez.
 */
struct DeductionPlan {
    /// Los indices de los parametros en cuyo tipo aparece alguna variable, en
    /// orden y sin repetir: los UNICOS argumentos que hay que comprobar.
    util::SmallVector<uint16_t, 4> needed_args;

    /// Bit @c i encendido = la variable @c i sale de algun parametro.  Las
    /// apagadas solo se pueden dar escribiendolas.
    uint32_t deducible = 0;

    /// Cuantas variables declara la plantilla.
    uint16_t count = 0;

    /// @return Si TODAS se pueden deducir de los argumentos.
    bool complete() const noexcept {
        return count <= kMaxTypeParams &&
               deducible == full_type_param_mask(count);
    }
};

/**
 * @brief Recorre la firma UNA vez y apunta de donde sale cada variable.
 *
 * @param type_params Los nombres declarados entre `<...>`, en orden.
 * @param param_types El tipo DECLARADO de cada parametro, en orden.  Un nulo
 *                    se salta (parametro sin tipo escrito).
 * @return El plan.
 */
DeductionPlan
build_deduction_plan(const std::vector<std::string> &type_params,
                     const std::vector<const ast::TypeNode *> &param_types);

/**
 * @brief El tipo TAL Y COMO SE ESCRIBIO, en texto.
 *
 * Para poder ensenyar en un diagnostico un tipo que todavia no existe: el
 * `Caja<T>` de una plantilla no resuelve a nada, pero es lo que el usuario
 * tiene delante, y es lo que hay que citarle.
 *
 * @param t El tipo declarado.
 * @return Su texto, o vacio si el nodo no se sabe escribir.
 */
std::string type_node_text(const ast::TypeNode *t);

/**
 * @brief El @c Type con el que figura un parametro de PLANTILLA.
 *
 * Si su tipo declarado contiene variables, no hay tipo que resolver todavia:
 * sale un @c TYPE_PARAM que lleva el texto de lo escrito.  Si no las contiene,
 * es un tipo normal y esto no aplica.
 *
 * @param t    El tipo declarado.
 * @param vars Los nombres que son variables.
 * @return El tipo sin resolver, o uno vacio si @p t no lleva variables (que se
 *         reconoce por @c kind != TYPE_PARAM).
 */
Type unresolved_param_type(const ast::TypeNode *t,
                           const std::vector<std::string> &vars);

/**
 * @brief Liga un type-node PATRON contra un @c Type concreto.
 *
 * Recorre los dos a la vez.  Un nombre de @p vars liga a lo que haya enfrente
 * -- y si ya estaba ligado, tiene que COINCIDIR, que es lo que hace que
 * `Par<T, T>` rechace `Par<i64, u8>` --; cualquier otra cosa exige igualdad
 * estructural.
 *
 * Un generico ya instanciado llega con su nombre aplanado (`Caja_i64`) y sin
 * sus argumentos, porque un @c Type no los lleva: se recuperan de la ficha de
 * la instanciacion, que es quien los guardo al crearla.
 *
 * @param tc       Para resolver tipos concretos y consultar esa ficha.
 * @param pattern  El tipo tal y como se declaro (puede contener variables).
 * @param concrete El tipo real del argumento.
 * @param vars     Los nombres que son variables, en orden de declaracion.
 * @param out      [in,out] Array de @c vars.size() tipos, indexado igual.
 * @param bound    [in,out] Mascara de los que ya estan ligados.
 * @return @c true si encajan.
 */
bool match_type_pattern(TypeChecker &tc, const ast::TypeNode *pattern,
                        const Type &concrete,
                        const std::vector<std::string> &vars, Type *out,
                        uint32_t &bound);

/**
 * @brief Cuanta FORMA impone un tipo declarado: lo que no es una variable.
 *
 * Sirve para ordenar dos plantillas homonimas que las dos encajan.  Entre
 *
 * ```vx
 * T saca<T>(T x)           // no pide nada:      0
 * T saca<T>(Caja<T> c)     // pide una Caja:     1
 * T saca<T>(Caja<Par<T>> c)// y que lleve un Par:2
 * ```
 *
 * gana la que mas pide, que es la que el programador escribio PARA ese caso:
 * la otra la habria cogido igual, y entonces la especifica no serviria de nada.
 *
 * @par Por que este criterio y no el orden de declaracion
 * Es el MISMO que el lenguaje ya usa al elegir especializacion -- exacta,
 * luego patron, luego la primaria --, asi que las dos formas de tener varias
 * versiones de algo se resuelven igual.  Y cuando dos piden lo mismo pero
 * cosas distintas, el empate no se rompe: se dice que la llamada es ambigua,
 * que es preferible a elegir por un criterio que no esta escrito en el fuente.
 *
 * @param t    El tipo declarado del parametro.
 * @param vars Los nombres que son variables de tipo.
 * @return Cuantos nodos del tipo NO son una variable.
 */
uint32_t shape_specificity(const ast::TypeNode *t,
                           const std::vector<std::string> &vars);

} // namespace generics
} // namespace vx

#endif // VX_GENERICS_GENERIC_INFER_H
