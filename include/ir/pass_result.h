/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file pass_result.h
 * @brief Lo que devuelve un pase del intermedio, y la unica puerta para leerlo.
 *
 * Vive aparte de @c ir_optimizer.h porque los pases que tienen cabecera propia
 * (@c passes/unroll.h y companyia) tambien la necesitan, y no deben arrastrar
 * el optimizador entero para declarar su firma.
 */
#ifndef IR_PASS_RESULT_H
#define IR_PASS_RESULT_H

namespace ir {

struct IrFunction;
struct IrModule;

/**
 * @brief Lo que devuelve un pase: si cambio algo, y A QUIEN se lo cambio.
 *
 * No es un `bool` disfrazado.  Existe para que sea IMPOSIBLE saber si un pase
 * cambio algo sin avanzar, en el mismo gesto, la version de lo que cambio.
 *
 * @par Por que no basta con acordarse
 * La version es lo que impide entregar un analisis cacheado despues de mutar la
 * funcion, y lo que ese analisis guarda son PUNTEROS a instrucciones: servirlo
 * rancio no es una respuesta imprecisa, es leer memoria liberada.  El modelo
 * anterior lo dejaba en manos de una macro que solo usaba UNA de las etapas,
 * asi que los nueve pases de la etapa de modulo llevaban mutando el IR sin
 * subirla -- y cuatro de ellos se llaman ademas desde fuera del optimizador,
 * donde nadie iba a acordarse tampoco.
 *
 * Costo 165 rojos descubrirlo, y el modo de fallo es el peor: no da error, deja
 * de invalidar.  Ponerlos a mano en los sitios que uno encuentra tampoco basta
 * -- se probo, y de 165 rojos bajo a 150 --: garantiza los que miro, no los que
 * hay.  Por eso el arreglo es que no se pueda escribir el codigo que se los
 * salta.
 *
 * @par Como lo impide
 * El `bool` de dentro solo lo puede leer @ref applied , que es amiga.  Asi
 * `if (ir_pass_x(fn))` NO COMPILA: hay que escribir `if
 * (applied(ir_pass_x(fn)))` y esa puerta avanza la version.  Y el resultado
 * lleva a QUE funcion se aplico, para que la puerta no pueda avanzar la version
 * equivocada -- que seria el mismo fallo mudo con otro disfraz.
 */
class [[nodiscard]] PassResult {
  public:
    /**
     * @brief Lo que construye un pase al terminar.
     * @param fn      La funcion sobre la que corrio.
     * @param changed Si la cambio.
     * @return El resultado, que solo @ref applied puede abrir.
     */
    static PassResult of(IrFunction &fn, bool changed) {
        return PassResult(&fn, changed);
    }

  private:
    PassResult(IrFunction *fn, bool changed) : fn_(fn), changed_(changed) {}
    IrFunction *fn_;
    bool changed_;
    friend bool applied(PassResult r);
};

/**
 * @brief La UNICA forma de leer si un pase cambio algo.  Avanza la version.
 * @param r Lo que devolvio el pase.
 * @return true si cambio algo.
 */
bool applied(PassResult r);

/**
 * @brief Igual, para un pase que recorre el MODULO.
 *
 * Estos no dicen a quien tocaron, asi que al abrirlo se dan por cambiadas
 * TODAS las funciones.  Es conservador y se nota -- despues de un inline nadie
 * se salta nada --, pero equivocarse por el otro lado no da un error: deja de
 * invalidar, y eso es lo que se esta cerrando.
 */
class [[nodiscard]] ModulePassResult {
  public:
    static ModulePassResult of(IrModule &mod, bool changed) {
        return ModulePassResult(&mod, changed);
    }

  private:
    ModulePassResult(IrModule *mod, bool changed)
        : mod_(mod), changed_(changed) {}
    IrModule *mod_;
    bool changed_;
    friend bool applied(ModulePassResult r);
};

/// @copydoc applied(PassResult)
bool applied(ModulePassResult r);

} // namespace ir

#endif // IR_PASS_RESULT_H
