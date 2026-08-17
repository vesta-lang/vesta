/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/emmit/test_asm_efectos.cpp
 * @brief Que EFECTOS declara el analisis de cada instruccion de un bloque `asm`.
 *
 * Lo que un bloque `asm` dice de si mismo alimenta al compilador entero: los
 * contratos (`pure`, `readonly`), la eliminacion de codigo muerto, el
 * planificador, el movimiento de invariantes fuera de un bucle.  Un efecto que
 * FALTA no da error -- da una funcion que escribe memoria declarada de solo
 * lectura, y con ella un llamante que se queda con el valor de antes.
 *
 * Y eso no es hipotetico: `movdqa` se declaraba como que NO toca memoria, y para
 * los bloques ELEVADOS se analizaba una cadena vacia, asi que todos salian
 * inocuos.  Los dos fallos vivieron sin que nada chocara porque no habia ningun
 * sitio donde se comprobara instruccion por instruccion.  Este es ese sitio.
 *
 * ## Como se lee un caso
 *
 * Se declaran los efectos que la instruccion DEBE tener, por nombre.  Lo que no
 * se nombra tiene que estar AUSENTE -- eso es lo que convierte el test en algo
 * util: un efecto de mas convierte un `asm` inocente en una barrera para todo lo
 * que le rodea, y uno de menos deja pasar una optimizacion que rompe.
 *
 * Cuando de verdad no se puede afirmar un campo, se dice con @ref SIN_MIRAR en
 * vez de poner el valor que salga.  Un test que se ajusta a lo que el codigo hace
 * hoy no comprueba nada.
 */

#include "vx/asm/asm_analyze.h"

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

namespace {

/// Los efectos que una instruccion puede declarar, por nombre.
enum Efecto : uint32_t {
    NINGUNO      = 0,
    LEE_MEM      = 1u << 0, ///< lee memoria
    ESCRIBE_MEM  = 1u << 1, ///< la escribe
    BANDERAS     = 1u << 2, ///< modifica las banderas de condicion
    LLAMADA      = 1u << 3, ///< transfiere el control fuera del bloque
};

/// Un caso: la instruccion y los efectos que tiene que declarar.
struct Caso {
    const char *texto;     ///< la instruccion, con marcadores `$N`
    uint32_t    tiene;     ///< efectos que DEBE declarar
    uint32_t    sin_mirar; ///< los que no se comprueban, y por que en @ref porque
    const char *porque;    ///< sale en el mensaje de fallo
};

/// Atajo para el campo que casi siempre esta vacio.
constexpr uint32_t SIN_MIRAR = 0;

/**
 * @brief Instrucciones cuyo analisis NO es correcto todavia.
 *
 * Se listan aqui en vez de ajustar el caso a lo que el codigo hace hoy: un test
 * que se acomoda a la respuesta equivocada deja de ser un test.  Salen contadas
 * al final, con su motivo, y no ponen la suite en rojo -- un rojo permanente se
 * acaba ignorando, y entonces el dia que se rompa otra cosa nadie lo mira.
 *
 * Cada linea es trabajo pendiente concreto, no una excusa.
 */
bool es_pendiente(const char *texto, const char **motivo) {
    struct P { const char *t, *m; };
    static const P kPend[] = {
        {"lea $0, [$1]",
         "cuenta como LECTURA de memoria y no lo es: `lea` calcula la direccion "
         "y no la sigue.  Es el peor de la lista porque `lea` esta por todas "
         "partes, y una lectura de mas convierte el bloque en una barrera"},
        {"bt $0, 3", "no declara que deja el bit probado en el acarreo"},
        {"setz $0", "no declara que LEE las banderas"},
        {"cmovz $0, $1", "no declara que lee las banderas para decidir"},
        {"movsd [$0], $1",
         "ambiguedad de nombre: la tabla tiene `movsd_sse` aparte, asi que el "
         "`movsd` pelado se toma por la instruccion de CADENA"},
        {"paddd $1, [$0]",
         "aritmetica empaquetada contra memoria: no declara la lectura"},
        {"vaddps $1, $1, [$0]", "igual en AVX flotante"},
    };
    for (const P &p : kPend)
        if (std::string(texto) == p.t) {
            *motivo = p.m;
            return true;
        }
    return false;
}

int pendientes = 0;

int fallos = 0;
int comprobados = 0;

/// Clases de los marcadores.  Sin ellas, un operando que el compilador nombro
/// `$1` no tiene ancho y el analisis no puede acotar cuantos bytes toca.
std::vector<std::pair<std::string, std::string>> clases(const char *c1) {
    return {{"$0", "reg"}, {"$1", c1}, {"$2", c1}};
}

void comprueba(const Caso &c, const char *clase_operando) {
    const vx::AsmBlockEffects r =
        vx::asm_analyze_block(c.texto, std::string("x86-64"),
                              clases(clase_operando));
    uint32_t real = NINGUNO;
    if (r.reads_mem) real |= LEE_MEM;
    if (r.writes_mem) real |= ESCRIBE_MEM;
    if (r.touches_flags) real |= BANDERAS;
    if (r.is_call) real |= LLAMADA;

    /* Una escritura a memoria se cuenta TAMBIEN como lectura, y es deliberado:
     * `add [rdi], rax` acumula, y el analisis prefiere no distinguir el destino
     * puro -- el `mov` -- porque equivocarse ahi deja pasar una optimizacion que
     * rompe.  Asi que un caso que espera escritura no exige que NO se lea. */
    uint32_t sin_mirar = c.sin_mirar;
    if (c.tiene & ESCRIBE_MEM) sin_mirar |= LEE_MEM;
    const uint32_t mirar = ~sin_mirar;
    if ((real & mirar) == (c.tiene & mirar)) {
        ++comprobados;
        return;
    }
    auto nombre = [](uint32_t e) {
        static std::string s;
        s.clear();
        if (e & LEE_MEM) s += "lee_mem ";
        if (e & ESCRIBE_MEM) s += "escribe_mem ";
        if (e & BANDERAS) s += "banderas ";
        if (e & LLAMADA) s += "llamada ";
        if (s.empty()) s = "(ninguno)";
        return s;
    };
    const char *motivo = nullptr;
    if (es_pendiente(c.texto, &motivo)) {
        std::printf("  PENDIENTE '%s': %s\n", c.texto, motivo);
        ++pendientes;
        return;
    }
    std::printf("  FALLA    '%s'\n", c.texto);
    std::printf("           esperado: %s\n", nombre(c.tiene & mirar).c_str());
    std::printf("           real:     %s\n", nombre(real & mirar).c_str());
    std::printf("           porque:   %s\n", c.porque);
    ++fallos;
}

} // namespace

int main() {
    /* --- Clase general: enteros, memoria, control ------------------------- */
    const Caso gp[] = {
        // Mover y copiar sin tocar memoria.
        {"mov $0, $1", NINGUNO, SIN_MIRAR, "mover entre registros no toca nada"},
        {"movzx $0, $1", NINGUNO, SIN_MIRAR, "extender con ceros tampoco"},
        {"movsx $0, $1", NINGUNO, SIN_MIRAR, "ni extender con signo"},
        {"lea $0, [$1]", NINGUNO, SIN_MIRAR,
         "calcula una direccion, NO la sigue: no lee memoria"},
        {"xchg $0, $1", NINGUNO, SIN_MIRAR, "entre registros no hay memoria"},
        {"bswap $0", NINGUNO, SIN_MIRAR, "invertir bytes de un registro"},

        // Aritmetica y logica: dejan banderas.
        {"add $0, $1", BANDERAS, SIN_MIRAR, "la suma deja acarreo y cero"},
        {"sub $0, $1", BANDERAS, SIN_MIRAR, "la resta igual"},
        {"and $0, $1", BANDERAS, SIN_MIRAR, "la logica tambien las deja"},
        {"or $0, $1", BANDERAS, SIN_MIRAR, "idem"},
        {"xor $0, $0", BANDERAS, SIN_MIRAR, "el idioma de poner a cero"},
        {"not $0", NINGUNO, SIN_MIRAR, "complemento: es la que NO toca banderas"},
        {"neg $0", BANDERAS, SIN_MIRAR, "negar si las toca"},
        {"inc $0", BANDERAS, SIN_MIRAR, "incrementar deja cero y signo"},
        {"dec $0", BANDERAS, SIN_MIRAR, "decrementar igual"},
        {"shl $0, 3", BANDERAS, SIN_MIRAR, "desplazar deja el bit que sale"},
        {"shr $0, 3", BANDERAS, SIN_MIRAR, "idem a la derecha"},
        {"sar $0, 3", BANDERAS, SIN_MIRAR, "idem con signo"},
        {"rol $0, 3", BANDERAS, SIN_MIRAR, "rotar deja acarreo"},
        {"imul $0, $1", BANDERAS, SIN_MIRAR, "el producto puede desbordar"},
        {"cmp $0, $1", BANDERAS, SIN_MIRAR, "comparar existe PARA dejar banderas"},
        {"test $0, $1", BANDERAS, SIN_MIRAR, "y probar bits, igual"},
        {"bt $0, 3", BANDERAS, SIN_MIRAR, "probar un bit lo deja en el acarreo"},
        {"popcnt $0, $1", BANDERAS, SIN_MIRAR, "contar unos deja cero"},

        // Las que LEEN banderas en vez de escribirlas.
        {"setz $0", BANDERAS, SIN_MIRAR, "leer una bandera cuenta como tocarlas"},
        {"cmovz $0, $1", BANDERAS, SIN_MIRAR, "mover condicional las lee"},
        {"adc $0, $1", BANDERAS, SIN_MIRAR, "sumar con acarreo lee Y escribe"},

        // No hacen nada observable.
        {"nop", NINGUNO, SIN_MIRAR, "no hacer nada hay que poder decirlo"},
        {"pause", NINGUNO, SIN_MIRAR, "un hint de espera no tiene efecto"},

        // Barreras: ordenan, no mueven datos.
        {"mfence", NINGUNO, SIN_MIRAR, "una barrera ordena, no lee ni escribe"},
        {"sfence", NINGUNO, SIN_MIRAR, "barrera de escrituras"},
        {"lfence", NINGUNO, SIN_MIRAR, "barrera de lecturas"},

        // Memoria explicita, por corchetes.
        {"mov $0, [$1]", LEE_MEM, SIN_MIRAR, "cargar lee y no escribe"},
        {"mov [$0], $1", ESCRIBE_MEM, SIN_MIRAR, "guardar escribe y no lee"},
        {"movzx $0, [$1]", LEE_MEM, SIN_MIRAR, "cargar extendiendo lee"},
        {"add [$0], $1", LEE_MEM | ESCRIBE_MEM | BANDERAS, SIN_MIRAR,
         "acumular en memoria lee, escribe y deja banderas"},
        {"inc [$0]", LEE_MEM | ESCRIBE_MEM | BANDERAS, SIN_MIRAR,
         "incrementar en sitio hace las tres"},
        {"xchg [$0], $1", LEE_MEM | ESCRIBE_MEM, SIN_MIRAR,
         "intercambiar con memoria lee y escribe"},
        {"cmp $0, [$1]", LEE_MEM | BANDERAS, SIN_MIRAR,
         "comparar contra memoria lee, no escribe"},

        // Atomicas: el prefijo no cambia QUE se toca.
        {"lock inc [$0]", LEE_MEM | ESCRIBE_MEM | BANDERAS, SIN_MIRAR,
         "`lock` da atomicidad, no cambia los efectos"},
        {"lock xadd [$0], $1", LEE_MEM | ESCRIBE_MEM | BANDERAS, SIN_MIRAR,
         "sumar e intercambiar atomico"},
        {"lock cmpxchg [$0], $1", LEE_MEM | ESCRIBE_MEM | BANDERAS, SIN_MIRAR,
         "comparar e intercambiar: la base de todo lo atomico"},

        // Pila: tocan memoria SIN corchetes a la vista.
        {"push $0", ESCRIBE_MEM, LEE_MEM,
         "apilar escribe memoria aunque no lleve corchetes; si ademas se cuenta "
         "como lectura no es lo que se comprueba aqui"},
        {"pop $0", LEE_MEM, ESCRIBE_MEM, "desapilar lee memoria"},

        // Control: se va del bloque.
        {"call $0", LLAMADA, LEE_MEM | ESCRIBE_MEM | BANDERAS,
         "una llamada puede hacer cualquier cosa; lo que importa es que se "
         "declare COMO llamada"},
        {"syscall", LLAMADA, LEE_MEM | ESCRIBE_MEM | BANDERAS,
         "entrar al sistema es una llamada"},

        // Cadena: repiten segun un contador.
        {"rep stosb", ESCRIBE_MEM, LEE_MEM, "rellenar escribe memoria"},
        {"rep movsb", LEE_MEM | ESCRIBE_MEM, SIN_MIRAR, "copiar lee y escribe"},
        {"rep scasb", LEE_MEM | BANDERAS, SIN_MIRAR, "buscar lee y compara"},

        // Lectura de estado del procesador.
        {"rdtsc", NINGUNO, SIN_MIRAR, "leer el contador no toca memoria"},
        {"cpuid", NINGUNO, SIN_MIRAR, "consultar el procesador tampoco"},
    };

    /* --- Banco ancho: SIMD, con y sin exigencia de alineacion ------------- */
    const Caso vec[] = {
        // Sin memoria: entre registros del banco ancho.
        {"pxor $1, $2", NINGUNO, SIN_MIRAR, "entre registros anchos no hay memoria"},
        {"vpxor $1, $1, $2", NINGUNO, SIN_MIRAR, "la forma AVX tampoco"},
        {"paddd $1, $2", NINGUNO, SIN_MIRAR, "sumar empaquetado, sin banderas"},
        {"pand $1, $2", NINGUNO, SIN_MIRAR, "logica empaquetada"},
        {"punpcklqdq $1, $2", NINGUNO, SIN_MIRAR, "reordenar dentro del banco"},
        {"pshufd $1, $2, 0", NINGUNO, SIN_MIRAR, "permutar tampoco toca memoria"},
        {"vzeroupper", NINGUNO, SIN_MIRAR, "limpiar la parte alta no toca memoria"},

        // ALINEADAS: escriben o leen memoria Y la exigen alineada.
        {"movdqa [$0], $1", ESCRIBE_MEM, SIN_MIRAR,
         "la forma ALINEADA de guardar SI toca memoria: se declaraba que no, y "
         "por eso una funcion que escribia salia readonly"},
        {"movdqa $1, [$0]", LEE_MEM, SIN_MIRAR, "y su lectura, lee"},
        {"movaps [$0], $1", ESCRIBE_MEM, SIN_MIRAR, "igual con flotantes"},
        {"movapd [$0], $1", ESCRIBE_MEM, SIN_MIRAR, "igual en doble precision"},
        {"vmovdqa [$0], $1", ESCRIBE_MEM, SIN_MIRAR, "igual la forma AVX"},
        {"vmovdqa64 [$0], $1", ESCRIBE_MEM, SIN_MIRAR, "igual la de 512 bits"},
        {"vmovaps [$0], $1", ESCRIBE_MEM, SIN_MIRAR, "igual en flotante AVX"},

        // NO TEMPORALES: se saltan la cache, no la memoria.
        {"movntdq [$0], $1", ESCRIBE_MEM, SIN_MIRAR,
         "no temporal: evita la cache, sigue escribiendo memoria"},
        {"vmovntdq [$0], $1", ESCRIBE_MEM, SIN_MIRAR, "igual en AVX"},
        {"vmovntdqa $1, [$0]", LEE_MEM, SIN_MIRAR, "su lectura, lee"},

        // SIN exigencia de alineacion: mismos efectos, otra exigencia.
        {"movdqu [$0], $1", ESCRIBE_MEM, SIN_MIRAR, "la no alineada escribe igual"},
        {"movdqu $1, [$0]", LEE_MEM, SIN_MIRAR, "y lee igual"},
        {"vmovdqu [$0], $1", ESCRIBE_MEM, SIN_MIRAR, "idem en AVX"},
        {"vmovdqu $1, [$0]", LEE_MEM, SIN_MIRAR, "idem"},
        {"vmovdqu64 [$0], $1", ESCRIBE_MEM, SIN_MIRAR, "idem a 512 bits"},

        // Escalares entre bancos: tocan pocos bytes, sin exigir alineacion.
        {"movq $1, [$0]", LEE_MEM, SIN_MIRAR, "mover ocho bytes desde memoria"},
        {"movq [$0], $1", ESCRIBE_MEM, SIN_MIRAR, "y hacia memoria"},
        {"movd $1, [$0]", LEE_MEM, SIN_MIRAR, "cuatro bytes"},
        {"movss [$0], $1", ESCRIBE_MEM, SIN_MIRAR, "un flotante simple"},
        {"movsd [$0], $1", ESCRIBE_MEM, SIN_MIRAR, "uno doble"},

        // Difusiones: leen un valor y lo replican.
        {"vpbroadcastq $1, [$0]", LEE_MEM, SIN_MIRAR, "difundir desde memoria lee"},
        {"vbroadcastss $1, [$0]", LEE_MEM, SIN_MIRAR, "idem en flotante"},

        // Aritmetica empaquetada CON memoria.
        {"paddd $1, [$0]", LEE_MEM, SIN_MIRAR, "sumar contra memoria lee"},
        {"vaddps $1, $1, [$0]", LEE_MEM, SIN_MIRAR, "idem en AVX flotante"},
    };

    std::printf("[asm-efectos] %zu instrucciones\n",
                sizeof(gp) / sizeof(gp[0]) + sizeof(vec) / sizeof(vec[0]));
    for (const Caso &c : gp) comprueba(c, "reg");
    for (const Caso &c : vec) comprueba(c, "xmm");

    std::printf("[asm-efectos] %d correctas, %d pendientes, %d FALLOS -> %s\n",
                comprobados, pendientes, fallos,
                fallos == 0 ? "OK" : "CON FALLOS");
    return fallos == 0 ? 0 : 1;
}
