/**
 * @file annotation_names.cpp
 * @brief La tabla de anotaciones del lenguaje y la busqueda del nombre cercano.
 *
 * Ver @ref annotation_names.h para por que existe.  Aqui solo esta la tabla y
 * como se consulta.
 */
#include "vx/annotation_names.h"

#include <cstring>

#include "Levenshtein.hpp"

namespace vx {

namespace {

/**
 * Todas las anotaciones que el parser sabe manejar, en ORDEN ALFABETICO para
 * poder buscarlas por biseccion.
 *
 * Salieron de leer los siete sitios que las reconocen, no de la documentacion:
 * `CLAUDE.md` nombra `@Asm`, `@Export`, `@Module` y `@Generic`, que ya no las
 * maneja nadie -- estan retiradas o nunca llegaron --, y ponerlas aqui seria
 * prometer que funcionan.
 *
 * Las dos RETIRADAS (`@AllocatorOverride`, `@PanicHandler`) NO estan a
 * proposito: tienen su propio mensaje, que dice el reemplazo, y ese es mejor
 * que "no existe, quiza querias decir...".
 *
 * Al anyadir una anotacion al parser hay que anyadirla AQUI, o quien la escriba
 * recibira un error diciendo que no existe.  Es la direccion buena del fallo:
 * antes, olvidarse daba una anotacion que no hacia nada y nadie lo notaba.
 */
const char *const k_annotations[] = {
    /* -- Sobre una declaracion: que ES, o como se compila -- */
    "Abstract",           ///< clase que no se puede instanciar
    "After",              ///< AOP: aspecto que corre al salir
    "AfterReturning",     ///< AOP: al salir con valor
    "AllArgsConstructor", ///< genera un constructor con todos los campos
    "Around",             ///< AOP: aspecto que envuelve
    "Aspect",             ///< clase que agrupa aspectos
    "Async",              ///< devuelve un futuro implicito
    "Before",             ///< AOP: aspecto que corre al entrar
    "Builder",            ///< genera el patron constructor
    "Data",               ///< combo: getters, setters, toString, equals
    "EqualsAndHashCode",  ///< genera la comparacion y el hash
    "Final",              ///< clase no heredable / metodo no redefinible
    "Getter",             ///< genera el lector de un campo
    "HelperOverride", ///< sustituye un helper multi-versionado (memcpy, ...)
    "Hook",           ///< instrumentacion en compilacion
    "Inline",         ///< sugerencia al optimizador
    "Introspect",     ///< expone la forma del tipo al comptime
    "Log",            ///< inyecta el registro
    "Macro",          ///< se expande al compilar
    "Naked",          ///< sin prologo ni epilogo: el cuerpo se emite tal cual
    "NoArgsConstructor", ///< genera el constructor vacio
    "NoExcept",          ///< la funcion no lanza
    "NoExceptions",      ///< el modulo entero sin excepciones
    "NoIdiom",           ///< no reconocer patrones en este cuerpo
    "NoInstrument",      ///< no instrumentar aunque haya @Hook
    "NonNull",           ///< campo que no admite nulo
    "Override", ///< redefine un metodo heredado (OBLIGATORIA al redefinir)
    "Provides", ///< cubre un builtin del lenguaje: el mecanismo de ganchos
    "Pure",     ///< sin efectos observables
    "RequiredArgsConstructor", ///< constructor con los campos obligatorios
    "Setter",                  ///< genera el escritor de un campo
    "StringConcat",            ///< sustituye el `+` de cadenas
    "StringEq",                ///< sustituye el `==` de cadenas
    "SyncImpl",                ///< sustituye la primitiva de monitor
    "Synchronized",            ///< el cuerpo se sincroniza sobre `this`
    "Target",                  ///< condiciona la declaracion al objetivo
    "ToString",                ///< genera la conversion a cadena
    "Value",                   ///< combo inmutable
    "Virtual",                 ///< metodo de struct con despacho dinamico
    "With",                    ///< genera el copiador con un campo cambiado

    /* -- En minuscula: colocacion, forma y contratos -- */
    "align",       ///< alineacion de un tipo
    "alloc",       ///< contrato: cuanto reserva
    "allocator",   ///< efecto: es un asignador
    "at",          ///< direccion fija
    "bits",        ///< ancho del bloque asm (16/32/64)
    "blocks",      ///< efecto: puede bloquear
    "cold",        ///< camino frio
    "complexity",  ///< contrato de coste (Big-O)
    "det",         ///< efecto: determinista
    "element",     ///< forma del elemento de un array en un overlay
    "endian",      ///< orden de bytes de un campo de overlay
    "fast",        ///< coma flotante relajada
    "fp",          ///< modo de coma flotante
    "frees",       ///< efecto: libera
    "hot",         ///< camino caliente
    "io",          ///< efecto: toca entrada/salida
    "keeps_state", ///< efecto: guarda estado entre llamadas
    "maps",        ///< efecto: mapea memoria
    "no_heap",     ///< no usa el heap
    "noblock",     ///< efecto: no bloquea
    "nondet",      ///< efecto: no determinista
    "nopanic",     ///< contrato: no entra en panico
    "nothrow",     ///< contrato: no lanza
    "notrap",      ///< efecto: no atrapa
    "offset",      ///< desplazamiento de un campo de overlay
    "opaque",      ///< typedef cuyo interior no se ve
    "order",       ///< orden de los campos
    "overlaps",    ///< campo de overlay que comparte sitio con otro
    "overlay",     ///< vista sobre memoria
    "panics",      ///< efecto: puede entrar en panico
    "pod",         ///< tipo plano, sin constructores
    "pure",        ///< efecto: sin efectos
    "reads_env",   ///< efecto: lee el entorno
    "section",     ///< seccion del binario donde va
    "size",        ///< tamanyo fijo
    "stack",       ///< contrato: cuanta pila usa
    "strict",      ///< coma flotante estricta
    "throws",      ///< efecto: lanza
    "traps",       ///< efecto: atrapa
    "writes_env",  ///< efecto: escribe el entorno
};

constexpr size_t k_count = sizeof(k_annotations) / sizeof(k_annotations[0]);

} // namespace

bool annotation_exists(const std::string &name) noexcept {
    /* Recorrido lineal, y no biseccion sobre la tabla ordenada.
     *
     * Bisecar exigiria que el orden sea CIERTO, y el dia que alguien meta una
     * entrada en el sitio equivocado la busqueda no daria un error: diria que
     * una anotacion buena no existe.  Un invariante que nadie comprueba y cuyo
     * incumplimiento miente no compensa aqui: son ochenta entradas y se
     * pregunta una vez por cada `@` escrita.  El orden alfabetico se mantiene
     * para leerla y para listarla, no para buscarla. */
    for (size_t i = 0; i < k_count; ++i)
        if (name == k_annotations[i]) return true;
    return false;
}

const char *annotation_nearest(const std::string &name) noexcept {
    if (name.empty()) return nullptr;
    const char *best = nullptr;
    int best_dist = 0;
    for (size_t i = 0; i < k_count; ++i) {
        const std::string cand(k_annotations[i]);
        /* Filtro barato antes de medir: dos nombres que difieren mucho de
         * LARGO no van a estar cerca, y medir cuesta mas que restar. */
        const size_t la = name.size(), lb = cand.size();
        const size_t dif = la > lb ? la - lb : lb - la;
        if (dif > 3) continue;
        const int d = utils::Levenshtein::distance(name, cand);
        /* Un parecido MINIMO, o la sugerencia despista: sin este tope, un
         * nombre inventado entero arrastraba la entrada mas corta de la tabla.
         * Dos ediciones sobre un nombre de anotacion ya es mucho. */
        if (d > 2) continue;
        if (best == nullptr || d < best_dist ||
            (d == best_dist && cand.size() < std::strlen(best))) {
            best = k_annotations[i];
            best_dist = d;
        }
    }
    return best;
}

const char *const *annotation_all(size_t *out_count) noexcept {
    if (out_count) *out_count = k_count;
    return k_annotations;
}

} // namespace vx
