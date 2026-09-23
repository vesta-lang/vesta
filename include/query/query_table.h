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
 * @file query/query_table.h
 * @brief El catalogo de CONSULTAS al compilador: una fila por pregunta, y de
 *        ella salen todas las caras que la publican.
 *
 * Una consulta es "que le puedo preguntar al compilador sobre este programa":
 * su intermedio, su bytecode, su ensamblador, lo que el ASA sabe de el, lo que
 * cuesta, si los tres modos coinciden.  Hay VARIOS consumidores que hacen las
 * MISMAS preguntas por sitios distintos -- el editor, el terminal, y manana un
 * servidor MCP --, y de ahi sale la regla de esta capa:
 *
 *     el nombre de una consulta, sus parametros y sus defectos se escriben UNA
 *     vez; quien la publica los LEE de aqui, nunca los repite.
 *
 * Por que hace falta decirlo: porque ya habia divergido.  Los metodos a medida
 * del editor estaban escritos DOS veces -- la lista que se anuncia al cliente y
 * la cadena de comparaciones que los despacha -- y tres de ellos
 * (`irDiff`, `paramHints`, `symbolInfo`) se despachaban SIN anunciarse: un
 * cliente que descubriera por la lista no sabia que existian.  Nadie lo dijo
 * nunca, porque dos listas escritas a mano no se contradicen: simplemente una
 * se queda corta.  Es el mismo modo de fallo que el vocabulario de anotaciones
 * (siete sitios) y el de las banderas de entorno, y se cierra igual: UNA tabla.
 *
 * Esta cabecera es solo el MECANISMO.  No sabe que existe el editor ni el
 * inspector: las filas las registra quien tenga los manejadores, que es quien
 * conoce a los dos lados.  Si esta capa dependiera de un consumidor concreto
 * dejaria de ser compartida y volveriamos a tener dos caminos.
 */
#ifndef QUERY_QUERY_TABLE_H
#define QUERY_QUERY_TABLE_H

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "json.hpp"

namespace query {

// ===========================================================================
// El VOCABULARIO de una fila.  Enumeraciones con ancho explicito, y ninguna
// bandera empaquetada a mano: un entero estrecho elegido hoy es un refactor
// completo el dia que se quede corto, y ya paso con la FORMA de los operandos
// truncada a ocho bits.
// ===========================================================================

/**
 * @brief De que tipo es un parametro.
 *
 * Es lo que permite GENERAR el esquema que publica cada cara en vez de
 * escribirlo a mano al lado del manejador -- que es donde se desincroniza.
 */
enum class ParamType : uint16_t {
    String,
    UInt,
    Int,
    Bool,
};

/**
 * @brief Que papel tiene un parametro en la consulta.
 *
 * Enumeracion y no un `bool obligatorio`, porque un si/no no puede crecer: lo
 * que viene detras -- un parametro que se sigue aceptando pero esta retirado,
 * uno que se puede repetir, uno obligatorio salvo si viene otro -- no son mas
 * booleanos, son mas ESTADOS del mismo eje.  Con un `bool` cada uno seria otra
 * bandera paralela, y dos banderas paralelas acaban contradiciendose.
 */
enum class Presence : uint16_t {
    /// Sin el, la consulta no significa nada.  Falta -> error, nunca un cero.
    Required,
    /// Tiene defecto, y el defecto es una respuesta correcta.  @see QueryParam::def
    Optional,
    // Sitio reservado, sin coste: Deprecated (se acepta y se avisa),
    // Repeatable (lista), RequiredUnless (depende de otro).
};

/**
 * @brief Que NECESITA una consulta para poder contestar.
 *
 * Es un CONJUNTO: una consulta puede necesitar varias cosas a la vez.  El ancho
 * va dicho a proposito -- pasar de 32 valores es cambiar este `uint32_t` y nada
 * mas, porque nadie escribe el entero a mano.
 *
 * Ojo: esto dice que necesita, NO que hace.  Son dos preguntas distintas y van
 * en dos campos distintos (@ref Effect).  Mezclarlas en una sola bolsa de
 * banderas es el error que el ASA ya corrigio al separar en que esta escrito el
 * codigo de como se ejecuta.
 *
 * Y no es solo una marca: **cada valor trae consigo sus PARaMETROS**
 * (@ref bundle_params).  Las cinco del objetivo -- `os`, `arch`, `opt`,
 * `floatIsa`, `cpu` -- las usan la mitad de las consultas, y repetirlas fila a
 * fila seria volver a tener el mismo vocabulario escrito quince veces: basta
 * con que una se quede sin `cpu` para que su esquema mienta.  Dicho aqui, la
 * fila declara solo lo SUYO y el paquete pone el resto.
 */
enum class Needs : uint32_t {
    None = 0,
    /// Exige `uri`: la pregunta es sobre un documento concreto.
    Document = 1u << 0,
    /// Lee el objetivo (os, arch, opt, floatIsa, cpu): la respuesta CAMBIA con
    /// el, asi que forma parte de la pregunta y no es un adorno.
    Target = 1u << 1,
    /// Se contesta sobre el proyecto entero, no sobre un fichero suelto.
    Project = 1u << 2,
};

inline constexpr Needs operator|(Needs a, Needs b) {
    return static_cast<Needs>(static_cast<uint32_t>(a) |
                              static_cast<uint32_t>(b));
}
inline constexpr bool has(Needs set, Needs one) {
    return (static_cast<uint32_t>(set) & static_cast<uint32_t>(one)) != 0u;
}

/**
 * @brief Que HACE una consulta, por severidad creciente.
 *
 * EXCLUSIVO y ordenado, no un conjunto: quien lo lee tiene una sola pregunta
 * que hacer -- "puedo llamar a esto sin preguntarle a nadie?" -- y necesita UNA
 * respuesta.  Una consulta que ejecuta tambien escribe, asi que se queda con el
 * valor mas alto de los que le apliquen.
 *
 * Es lo que un servidor MCP traduce a sus anotaciones de herramienta, y lo que
 * el editor mira antes de lanzar algo sin avisar.
 */
enum class Effect : uint16_t {
    /// Solo mira.  No toca nada fuera del proceso.
    ReadOnly,
    /// Produce artefactos en disco (compilar deja un `.velb`).
    Writes,
    /// EJECUTA el programa del usuario.  Nunca sin politica delante.
    Executes,
};

/// Nombres estables para el volcado y para el protocolo.  NO son texto de
/// usuario: lo que se le ensena a una persona sale del catalogo multi-idioma.
const char *param_type_name(ParamType t);
const char *presence_name(Presence p);
const char *effect_name(Effect e);

struct ParamList;

/**
 * @brief Los parametros que aporta UN valor de @ref Needs.
 *
 * Quien genera el esquema recorre la fila y ademas cada bandera que tenga
 * puesta, asi que lo publicado incluye los parametros comunes sin que la fila
 * los repita.  @ref Args busca en los dos sitios por la misma razon: lo que el
 * manejador puede leer es exactamente lo que se publico.
 *
 * @param one UN valor, no un conjunto.
 * @return Sus parametros; vacio si ese valor no aporta ninguno.
 */
ParamList bundle_params(Needs one);

// ===========================================================================
// La FILA.
// ===========================================================================

/**
 * @brief El valor por defecto de un parametro opcional.
 *
 * Vive en la FILA y no en el manejador a proposito.  Repartidos por el
 * despacho -- que es donde estaban -- el esquema que se publica puede decir
 * "opcional" pero no QUE PASA si no lo mandas, con lo que el defecto vuelve a
 * estar escrito en un solo sitio que nadie mas ve.  Y el defecto es parte del
 * contrato: cambiar "post" por "pre" cambia lo que recibe quien no mando nada.
 */
struct DefaultValue {
    ParamType type = ParamType::String;
    const char *s = "";  ///< valido si type == String
    int64_t n = 0;       ///< valido si type == UInt / Int
    bool b = false;      ///< valido si type == Bool

    static constexpr DefaultValue str(const char *v) {
        return DefaultValue{ParamType::String, v, 0, false};
    }
    static constexpr DefaultValue uint(int64_t v) {
        return DefaultValue{ParamType::UInt, "", v, false};
    }
    static constexpr DefaultValue integer(int64_t v) {
        return DefaultValue{ParamType::Int, "", v, false};
    }
    static constexpr DefaultValue boolean(bool v) {
        return DefaultValue{ParamType::Bool, "", 0, v};
    }
};

/**
 * @brief La IDENTIDAD de un parametro: como se llama, de que tipo es y que
 *        significa.  Se escribe UNA vez para todo el sistema.
 *
 * Va aparte de su uso en una fila porque son dos cosas distintas, y mezclarlas
 * duplica: `arch` es el mismo parametro lo pida quien lo pida -- mismo nombre,
 * mismo tipo, misma explicacion --, mientras que si es obligatorio y con que
 * valor se queda cuando no viene SI cambia de una consulta a otra (`line` es
 * obligatorio para la ficha de una instruccion y opcional para el simbolo bajo
 * el cursor).  Escribiendolo todo junto, `arch` acababa declarado tres veces
 * con tres copias de su descripcion, libres de divergir -- que es el mismo
 * fallo que esta capa existe para cerrar, un nivel mas abajo.
 */
struct ParamSpec {
    const char *name = "";
    ParamType type = ParamType::String;
    /// Codigo del catalogo multi-idioma que lo describe.  Un codigo y no una
    /// frase: quien pregunta merece leerlo en su idioma, y el catalogo es el
    /// unico sitio donde vive el texto.
    const char *doc = "";
};

/**
 * @brief El USO de un parametro en una fila concreta.
 *
 * Apunta a su identidad y anade lo unico que de verdad depende de la consulta:
 * si hace falta, y que se supone cuando no viene.
 */
struct QueryParam {
    const ParamSpec *spec = nullptr;
    Presence presence = Presence::Required;
    /// Solo se mira si @c presence == Optional.
    DefaultValue def{};

    constexpr const char *name() const { return spec->name; }
    constexpr ParamType type() const { return spec->type; }
    constexpr const char *doc() const { return spec->doc; }
};

/// Este parametro hace falta: si no viene, la consulta no significa nada.
constexpr QueryParam required(const ParamSpec &s) {
    return QueryParam{&s, Presence::Required, {}};
}
/// Este se puede omitir, y entonces vale @p d -- que se PUBLICA en el esquema.
constexpr QueryParam optional(const ParamSpec &s, DefaultValue d) {
    return QueryParam{&s, Presence::Optional, d};
}

/**
 * @brief El catalogo de parametros: cada uno, UNA vez, para todo el sistema.
 *
 * Una fila compone los suyos de aqui.  Que el array de la fila guarde copias
 * del @c QueryParam es irrelevante -- son unos bytes de solo lectura --: lo que
 * importa es que la DEFINICION este en un sitio, porque es la definicion lo que
 * diverge, no la copia.
 */
namespace p {

inline constexpr ParamSpec uri{"uri", ParamType::String, "QRY.param.uri"};
inline constexpr ParamSpec os{"os", ParamType::String, "QRY.param.os"};
inline constexpr ParamSpec arch{"arch", ParamType::String, "QRY.param.arch"};
inline constexpr ParamSpec opt{"opt", ParamType::Int, "QRY.param.opt"};
inline constexpr ParamSpec float_isa{"floatIsa", ParamType::String,
                                     "QRY.param.floatIsa"};
inline constexpr ParamSpec cpu{"cpu", ParamType::String, "QRY.param.cpu"};
inline constexpr ParamSpec function{"function", ParamType::String,
                                    "QRY.param.function"};
inline constexpr ParamSpec phase{"phase", ParamType::String, "QRY.param.phase"};
inline constexpr ParamSpec kind{"kind", ParamType::String, "QRY.param.kind"};
inline constexpr ParamSpec format{"format", ParamType::String,
                                  "QRY.param.format"};
inline constexpr ParamSpec cost{"cost", ParamType::Bool, "QRY.param.cost"};
inline constexpr ParamSpec tier{"tier", ParamType::String, "QRY.param.tier"};
inline constexpr ParamSpec mode{"mode", ParamType::String, "QRY.param.mode"};
inline constexpr ParamSpec line{"line", ParamType::UInt, "QRY.param.line"};
inline constexpr ParamSpec character{"character", ParamType::UInt,
                                     "QRY.param.character"};
inline constexpr ParamSpec domain{"domain", ParamType::String,
                                  "QRY.param.domain"};
inline constexpr ParamSpec code{"code", ParamType::String, "QRY.param.code"};
inline constexpr ParamSpec subject{"subject", ParamType::UInt,
                                   "QRY.param.subject"};
inline constexpr ParamSpec reason{"reason", ParamType::String,
                                  "QRY.param.reason"};

} // namespace p

/**
 * @brief Los parametros de una fila, SIN contador escrito a mano.
 *
 * Deliberadamente no hay un campo `param_count`.  Ensanchar el entero solo
 * retrasa el problema; que nadie escriba el numero lo quita: el tamano lo
 * deduce el compilador del array, asi que no se puede truncar ni quedar
 * desfasado al anadir un parametro.
 */
struct ParamList {
    const QueryParam *first = nullptr;
    const QueryParam *last = nullptr;

    constexpr ParamList() = default;
    template <std::size_t N>
    constexpr ParamList(const QueryParam (&a)[N]) : first(a), last(a + N) {}

    constexpr const QueryParam *begin() const { return first; }
    constexpr const QueryParam *end() const { return last; }
    constexpr bool empty() const { return first == last; }
};

struct Args;
struct Ctx;

/**
 * @brief UNA consulta: todo lo que hay que saber de ella, en un sitio.
 *
 * El nombre va SIN prefijo -- `bytecode`, no `vesta/bytecode` -- porque el
 * prefijo es cosa del transporte: el editor lo publica como `vesta/bytecode`,
 * un servidor MCP como una herramienta `bytecode`, y el terminal como un
 * subcomando.  Escribirlo decorado aqui obligaria a cada cara a des-decorarlo,
 * y esa es otra copia del nombre.
 */
struct QueryDesc {
    const char *name = "";
    /// Codigo del catalogo multi-idioma con lo que hace.  Ver @c QueryParam::doc.
    const char *summary = "";
    ParamList params{};
    Needs needs = Needs::None;
    Effect effect = Effect::ReadOnly;
    /// El manejador.  Puntero a funcion y no `std::function`: la tabla es
    /// estatica y no captura nada, asi que no hay por que pagar una indireccion
    /// ni una reserva por fila.
    nlohmann::json (*fn)(Ctx &, const Args &) = nullptr;
};

// ===========================================================================
// Leer los argumentos: la puerta que impide que la fila y el manejador se
// separen.
// ===========================================================================

/**
 * @brief El manejador pide un parametro que su propia fila NO declara.
 *
 * Es un fallo NUESTRO, no de quien pregunta: significa que el esquema que se
 * publica miente sobre que parametros acepta la consulta.  Va aparte de
 * @ref MissingArgument justo por eso -- se arreglan en sitios opuestos, uno
 * tocando la tabla y el otro la peticion --, y confundirlos haria que un error
 * del compilador se le echara en cara al cliente.
 */
class BadQueryDefinition : public std::logic_error {
  public:
    explicit BadQueryDefinition(const std::string &what);
};

/**
 * @brief Falta un parametro que la fila declara @c Required.
 *
 * Esto SI es de quien pregunta, y por eso lleva el nombre dentro: el transporte
 * lo convierte en una respuesta de error que dice CUAL falta, en vez de
 * contestar con un cero que parece un resultado.
 */
class MissingArgument : public std::runtime_error {
  public:
    explicit MissingArgument(const std::string &name);
    /// El parametro que falta, para que el transporte lo nombre.
    const std::string &name() const { return name_; }

  private:
    std::string name_;
};

/**
 * @brief Los argumentos de UNA invocacion, leidos contra la fila que los
 *        declara.
 *
 * Aqui esta la mitad que hace que la tabla no sea decorativa.  Si el manejador
 * pudiera leer el JSON crudo, la fila seria una descripcion que nadie
 * comprueba: declarar `line` y leer `linea` compila, se publica un esquema que
 * miente, y quien manda `line` recibe un cero silencioso.  Es exactamente la
 * clase de fallo que esta capa existe para cerrar, asi que:
 *
 * - pedir un parametro que la fila NO declara es un fallo RUIDOSO, no un
 *   defecto callado;
 * - un @c Required que no viene es un error con nombre, tambien ruidoso;
 * - un @c Optional que no viene devuelve el defecto DE LA FILA, que es el mismo
 *   que se publico en el esquema.
 */
struct Args {
    /// La fila contra la que se leen.  Nunca nula mientras el Args viva.
    const QueryDesc *desc = nullptr;
    /// El objeto `params` tal y como llego.  No se lee directamente: se pasa
    /// por los captadores de abajo para que la fila gobierne.
    const nlohmann::json *raw = nullptr;

    std::string str(const char *name) const;
    uint32_t uint(const char *name) const;
    int64_t integer(const char *name) const;
    bool boolean(const char *name) const;

    /// Si vino de verdad (para distinguir "no lo mando" de "mando el defecto").
    bool given(const char *name) const;
};

/**
 * @brief Lo que un manejador necesita para contestar, sin saber quien pregunta.
 *
 * Se hereda por cada consumidor para meter lo suyo (el inspector del editor, el
 * proyecto del terminal).  Asi las filas se escriben una vez aunque el contexto
 * de cada cara sea distinto.
 */
struct Ctx {
    virtual ~Ctx() = default;
};

// ===========================================================================
// El REGISTRO.
// ===========================================================================

/**
 * @brief Todas las consultas conocidas.
 *
 * Una sola lista, recorrida entera al buscar.  Son un par de docenas de filas
 * contiguas y la comparacion corta por el tamano antes de mirar un solo byte,
 * asi que se acaba antes de lo que un mapa tarda en calcular el hash -- mismo
 * criterio que los dominios ya producidos del ASA.
 *
 * NO se internan los nombres, y merece decir por que, porque la primera version
 * si lo hacia: el pozo compartido (@c util::intern_name) **no se vacia nunca**
 * -- esta pensado para nombres de fichero, acotados por el proyecto --, asi que
 * pasarle el metodo que llega POR EL PROTOCOLO deja que un cliente lo haga
 * crecer sin limite mandando nombres inventados.  Y encima no compraba nada:
 * toma un cerrojo, y su atajo por hilo falla justo con los nombres
 * desconocidos, que son todos distintos.
 */
class Registry {
  public:
    /// El registro del proceso.
    static Registry &instance();

    /**
     * @brief Da de alta un bloque de filas.  Se llama una vez al arrancar.
     *
     * Las filas tienen que vivir lo que el programa (un array estatico); el
     * registro guarda punteros a ellas, no copias.
     *
     * @param rows Primera fila del bloque.
     * @param n    Cuantas hay.  Lo pasa @ref add_array deduciendolo.
     */
    void add(const QueryDesc *rows, std::size_t n);

    /// Igual, deduciendo el tamano del array: nadie escribe el numero.
    template <std::size_t N> void add_array(const QueryDesc (&rows)[N]) {
        add(rows, N);
    }

    /// La fila de @p name, o nullptr si no es una consulta nuestra.
    const QueryDesc *find(const std::string &name) const;

    /// Todas, en el orden en que se registraron.  Es de aqui de donde sale la
    /// lista que cada cara publica; escribirla a mano es lo que ya divergio.
    const std::vector<const QueryDesc *> &all() const { return rows_; }

  private:
    Registry() = default;

    /// UNA lista.  Habia dos -- una por nombre y otra por orden -- con las
    /// mismas filas dentro: el doble de memoria y dos sitios que mantener de
    /// acuerdo, a cambio de nada.
    std::vector<const QueryDesc *> rows_;
};

} // namespace query

#endif // QUERY_QUERY_TABLE_H
