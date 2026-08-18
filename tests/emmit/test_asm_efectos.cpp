/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file tests/emmit/test_asm_efectos.cpp
 * @brief Que EFECTOS declara el analisis de cada instruccion de un bloque
 * `asm`.
 *
 * Lo que un bloque `asm` dice de si mismo alimenta al compilador entero: los
 * contratos (`pure`, `readonly`), la eliminacion de codigo muerto, el
 * planificador, el movimiento de invariantes fuera de un bucle.  Un efecto que
 * FALTA no da error -- da una funcion que escribe memoria declarada de solo
 * lectura, y con ella un llamante que se queda con el valor de antes.
 *
 * Y eso no es hipotetico: para los bloques ELEVADOS se analizaba una cadena
 * vacia, asi que todos salian inocuos.  El fallo vivio sin que nada chocara
 * porque no habia ningun sitio donde se comprobara instruccion por instruccion.
 * Este es ese sitio.
 *
 * ## Que se comprueba de cada instruccion
 *
 * El efecto COMPLETO, no solo si toca memoria: que operandos escribe, si
 * escribe registros que no nombra, si lee o escribe memoria, si lee o escribe
 * las banderas, si se va del bloque.  Un informe que solo mirara memoria diria
 * que un `mov rax, rbx` no tiene ningun efecto, y un `mov` que no escribe nada
 * es exactamente el error que hace que su destino se de por invariante.
 *
 * Los sentidos van SEPARADOS a proposito -- leer no es escribir, ni en memoria
 * ni en banderas --.  Un bloque que solo LEE las banderas (`setz al`) depende
 * de la comparacion de antes pero no destruye nada; uno que solo las ESCRIBE
 * (`cmp`) destruye pero no depende.  Con un bit unico los dos salen iguales y
 * hay que suponer lo peor de ambos, que es no mover nada en ninguna direccion.
 *
 * ## Como se lee un caso
 *
 * Se declaran los efectos que la instruccion DEBE tener, por nombre.  Lo que no
 * se nombra tiene que estar AUSENTE -- eso es lo que convierte el test en algo
 * util: un efecto de mas convierte un `asm` inocente en una barrera para todo
 * lo que le rodea, y uno de menos deja pasar una optimizacion que rompe.
 *
 * Cuando de verdad no se puede afirmar un campo, se dice con @ref UNCHECKED en
 * vez de poner el valor que salga.  Un test que se ajusta a lo que el codigo
 * hace hoy no comprueba nada.
 */

#include "vx/asm/asm_analyze.h"

/* Relativo a proposito: ver la nota en `test_asm_cobertura.cpp`. */
#include "../util/test_report.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

/// Los efectos que una instruccion puede declarar, por nombre.
enum Effect : uint32_t {
    NONE = 0,
    WRITES_OP0 = 1u << 0, ///< deja su resultado en el 1er operando
    WRITES_OP1 = 1u << 1, ///< y en el 2o (xchg escribe los dos)
    WRITES_REG =
        1u << 2,         ///< escribe un registro que NO nombra (rdtsc: rax:rdx)
    READS_MEM = 1u << 3, ///< lee memoria
    WRITES_MEM = 1u << 4,   ///< la escribe
    READS_FLAGS = 1u << 5,  ///< CONSUME las banderas (adc, setcc, cmovcc)
    WRITES_FLAGS = 1u << 6, ///< las modifica: quien las tuviera las pierde
    CALL = 1u << 7,         ///< transfiere el control fuera del bloque
    /// ORDENA lo de alrededor.  Lo ponen las barreras (`mfence`, `dmb`) y
    /// tambien los accesos ATOMICOS (`lock inc`, `ldar`, `casal`): la
    /// consecuencia es la misma -- nada los cruza -- y por eso es un solo
    /// efecto.  Que ademas toquen memoria se dice aparte.
    ORDERS = 1u << 8,
    PORT_IO = 1u << 9, ///< entrada/salida por puerto: se ve desde fuera
};

/// Un caso: la instruccion y los efectos que tiene que declarar.
struct Case {
    const char *text;   ///< la instruccion, con marcadores `$N`
    uint32_t has;       ///< efectos que DEBE declarar
    uint32_t unchecked; ///< los que no se comprueban, y por que en @ref why
    const char *why;    ///< sale en el mensaje de fallo
};

/// Atajo para el campo que casi siempre esta vacio.
constexpr uint32_t UNCHECKED = 0;

tests::Tally tally;

/// Nombres de los efectos presentes en @p e, para el mensaje de fallo.
std::string names_of(uint32_t e) {
    struct N {
        uint32_t bit;
        const char *name;
    };
    static const N kNames[] = {
        {WRITES_OP0, "write op0"},
        {WRITES_OP1, "write op1"},
        {WRITES_REG, "write reg"},
        {READS_MEM, "read mem"},
        {WRITES_MEM, "write mem"},
        {READS_FLAGS, "read flags"},
        {WRITES_FLAGS, "write flags"},
        {CALL, "call"},
        {ORDERS, "orders"},
        {PORT_IO, "port io"},
    };
    std::string s;
    for (const N &n : kNames)
        if ((e & n.bit) != 0u) {
            if (!s.empty()) s += ", ";
            s += n.name;
        }
    return s.empty() ? "(nada)" : s;
}

/**
 * @brief El efecto de un bloque con NOMBRES: que operandos y que registros.
 *
 * Un `write reg` a secas no sirve para leer el informe: lo que hace falta saber
 * es CUAL.  `imul rax, rbx` escribe su destino y nada mas, pero `imul rbx`
 * escribe `rax` y `rdx` sin nombrarlos en el texto -- y quien tuviera algo en
 * `rdx` lo pierde --.  Con el nombre delante, la diferencia se ve; con una
 * bandera, las dos lineas son identicas.
 *
 * Lo mismo la memoria: se dice POR DONDE se llega cuando se sabe (`por $1`), y
 * se dice que no se sabe cuando no -- que es un dato distinto de no tocarla --.
 */
std::string describe(const vx::AsmBlockEffects &r) {
    std::string s;
    auto add = [&s](const std::string &t) {
        if (!s.empty()) s += " | ";
        s += t;
    };
    /* Los operandos que escribe y los registros que escribe SIN nombrarlos van
     * separados: el primero es una variable del programa que cambia de valor,
     * el segundo un registro que quien llame no puede suponer intacto. */
    std::string ops, regs;
    for (const std::string &w : r.escritos) {
        std::string &dst = (!w.empty() && w[0] == '$') ? ops : regs;
        if (!dst.empty()) dst += ", ";
        dst += w;
    }
    if (!ops.empty()) add("write " + ops);
    if (!regs.empty()) add("write reg " + regs);
    /* Por donde se llega a la memoria.  Se agrupa por sentido y se nombra la
     * base de cada acceso; los que no se pudieron atribuir salen dichos, no
     * callados. */
    std::string mem_r, mem_w;
    for (const vx::AsmBlockEffects::Acceso &a : r.accesos) {
        std::string &dst = a.escribe ? mem_w : mem_r;
        if (dst.find(a.base) != std::string::npos) continue;
        if (!dst.empty()) dst += ", ";
        dst += a.base;
    }
    if (r.reads_mem) add(mem_r.empty() ? "read mem" : "read mem por " + mem_r);
    if (r.writes_mem)
        add(mem_w.empty() ? "write mem" : "write mem por " + mem_w);
    if (r.accesos_incompletos) add("mem sin atribuir");
    if (r.reads_flags) add("read flags");
    if (r.writes_flags) add("write flags");
    if (r.is_call) add("call");
    if (r.has_atomic) add("orders");
    if (r.has_port_io) add("port io");
    return s.empty() ? "(nada)" : s;
}

/// Clases de los marcadores.  Sin ellas, un operando que el compilador nombro
/// `$1` no tiene ancho y el analisis no puede acotar cuantos bytes toca.
std::vector<std::pair<std::string, std::string>>
operand_classes(const char *c1) {
    /* `$0` es siempre un registro general: es el que lleva la DIRECCION en los
     * casos de memoria (`[$0]`), y una direccion no vive en el banco ancho. */
    return {{"$0", "reg"}, {"$1", c1}, {"$2", c1}};
}

void check(const Case &c, const char *operand_class, const char *arch) {
    const vx::AsmBlockEffects r = vx::asm_analyze_block(
        c.text, std::string(arch), operand_classes(operand_class));
    /* "No se sabe" no es "no hace nada", y hay que separarlos ANTES de comparar
     * efectos.  Una instruccion que el analisis no reconoce sale con todos los
     * campos a cero, o sea identica a un `nop`: si el caso esperaba `NONE`
     * pasaria, y estaria dando por comprobado justo lo que no se comprobo.
     *
     * Sale como PENDIENTE y no como fallo porque es trabajo identificado -- la
     * ISA sin tabla propia, la forma que la base no modela -- y un rojo
     * permanente se acaba ignorando. */
    if (!r.known()) {
        std::string motivo = "sin modelar: ";
        for (size_t k = 0; k < r.unknown_mnemonics.size(); ++k) {
            if (k != 0) motivo += ", ";
            motivo += r.unknown_mnemonics[k];
        }
        motivo += " -- ni la tabla de esta arquitectura ni la forma de la base";
        tests::pending(tally, c.text, motivo.c_str());
        return;
    }
    uint32_t actual = NONE;
    /* Que operandos escribe.  Un marcador `$N` sale tal cual en `escritos`; un
     * nombre de registro es un efecto IMPLICITO -- la instruccion lo escribe
     * sin nombrarlo, como el rax:rdx de una `rdtsc` --, y son cosas distintas:
     * el primero es una variable del programa que cambia de valor, el segundo
     * un registro que quien llame no puede suponer intacto. */
    for (const std::string &w : r.escritos) {
        if (w == "$0")
            actual |= WRITES_OP0;
        else if (w == "$1")
            actual |= WRITES_OP1;
        else if (!w.empty() && w[0] != '$')
            actual |= WRITES_REG;
    }
    if (r.reads_mem) actual |= READS_MEM;
    if (r.writes_mem) actual |= WRITES_MEM;
    if (r.reads_flags) actual |= READS_FLAGS;
    if (r.writes_flags) actual |= WRITES_FLAGS;
    if (r.is_call) actual |= CALL;
    if (r.has_atomic) actual |= ORDERS;
    if (r.has_port_io) actual |= PORT_IO;

    /* Una escritura a memoria se cuenta TAMBIEN como lectura, y es deliberado:
     * `add [rdi], rax` acumula, y el analisis prefiere no distinguir el destino
     * puro -- el `mov` -- porque equivocarse ahi deja pasar una optimizacion
     * que rompe.  Asi que un caso que espera escritura no exige que NO se lea.
     */
    uint32_t unchecked = c.unchecked;
    if ((c.has & WRITES_MEM) != 0u) unchecked |= READS_MEM;
    const uint32_t checked = ~unchecked;

    if ((actual & checked) == (c.has & checked)) {
        /* Se imprime TAMBIEN lo que esta bien, y con TODOS sus efectos -- no
         * solo los comprobados --.  Un informe que solo ensena los fallos no
         * dice que se comprobo, y entonces no se puede distinguir "todo
         * correcto" de "no se miro". */
        tests::pass(tally, c.text, describe(r).c_str());
        return;
    }
    tests::fail(tally, c.text, names_of(c.has & checked),
                names_of(actual & checked), c.why);
}

} // namespace

int main() {
    /* --- Clase general: enteros, memoria, control ------------------------- */
    const Case gp[] = {
        // Mover y copiar sin tocar memoria.  Ninguna es "sin efectos": todas
        // dejan su resultado en algun sitio, y ese sitio es el efecto.
        {"mov $0, $1", WRITES_OP0, UNCHECKED,
         "mover deja el valor en su destino"},
        {"movzx $0, $1", WRITES_OP0, UNCHECKED,
         "extender con ceros, en el destino"},
        {"movsx $0, $1", WRITES_OP0, UNCHECKED,
         "ni extender con signo toca memoria"},
        {"lea $0, [$1]", WRITES_OP0, UNCHECKED,
         "calcula una direccion, NO la sigue: escribe su destino y no lee "
         "memoria"},
        {"xchg $0, $1", WRITES_OP0 | WRITES_OP1, UNCHECKED,
         "intercambiar escribe LOS DOS operandos: con un solo bit de destino, "
         "el "
         "segundo se da por intacto"},
        {"bswap $0", WRITES_OP0, UNCHECKED, "invertir bytes de un registro"},

        // Aritmetica y logica: escriben su destino Y dejan banderas.
        {"add $0, $1", WRITES_OP0 | WRITES_FLAGS, UNCHECKED,
         "la suma deja acarreo y cero, y el resultado en su destino"},
        {"sub $0, $1", WRITES_OP0 | WRITES_FLAGS, UNCHECKED, "la resta igual"},
        {"and $0, $1", WRITES_OP0 | WRITES_FLAGS, UNCHECKED,
         "la logica tambien"},
        {"or $0, $1", WRITES_OP0 | WRITES_FLAGS, UNCHECKED, "idem"},
        {"xor $0, $0", WRITES_OP0 | WRITES_FLAGS, UNCHECKED,
         "el idioma de poner a cero"},
        {"not $0", WRITES_OP0, UNCHECKED,
         "complemento: es la que NO toca banderas"},
        {"neg $0", WRITES_OP0 | WRITES_FLAGS, UNCHECKED, "negar si las toca"},
        {"inc $0", WRITES_OP0 | WRITES_FLAGS, UNCHECKED,
         "incrementar deja cero"},
        {"dec $0", WRITES_OP0 | WRITES_FLAGS, UNCHECKED, "decrementar igual"},
        {"shl $0, 3", WRITES_OP0 | WRITES_FLAGS, UNCHECKED,
         "desplazar deja el bit que sale"},
        {"shr $0, 3", WRITES_OP0 | WRITES_FLAGS, UNCHECKED,
         "idem a la derecha"},
        {"sar $0, 3", WRITES_OP0 | WRITES_FLAGS, UNCHECKED, "idem con signo"},
        {"rol $0, 3", WRITES_OP0 | WRITES_FLAGS, UNCHECKED,
         "rotar deja acarreo"},
        /* Las DOS formas de `imul`, que es el caso donde el mnemonico no basta:
         * con dos operandos deja el producto en su destino y nada mas; con uno
         * multiplica contra el acumulador y lo deja en `rdx:rax`, sin nombrar
         * ninguno de los dos.  Si las dos se tabulan igual, una de las dos
         * miente: o `imul rax, rbx` sale destruyendo `rdx` -- y se pierde lo
         * que hubiera ahi --, o `imul rbx` sale sin tocarlo y se pierde el
         * producto. */
        {"imul $0, $1", WRITES_OP0 | WRITES_FLAGS, UNCHECKED,
         "la forma de dos operandos deja el producto SOLO en su destino"},
        {"imul $0", WRITES_REG | WRITES_FLAGS, UNCHECKED,
         "la de uno lo deja en `rdx:rax`, que no aparecen en el texto"},
        {"mul $0", WRITES_REG | WRITES_FLAGS, UNCHECKED,
         "el producto sin signo, igual"},
        {"div $0", WRITES_REG | WRITES_FLAGS, UNCHECKED,
         "dividir deja cociente y resto en `rax` y `rdx`"},
        {"idiv $0", WRITES_REG | WRITES_FLAGS, UNCHECKED, "con signo, igual"},
        {"cqo", WRITES_REG, UNCHECKED,
         "extender el signo a `rdx` es lo que se hace ANTES de dividir, y su "
         "efecto entero es un registro que no nombra"},
        {"popcnt $0, $1", WRITES_OP0 | WRITES_FLAGS, UNCHECKED,
         "contar unos deja cero"},

        /* Las que existen PARA dejar banderas y no escriben ningun operando. Es
         * la prueba de que el destino y las banderas son efectos separados: si
         * `cmp` saliera escribiendo su primer operando, el valor que compara se
         * daria por destruido. */
        {"cmp $0, $1", WRITES_FLAGS, UNCHECKED,
         "comparar existe PARA dejar banderas, y NO escribe lo que compara"},
        {"test $0, $1", WRITES_FLAGS, UNCHECKED, "y probar bits, igual"},
        {"bt $0, 3", WRITES_FLAGS, UNCHECKED,
         "probar un bit lo deja en el acarreo sin tocar el operando"},

        /* Las que CONSUMEN banderas.  Aqui se ve por que los dos sentidos no se
         * pueden colapsar: un `setz` LEE lo que un `cmp` de antes produjo, asi
         * que no se pueden separar; pero no destruye nada, asi que lo que venga
         * despues si puede pasar por encima. */
        {"setz $0", WRITES_OP0 | READS_FLAGS, UNCHECKED,
         "poner segun bandera LEE las banderas y escribe su operando; "
         "declararla "
         "como que las escribe la hace pasar por destruir un valor que no "
         "toca"},
        {"cmovz $0, $1", WRITES_OP0 | READS_FLAGS, UNCHECKED,
         "mover condicional las lee para decidir"},
        {"adc $0, $1", WRITES_OP0 | READS_FLAGS | WRITES_FLAGS, UNCHECKED,
         "sumar con acarreo lee el acarreo Y lo vuelve a escribir: sin la "
         "lectura, "
         "la suma que lo produjo se puede mover por debajo"},
        {"sbb $0, $1", WRITES_OP0 | READS_FLAGS | WRITES_FLAGS, UNCHECKED,
         "restar con prestamo, igual"},
        {"rcl $0, 1", WRITES_OP0 | READS_FLAGS | WRITES_FLAGS, UNCHECKED,
         "rotar A TRAVES del acarreo lo lee y lo escribe"},
        {"cmc", READS_FLAGS | WRITES_FLAGS, UNCHECKED,
         "complementar el acarreo lo lee y lo escribe, sin operandos"},
        {"stc", WRITES_FLAGS, UNCHECKED, "ponerlo a uno solo lo escribe"},
        {"clc", WRITES_FLAGS, UNCHECKED, "y ponerlo a cero"},

        // No hacen nada observable.
        {"nop", NONE, UNCHECKED, "no hacer nada hay que poder decirlo"},
        {"pause", NONE, UNCHECKED, "un hint de espera no tiene efecto"},

        // Barreras: ordenan, no mueven datos.
        {"mfence", ORDERS, UNCHECKED,
         "una barrera no lee ni escribe, pero ORDENA: tratarla como una "
         "instruccion cualquiera permite justo lo que existe para impedir"},
        {"sfence", ORDERS, UNCHECKED, "barrera de escrituras"},
        {"lfence", ORDERS, UNCHECKED, "barrera de lecturas"},

        // Memoria explicita, por corchetes.
        {"mov $0, [$1]", WRITES_OP0 | READS_MEM, UNCHECKED,
         "cargar lee memoria y escribe su destino"},
        {"mov [$0], $1", WRITES_MEM, UNCHECKED,
         "guardar escribe memoria y NO escribe el registro de la direccion"},
        {"movzx $0, [$1]", WRITES_OP0 | READS_MEM, UNCHECKED,
         "cargar extendiendo lee"},
        {"add [$0], $1", READS_MEM | WRITES_MEM | WRITES_FLAGS, UNCHECKED,
         "acumular en memoria lee, escribe y deja banderas"},
        {"inc [$0]", READS_MEM | WRITES_MEM | WRITES_FLAGS, UNCHECKED,
         "incrementar en sitio hace las tres"},
        {"xchg [$0], $1", READS_MEM | WRITES_MEM | WRITES_OP1, UNCHECKED,
         "intercambiar con memoria lee, escribe, y ademas cambia el registro"},
        {"cmp $0, [$1]", READS_MEM | WRITES_FLAGS, UNCHECKED,
         "comparar contra memoria lee, no escribe"},

        // Atomicas: el prefijo no cambia QUE se toca.
        {"lock inc [$0]", READS_MEM | WRITES_MEM | WRITES_FLAGS | ORDERS,
         UNCHECKED, "`lock` da atomicidad y ordena; no cambia que se toca"},
        {"lock xadd [$0], $1", READS_MEM | WRITES_MEM | WRITES_FLAGS | ORDERS,
         UNCHECKED, "sumar e intercambiar atomico"},
        {"lock cmpxchg [$0], $1",
         READS_MEM | WRITES_MEM | WRITES_FLAGS | WRITES_REG | ORDERS, UNCHECKED,
         "comparar e intercambiar: la base de todo lo atomico.  Y escribe "
         "`rax` "
         "sin nombrarlo -- ahi deja lo que encontro cuando falla --, que es "
         "justo el dato con el que se decide si reintentar"},

        /* Pila: tocan memoria SIN corchetes a la vista, y ademas mueven `rsp`.
         * Las dos cosas hay que decirlas: quien crea que `rsp` sigue donde
         * estaba calcula mal cualquier direccion que salga de el. */
        {"push $0", WRITES_MEM | WRITES_REG, READS_MEM,
         "apilar escribe memoria aunque no lleve corchetes, y mueve `rsp`; si "
         "ademas se cuenta como lectura no es lo que se comprueba aqui"},
        {"pop $0", WRITES_OP0 | WRITES_REG | READS_MEM, WRITES_MEM,
         "desapilar lee memoria, escribe su destino y mueve `rsp`"},
        {"pushf", WRITES_MEM | WRITES_REG | READS_FLAGS, READS_MEM,
         "apilar las banderas las LEE: es la unica forma de guardarlas"},
        {"popf", WRITES_FLAGS | WRITES_REG | READS_MEM | READS_FLAGS,
         WRITES_MEM, "y recuperarlas las escribe"},

        // Control: se va del bloque.
        {"call $0", CALL,
         READS_MEM | WRITES_MEM | READS_FLAGS | WRITES_FLAGS | WRITES_REG,
         "una llamada puede hacer cualquier cosa; lo que importa es que se "
         "declare COMO llamada"},
        {"syscall", CALL | WRITES_REG,
         READS_MEM | WRITES_MEM | READS_FLAGS | WRITES_FLAGS,
         "entrar al sistema es una llamada, y ademas escribe registros que no "
         "nombra (rax/rcx/r11)"},

        // Cadena: repiten segun un contador, y acceden por registros que no
        // aparecen en el texto.
        {"rep stosb", WRITES_MEM | READS_FLAGS, READS_MEM | WRITES_REG,
         "rellenar escribe memoria por `rdi`, sin un corchete a la vista"},
        {"rep movsb", READS_MEM | WRITES_MEM | READS_FLAGS, WRITES_REG,
         "copiar lee por `rsi` y escribe por `rdi`"},
        {"rep scasb", READS_MEM | WRITES_FLAGS | READS_FLAGS, WRITES_REG,
         "buscar lee y compara"},

        // Entrada/salida por PUERTO: no es memoria, pero se ve desde fuera.
        {"in $0, 96", WRITES_OP0 | PORT_IO, READS_FLAGS,
         "leer un puerto no es memoria, pero NO es pura: sin declararlo, se "
         "puede borrar por no hacer nada"},
        {"out 96, $0", PORT_IO, READS_FLAGS, "y escribirlo tampoco es memoria"},

        /* Ramas.  Un salto CONDICIONAL lee las banderas y es de lo que depende;
         * uno incondicional no mira nada.  Y `loop` mira un REGISTRO -- `rcx`
         * --, que es un dato distinto: confundirlos deja la comparacion que
         * decide sin nadie que la use, o sea eliminable. */
        {"jmp destino", NONE, UNCHECKED, "un salto incondicional no mira nada"},
        {"je destino", READS_FLAGS, UNCHECKED,
         "el salto por igualdad LEE las banderas"},
        {"jne destino", READS_FLAGS, UNCHECKED, "y el de desigualdad"},
        {"jb destino", READS_FLAGS, UNCHECKED, "y los de orden sin signo"},
        {"jle destino", READS_FLAGS, UNCHECKED, "y los de orden con signo"},

        // Bits sueltos: modifican y dejan el anterior en el acarreo.
        {"bts $0, 3", WRITES_OP0 | WRITES_FLAGS, UNCHECKED,
         "poner un bit lo modifica Y deja el anterior en el acarreo"},
        {"btr $0, 3", WRITES_OP0 | WRITES_FLAGS, UNCHECKED, "quitarlo, igual"},
        {"btc $0, 3", WRITES_OP0 | WRITES_FLAGS, UNCHECKED, "complementarlo"},
        {"bsf $0, $1", WRITES_OP0 | WRITES_FLAGS, UNCHECKED,
         "buscar el primer bit deja cero si no habia ninguno"},
        {"lzcnt $0, $1", WRITES_OP0 | WRITES_FLAGS, UNCHECKED,
         "contar ceros a la izquierda"},
        {"tzcnt $0, $1", WRITES_OP0 | WRITES_FLAGS, UNCHECKED,
         "y a la derecha"},

        /* Manipulacion de bits sin banderas (BMI2).  Es su rasgo distintivo
         * frente a la aritmetica: NO tocan las banderas, y eso permite mover
         * una comparacion a traves de ellas. */
        {"pext $0, $1, $2", WRITES_OP0, UNCHECKED,
         "extraer bits por mascara NO toca banderas"},
        {"pdep $0, $1, $2", WRITES_OP0, UNCHECKED, "ni depositarlos"},
        {"andn $0, $1, $2", WRITES_OP0 | WRITES_FLAGS, UNCHECKED,
         "y con el negado si las toca"},
        {"bzhi $0, $1, $2", WRITES_OP0 | WRITES_FLAGS, UNCHECKED,
         "poner a cero desde un bit"},
        {"shlx $0, $1, $2", WRITES_OP0, UNCHECKED,
         "desplazar sin banderas: es para lo que existe frente a `shl`"},
        {"mulx $0, $1, $2", WRITES_OP0 | WRITES_OP1, UNCHECKED,
         "multiplicar sin banderas y con destino EXPLICITO: escribe los dos "
         "primeros operandos, que es lo que la separa de `mul`"},

        /* Memoria como destino de una operacion que la lee y la escribe.  Es el
         * patron de cualquier contador en memoria. */
        {"not [$0]", READS_MEM | WRITES_MEM, UNCHECKED,
         "complementar en sitio lee y escribe, y es la que NO toca banderas"},
        {"neg [$0]", READS_MEM | WRITES_MEM | WRITES_FLAGS, UNCHECKED,
         "negar en sitio si las toca"},
        {"shl [$0], 1", READS_MEM | WRITES_MEM | WRITES_FLAGS, UNCHECKED,
         "desplazar en sitio"},
        {"bts [$0], 3", READS_MEM | WRITES_MEM | WRITES_FLAGS, UNCHECKED,
         "poner un bit en memoria"},
        {"mov [$0], 5", WRITES_MEM, UNCHECKED,
         "guardar un inmediato escribe memoria y no lee ningun registro "
         "fuente"},
        {"lea $0, [$1 + $2*8 + 16]", WRITES_OP0, UNCHECKED,
         "una direccion COMPUESTA sigue siendo aritmetica: no lee memoria por "
         "muchos terminos que tenga"},

        // Marco de pila con instruccion dedicada.
        {"leave", WRITES_REG | READS_MEM, WRITES_MEM,
         "deshacer el marco lee la pila y mueve `rsp`/`rbp`"},

        /* Trampas y parada.  No mueven datos, pero ceden el control: tratarlas
         * como que no hacen nada permite borrarlas. */
        {"int3", NONE, READS_FLAGS | WRITES_FLAGS,
         "una trampa de depuracion no toca datos"},
        {"ud2", NONE, UNCHECKED, "ni una instruccion invalida deliberada"},
        {"hlt", NONE, UNCHECKED, "ni parar el procesador"},

        // Cadena en otros anchos: el sufijo dice cuanto mueve cada paso.
        {"rep stosq", WRITES_MEM | READS_FLAGS, READS_MEM | WRITES_REG,
         "rellenar de ocho en ocho, por `rdi`"},
        {"rep movsq", READS_MEM | WRITES_MEM | READS_FLAGS, WRITES_REG,
         "copiar de ocho en ocho"},
        {"rep cmpsb", READS_MEM | WRITES_FLAGS | READS_FLAGS, WRITES_REG,
         "comparar dos bloques lee los dos y deja banderas"},

        // Lectura de estado del procesador: escriben registros que no nombran.
        {"rdtsc", WRITES_REG, UNCHECKED,
         "leer el contador escribe rax:rdx sin nombrarlos; quien crea que "
         "siguen "
         "intactos se equivoca"},
        {"cpuid", WRITES_REG, UNCHECKED,
         "consultar el procesador escribe los cuatro"},
    };

    /* --- Banco ancho: SIMD, con y sin exigencia de alineacion ------------- */
    const Case vec[] = {
        // Sin memoria: entre registros del banco ancho.
        {"pxor $1, $2", WRITES_OP1, UNCHECKED,
         "entre registros anchos no hay memoria"},
        {"vpxor $1, $1, $2", WRITES_OP1, UNCHECKED, "la forma AVX tampoco"},
        {"paddd $1, $2", WRITES_OP1, UNCHECKED,
         "sumar empaquetado escribe su destino y NO toca banderas: eso es lo "
         "que "
         "permite mover una comparacion a traves de el"},
        {"pand $1, $2", WRITES_OP1, UNCHECKED, "logica empaquetada"},
        {"punpcklqdq $1, $2", WRITES_OP1, UNCHECKED,
         "reordenar dentro del banco"},
        {"pshufd $1, $2, 0", WRITES_OP1, UNCHECKED,
         "permutar tampoco toca memoria"},
        {"vzeroupper", NONE, UNCHECKED,
         "limpiar la parte alta no escribe ningun operando ni toca memoria"},

        /* ALINEADAS.  El efecto es el mismo que en la forma no alineada -- lo
         * que las distingue es lo que EXIGEN --, y ahi estuvo el error
         * contrario: declararlas como que tocan memoria SIEMPRE hacia que un
         * `movdqa xmm0, xmm1`, una copia entre registros, saliera leyendo y
         * escribiendo memoria, o sea que cada movimiento vectorial era una
         * barrera. */
        {"movdqa $1, $2", WRITES_OP1, UNCHECKED,
         "entre registros NO toca memoria: no hay un corchete a la vista"},
        {"movdqa [$0], $1", WRITES_MEM, UNCHECKED,
         "la forma ALINEADA de guardar SI toca memoria, por su operando"},
        {"movdqa $1, [$0]", WRITES_OP1 | READS_MEM, UNCHECKED,
         "y su lectura, lee"},
        {"movaps [$0], $1", WRITES_MEM, UNCHECKED, "igual con flotantes"},
        {"movapd [$0], $1", WRITES_MEM, UNCHECKED, "igual en doble precision"},
        {"vmovdqa [$0], $1", WRITES_MEM, UNCHECKED, "igual la forma AVX"},
        {"vmovdqa64 [$0], $1", WRITES_MEM, UNCHECKED, "igual la de 512 bits"},
        {"vmovaps [$0], $1", WRITES_MEM, UNCHECKED, "igual en flotante AVX"},

        // NO TEMPORALES: se saltan la cache, no la memoria.
        {"movntdq [$0], $1", WRITES_MEM, UNCHECKED,
         "no temporal: evita la cache, sigue escribiendo memoria"},
        {"vmovntdq [$0], $1", WRITES_MEM, UNCHECKED, "igual en AVX"},
        {"vmovntdqa $1, [$0]", WRITES_OP1 | READS_MEM, UNCHECKED,
         "su lectura, lee"},

        // SIN exigencia de alineacion: mismos efectos, otra exigencia.
        {"movdqu [$0], $1", WRITES_MEM, UNCHECKED,
         "la no alineada escribe igual"},
        {"movdqu $1, [$0]", WRITES_OP1 | READS_MEM, UNCHECKED, "y lee igual"},
        {"vmovdqu [$0], $1", WRITES_MEM, UNCHECKED, "idem en AVX"},
        {"vmovdqu $1, [$0]", WRITES_OP1 | READS_MEM, UNCHECKED, "idem"},
        {"vmovdqu64 [$0], $1", WRITES_MEM, UNCHECKED, "idem a 512 bits"},

        // Escalares entre bancos: tocan pocos bytes, sin exigir alineacion.
        {"movq $1, [$0]", WRITES_OP1 | READS_MEM, UNCHECKED,
         "mover ocho bytes desde memoria"},
        {"movq [$0], $1", WRITES_MEM, UNCHECKED, "y hacia memoria"},
        {"movd $1, [$0]", WRITES_OP1 | READS_MEM, UNCHECKED, "cuatro bytes"},
        {"movss [$0], $1", WRITES_MEM, UNCHECKED, "un flotante simple"},
        {"movsd [$0], $1", WRITES_MEM, UNCHECKED,
         "uno doble: y `movsd` con operandos es la de SSE, no la de CADENA que "
         "se llama igual"},

        // Difusiones: leen un valor y lo replican.
        {"vpbroadcastq $1, [$0]", WRITES_OP1 | READS_MEM, UNCHECKED,
         "difundir desde memoria lee"},
        {"vbroadcastss $1, [$0]", WRITES_OP1 | READS_MEM, UNCHECKED,
         "idem en flotante"},

        // Aritmetica empaquetada CON memoria.
        {"paddd $1, [$0]", WRITES_OP1 | READS_MEM, UNCHECKED,
         "sumar contra memoria lee: sin declararlo, una escritura ajena se "
         "puede "
         "mover por encima"},
        {"vaddps $1, $1, [$0]", WRITES_OP1 | READS_MEM, UNCHECKED,
         "idem en AVX flotante"},
    };

    /* --- arm64 ------------------------------------------------------------
     *
     * La otra arquitectura no es un extra: es lo que impide que el modelo se
     * escriba a la medida de x86.  Y tiene sus propias preguntas -- las
     * banderas las pone el sufijo `s` y no el mnemonico, el destino de un
     * almacen es su operando de memoria y no el primero, la direccion de
     * retorno vive en un REGISTRO y no en la pila --, asi que un efecto copiado
     * del otro lado sale mal por motivos distintos.
     *
     * Los operandos van en la sintaxis de arm: `[$0]`, `[$0, #8]`. */
    const Case a64[] = {
        // Mover y aritmetica: el sufijo `s` es lo que decide las banderas.
        {"mov $0, $1", WRITES_OP0, UNCHECKED,
         "mover deja el valor en su destino"},
        {"add $0, $0, $1", WRITES_OP0, UNCHECKED,
         "sumar NO toca banderas en arm: hace falta pedirlo con el sufijo"},
        {"adds $0, $0, $1", WRITES_OP0 | WRITES_FLAGS, UNCHECKED,
         "y con el sufijo `s` si: una letra separa las dos, y confundirlas "
         "deja "
         "una comparacion decidiendo con banderas de otra operacion"},
        {"sub $0, $0, $1", WRITES_OP0, UNCHECKED, "restar tampoco"},
        {"subs $0, $0, $1", WRITES_OP0 | WRITES_FLAGS, UNCHECKED,
         "su forma con `s`"},
        {"mul $0, $1, $2", WRITES_OP0, UNCHECKED,
         "multiplicar en arm deja el producto en su destino: no hay acumulador "
         "implicito que destruir, al contrario que en x86"},
        {"madd $0, $1, $2, $0", WRITES_OP0, UNCHECKED, "multiplicar y sumar"},
        {"udiv $0, $1, $2", WRITES_OP0, UNCHECKED, "dividir sin signo"},
        {"neg $0, $1", WRITES_OP0, UNCHECKED, "negar sin banderas"},
        {"negs $0, $1", WRITES_OP0 | WRITES_FLAGS, UNCHECKED, "y con ellas"},
        {"lsl $0, $1, 3", WRITES_OP0, UNCHECKED, "desplazar no deja banderas"},
        {"sxtw $0, $1", WRITES_OP0, UNCHECKED, "extender el signo de 32 a 64"},
        {"clz $0, $1", WRITES_OP0, UNCHECKED, "contar ceros a la izquierda"},
        {"rbit $0, $1", WRITES_OP0, UNCHECKED, "invertir el orden de los bits"},
        {"ubfx $0, $1, 4, 8", WRITES_OP0, UNCHECKED,
         "extraer un campo de bits"},
        {"adr $0, etiqueta", WRITES_OP0, UNCHECKED,
         "calcular una direccion NO es leerla, igual que el `lea` de x86"},

        // Comparar: solo banderas, sin escribir el registro comparado.
        {"cmp $0, $1", WRITES_FLAGS, UNCHECKED,
         "comparar existe PARA dejar banderas y no escribe lo que compara"},
        {"tst $0, $1", WRITES_FLAGS, UNCHECKED, "probar bits, igual"},
        {"cmn $0, $1", WRITES_FLAGS, UNCHECKED, "comparar con el negado"},

        /* Seleccion condicional: CONSUMEN las banderas.  Es el mismo caso que
         * `setcc`/`cmovcc` en x86, y aqui tampoco estaba declarado: sin la
         * lectura, el bloque se puede mover por encima del `cmp` que produjo la
         * bandera con la que decide. */
        {"cset $0, eq", WRITES_OP0 | READS_FLAGS, UNCHECKED,
         "convertir una bandera en un valor la LEE"},
        {"csel $0, $1, $2, eq", WRITES_OP0 | READS_FLAGS, UNCHECKED,
         "elegir entre dos las lee para decidir"},
        {"csinc $0, $1, $2, ne", WRITES_OP0 | READS_FLAGS, UNCHECKED, "idem"},
        {"cinc $0, $1, lt", WRITES_OP0 | READS_FLAGS, UNCHECKED, "idem"},
        {"ccmp $0, $1, 0, eq", READS_FLAGS | WRITES_FLAGS, UNCHECKED,
         "comparar CONDICIONALMENTE las lee para decidir si compara y las "
         "escribe "
         "con el resultado: es el unico caso que hace las dos cosas"},

        // Cargas y almacenes: la memoria SIEMPRE lleva sus corchetes en arm.
        {"ldr $0, [$1]", WRITES_OP0 | READS_MEM, UNCHECKED,
         "cargar lee memoria y escribe su destino"},
        {"ldr $0, [$1, 8]", WRITES_OP0 | READS_MEM, UNCHECKED,
         "y con desplazamiento, igual"},
        {"ldrb $0, [$1]", WRITES_OP0 | READS_MEM, UNCHECKED, "un byte"},
        {"ldrsw $0, [$1]", WRITES_OP0 | READS_MEM, UNCHECKED,
         "cuatro bytes con signo"},
        {"str $0, [$1]", WRITES_MEM, UNCHECKED,
         "guardar escribe MEMORIA y no el registro que guarda: el operando "
         "escrito "
         "es el de los corchetes, que en arm nunca es el primero"},
        {"strb $0, [$1]", WRITES_MEM, UNCHECKED, "un byte"},
        {"ldp $0, $1, [$2]", WRITES_OP0 | WRITES_OP1 | READS_MEM, UNCHECKED,
         "cargar un PAR escribe los dos destinos: con una sola marca de "
         "destino, "
         "el segundo se da por intacto"},
        {"stp $0, $1, [$2]", WRITES_MEM, UNCHECKED,
         "y guardar un par escribe memoria"},

        // Atomicas: carga con reserva, almacen condicional, y las CAS.
        {"ldxr $0, [$1]", WRITES_OP0 | READS_MEM | ORDERS, UNCHECKED,
         "cargar con reserva lee memoria"},
        {"ldar $0, [$1]", WRITES_OP0 | READS_MEM | ORDERS, UNCHECKED,
         "cargar con adquisicion, igual"},
        {"stxr $0, $1, [$2]", WRITES_OP0 | WRITES_MEM | ORDERS, UNCHECKED,
         "el almacen CONDICIONAL escribe memoria y ademas deja en su primer "
         "operando si lo consiguio -- ese es el dato con el que se reintenta"},
        {"stlr $0, [$1]", WRITES_MEM | ORDERS, UNCHECKED,
         "el almacen con LIBERACION no tiene registro de estado: escribe "
         "memoria "
         "y nada mas.  Tabularlo igual que `stlxr` le atribuia una escritura "
         "al "
         "registro que guarda"},
        {"casal $0, $1, [$2]", WRITES_OP0 | READS_MEM | WRITES_MEM | ORDERS,
         UNCHECKED,
         "comparar e intercambiar atomico lee, escribe y deja lo que encontro"},
        {"swpal $0, $1, [$2]", WRITES_OP0 | READS_MEM | WRITES_MEM | ORDERS,
         UNCHECKED, "intercambiar atomico, igual"},
        {"ldaddal $0, $1, [$2]", WRITES_OP0 | READS_MEM | WRITES_MEM | ORDERS,
         UNCHECKED, "sumar atomico devuelve el valor anterior"},

        /* Barreras: ORDENAN, no mueven datos.  Estaban tabuladas como que tocan
         * memoria, y como no nombran por donde, el bloque entero quedaba con
         * memoria sin atribuir -- o sea suponiendo lo peor de toda --.  Ordenar
         * y acceder son cosas distintas, y en x86 ya se distinguian. */
        {"dmb ish", ORDERS, UNCHECKED,
         "una barrera de datos ordena, no lee ni escribe"},
        {"dsb sy", ORDERS, UNCHECKED, "la de sistema, igual"},
        {"isb", ORDERS, UNCHECKED, "y la de instrucciones"},

        /* Control.  En arm la direccion de retorno va en un REGISTRO (`x30`),
         * no en la pila: una llamada lo escribe y `ret` lo lee, y ninguna de
         * las dos toca memoria por eso.  Estaban declaradas como que si. */
        {"bl destino", CALL | WRITES_REG, UNCHECKED,
         "llamar escribe el registro de enlace; no apila nada"},
        {"blr $0", CALL | WRITES_REG, UNCHECKED, "la llamada indirecta, igual"},
        {"ret", NONE, UNCHECKED,
         "volver LEE el registro de enlace: no toca memoria, y declarar que si "
         "convierte cualquier funcion en una barrera"},
        {"b destino", NONE, UNCHECKED, "un salto no condicional no mira nada"},
        {"b.eq destino", READS_FLAGS, UNCHECKED,
         "el salto CONDICIONAL lee las banderas: es de lo que depende"},
        {"cbz $0, destino", NONE, UNCHECKED,
         "saltar si un registro es cero mira el REGISTRO, no las banderas: es "
         "la "
         "diferencia con `b.eq`"},
        {"tbnz $0, 3, destino", NONE, UNCHECKED,
         "y saltar si un bit esta puesto"},

        // Sin efecto observable, y la llamada al sistema.
        {"nop", NONE, UNCHECKED, "no hacer nada hay que poder decirlo"},
        {"yield", NONE, UNCHECKED, "ceder no tiene efecto sobre los datos"},
        {"wfe", NONE, UNCHECKED, "esperar un evento tampoco"},
        {"svc 0", CALL | WRITES_REG | WRITES_FLAGS, READS_MEM | WRITES_MEM,
         "entrar al sistema es una llamada y escribe `x0` con el resultado"},
    };

    /* --- arm32 (A32) ------------------------------------------------------
     *
     * No es "arm64 con otros nombres".  Las banderas las pide el sufijo `s`
     * igual que en A64, pero ademas CASI TODA instruccion A32 lleva condicion,
     * asi que leer las banderas es lo normal y no la excepcion.  Y no tiene
     * tabla escrita a mano: lo que se sepa sale de la base, por la ISA
     * correcta.  Hasta hace un momento se contestaba con la tabla de arm64, que
     * es peor que no contestar.
     */
    const Case a32[] = {
        {"mov $0, $1", WRITES_OP0, UNCHECKED,
         "mover deja el valor en su destino"},
        {"add $0, $1, $2", WRITES_OP0, UNCHECKED,
         "sumar sin sufijo no deja banderas"},
        {"adds $0, $1, $2", WRITES_OP0 | WRITES_FLAGS, UNCHECKED,
         "y con el sufijo `s` si"},
        {"sub $0, $1, $2", WRITES_OP0, UNCHECKED, "restar"},
        {"cmp $0, $1", WRITES_FLAGS, UNCHECKED,
         "comparar existe PARA dejar banderas"},
        {"ldr $0, [$1]", WRITES_OP0 | READS_MEM, UNCHECKED,
         "cargar lee memoria"},
        {"str $0, [$1]", WRITES_MEM, UNCHECKED, "guardar la escribe"},
        {"bl destino", CALL | WRITES_REG, UNCHECKED,
         "llamar escribe el registro de enlace, igual que en A64: la direccion "
         "de "
         "retorno va a un REGISTRO y no a la pila, asi que no toca memoria"},
        {"bx $0", NONE, UNCHECKED, "saltar a un registro no mira banderas"},
        {"dmb", ORDERS, UNCHECKED, "la barrera ordena y no mueve datos"},
    };

    /* --- RISC-V -----------------------------------------------------------
     *
     * La que mas separa el modelo de x86, y por eso es la que mejor comprueba
     * que no se le ha escrito encima: RISC-V NO TIENE registro de banderas.  Un
     * `beq` compara dos registros y salta; no hay nada que leer ni que
     * destruir.  Un modelo que diera por hecho que una rama condicional lee
     * banderas se inventaria aqui una dependencia que no existe.
     *
     * Y la memoria se escribe `0(a1)`, sin corchetes: quien busque un `[` para
     * saber si hay acceso no encuentra ninguno.
     */
    const Case rv[] = {
        {"add $0, $1, $2", WRITES_OP0, UNCHECKED, "sumar deja el resultado"},
        {"addi $0, $1, 4", WRITES_OP0, UNCHECKED, "sumar un inmediato"},
        {"sub $0, $1, $2", WRITES_OP0, UNCHECKED, "restar"},
        {"xor $0, $1, $2", WRITES_OP0, UNCHECKED, "logica"},
        {"slli $0, $1, 3", WRITES_OP0, UNCHECKED, "desplazar"},
        {"mul $0, $1, $2", WRITES_OP0, UNCHECKED, "multiplicar"},
        {"beq $0, $1, destino", NONE, UNCHECKED,
         "la rama compara DOS REGISTROS: en RISC-V no hay banderas que leer, y "
         "suponer que las lee seria inventar una dependencia"},
        {"ld $0, 0($1)", WRITES_OP0 | READS_MEM, UNCHECKED,
         "cargar lee memoria, escrita sin corchetes"},
        {"sd $0, 8($1)", WRITES_MEM, UNCHECKED, "y guardar la escribe"},
        {"fence", ORDERS, UNCHECKED, "la barrera ordena"},
        {"ecall", CALL | WRITES_REG, READS_MEM | WRITES_MEM,
         "entrar al sistema es una llamada y escribe `a0` con el resultado"},
    };

    tests::title("asm-efectos");
    tests::section("x86: clase general",
                   "enteros, memoria, banderas y control");
    for (const Case &c : gp)
        check(c, "reg", "x86_64");
    tests::section("x86: banco ancho",
                   "SIMD, con y sin exigencia de alineacion");
    for (const Case &c : vec)
        check(c, "xmm", "x86_64");
    tests::section("arm64", "otra arquitectura, otras preguntas");
    for (const Case &c : a64)
        check(c, "reg", "arm64");
    tests::section("arm32",
                   "el sufijo `s` escribe banderas; la condicion las lee");
    for (const Case &c : a32)
        check(c, "reg", "arm32");
    tests::section("riscv",
                   "sin banderas en absoluto: la rama compara registros");
    for (const Case &c : rv)
        check(c, "reg", "riscv");

    tests::summary("asm-efectos", tally);
    return tally.exit_code();
}
