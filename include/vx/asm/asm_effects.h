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
 * @file asm_effects.h
 * @brief Tabla plana por-arquitectura de efectos de instruccion + inferencia
 *        PROPIA de clobbers para inline asm.
 *
 * Tabla @c mnemonic -> efectos (registros escritos implicitamente, bitmask de
 * operandos escritos, si toca memoria/flags/es call) por arquitectura (x86 y
 * arm64) + @c asm_infer_clobbers que tokeniza un cuerpo NASM Intel e infiere la
 * lista de clobbers que GCC
 * (port-C) o el JIT (inc.5) deben declarar.  Es 100% propia: NO usa Keystone
 * ni Capstone (la inferencia debe seguir funcionando sin esas libs).  El
 * ENSAMBLADO (texto->bytes) es un eje ortogonal que vive tras la interfaz
 * @c AsmBackend; la inferencia NUNCA lo consume.
 *
 * Tambien expone @c asm_canonical_reg: canonicaliza cualquier alias de ancho
 * (eax/ax/al/ah, r8d/r8w/r8b, xmm/ymm/zmm) a su registro fisico de 64 bits
 * (rax..r15, vN para el banco SIMD).  Usado por el type checker (validacion +
 * deteccion de conflicto same-reg) y por la inferencia (excluir los registros
 * ligados por register() del set de clobbers).
 */

#ifndef VX_ASM_EFFECTS_H
#define VX_ASM_EFFECTS_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "vx/asm/instr_db.h" // vx::instr_db::Isa (la ISA del objetivo)

namespace vx {

/**
 * @brief Canonicaliza un nombre de registro x86-64 a su fisico de 64 bits.
 *
 * @param raw Nombre tal cual (case-insensitive): rax/eax/ax/al/ah,
 *            r8/r8d/r8w/r8b, xmm0/ymm0/zmm0, etc.
 * @return Nombre canonico (rax..r15 para GP; @c "vN" para el banco
 *         vectorial xmm/ymm/zmm), o cadena vacia si no se reconoce.
 */
std::string asm_canonical_reg(const std::string &raw);

/**
 * @brief Canonicaliza @p raw con los registros de @p arch, sin mirar el
 *        objetivo activo.
 *
 * La version de un solo argumento resuelve la arquitectura por el objetivo que
 * se este compilando, que es lo que quiere casi todo el mundo.  Esta hace falta
 * cuando se analiza un cuerpo de una arquitectura CONCRETA que no tiene por que
 * ser esa -- las variantes por `@Target`, o un test que comprueba las dos --:
 * ahi el arch es un dato del analisis, no del entorno.
 *
 * @param raw  Nombre tal cual (case-insensitive).
 * @param arch `"x86_64"`, `"x86"`, `"arm64"`.
 * @return Nombre canonico, o cadena vacia si no se reconoce en esa
 *         arquitectura.
 */
std::string asm_canonical_reg(const std::string &raw, const std::string &arch);

/**
 * @brief Arquitectura del OBJETIVO que se esta compilando.
 *
 * Es la pregunta previa a analizar cualquier bloque de asm -- que mnemonicos
 * existen, como se llaman los registros, cuanto miden --, y tiene UNA respuesta:
 * el objetivo, no el host.  Una variante @c @Target("arch:arm64") se analiza con
 * los registros de ARM aunque el build corra en x86.
 *
 * Vive aqui porque la preguntan varios -- el modelo de efectos, el eliminador de
 * escrituras muertas, la inferencia de clobbers -- y cada uno la respondia por su
 * cuenta: uno la resolvia bien, otro tenia su propia copia, y otro la llevaba
 * escrita a mano como @c "x86_64".  El resultado era que analizar el MISMO
 * bloque daba cosas distintas segun quien preguntara.
 *
 * @return @c "x86_64", @c "x86", @c "arm64"...
 */
std::string asm_arch_actual();

/**
 * @brief La ISA del objetivo activo, para preguntarle a la base de
 *        instrucciones.
 *
 * Sale del MISMO sitio que los registros -- el objetivo que se compila, no la
 * maquina donde corre el compilador --, y estaba definida sin declarar, asi que
 * cada consumidor nuevo se veia obligado a repetir la traduccion de nombre de
 * arquitectura a ISA.  Dos copias de esa tabla son dos respuestas distintas en
 * cuanto una se quede atras.
 */
/// La ISA de un nombre de arquitectura cualquiera (no solo el objetivo activo).
instr_db::Isa isa_de_arch(const std::string &arch);

/**
 * @brief La ISA de un nombre de arquitectura cualquiera, no solo del objetivo.
 *
 * Quien analiza un bloque recibe SU arquitectura, no la activa, asi que necesita
 * poder traducirla sin repetir la correspondencia: dos copias de la misma tabla
 * se separan, y el sintoma seria leer la base de instrucciones de otra ISA.
 */
instr_db::Isa isa_de_arch(const std::string &arch);

instr_db::Isa isa_actual();

/**
 * @brief Registro BASE de un operando de memoria, en forma canonica.
 *
 * Es el primer identificador dentro de los corchetes, y eso vale igual en las
 * dos sintaxis: `[rdi]`, `[rbx + rcx*8]` en x86 y `[x0]`, `[x0, #8]`,
 * `[x0, x1, lsl #3]` en arm64.  Un marcador `$N` se devuelve TAL CUAL: el
 * registro aun no esta elegido -- lo elige el asignador despues -- pero el
 * marcador ya identifica de que operando se trata, que es lo que hace falta,
 * y ademas no depende de nombres de registro ni puede confundirse con otra
 * variable que use el mismo.
 *
 * Vive aqui, y no en quien analiza el bloque, porque lo preguntan DOS: quien
 * quiere saber que memoria toca el bloque y quien quiere saber si se cumple lo
 * que la instruccion EXIGE.  Son la misma pregunta sobre el mismo acceso.
 *
 * @param operando Texto del operando, con sus corchetes.
 * @param arch     Arquitectura del cuerpo que se analiza.  EXPLICITA a
 *                 proposito: el cuerpo puede ser de una arquitectura distinta
 *                 de la que se este compilando (una variante por `@Target`).
 * @return El registro canonico o el marcador; cadena vacia si no se pudo
 *         determinar (direccion absoluta, simbolo, expresion no reconocida).
 */
std::string asm_base_de_memoria(const std::string &operando,
                                const std::string &arch);

/**
 * @struct AsmMemOperando
 * @brief Un operando de memoria, leido: por donde se llega y a que distancia.
 */
struct AsmMemOperando {
    /// Registro base en forma canonica, o el marcador @c "$N".  Vacio = no se
    /// pudo determinar (direccion absoluta, simbolo, expresion no reconocida).
    std::string base;
    /// Parte CONSTANTE de la distancia desde la base, en bytes.
    int64_t desplazamiento = 0;
    /**
     * Operando que aporta el RESTO de la distancia, o vacio si no lo hay.
     *
     * `[rbx + rcx*8]` no es un acceso sin distancia conocida: es un acceso a
     * `rcx*8` de `rbx`.  Nombrarlo es lo que permite a quien tenga las
     * ligaduras acotarlo por el rango de esa variable, en vez de dar el objeto
     * entero por tocado.
     */
    std::string indice;
    int64_t escala = 1; ///< por cuanto multiplica @ref indice.
    /// La expresion de dentro de los corchetes se pudo leer entera.  En false,
    /// ni el desplazamiento ni el indice describen nada (simbolo, aritmetica
    /// que no se reconoce).
    bool reconocido = false;
};

/**
 * @brief Lee un operando de memoria: base, desplazamiento, indice y escala.
 *
 * Cubre las dos sintaxis: `[rdi]`, `[rdi+8]`, `[rdi-8]`, `[rbx+rcx*8]`,
 * `[rbx+rcx*8+16]` en x86 y `[x0]`, `[x0, #8]`, `[x0, x1, lsl #3]` en arm64.
 *
 * @param operando Texto del operando, con sus corchetes.
 * @param arch     Arquitectura del cuerpo que se analiza.
 * @return Lo leido; @c base vacia si no se pudo determinar por donde se llega.
 */
AsmMemOperando asm_parse_memoria(const std::string &operando,
                                 const std::string &arch);

/**
 * @enum AsmTransferencia
 * @brief Como pasa una instruccion el valor de sus fuentes a su destino.
 *
 * Es lo que hace falta para seguir un puntero por dentro de un bloque -- que
 * `add rdi, 8` no borra a `rdi`, lo mueve ocho bytes -- sin que quien lo sigue
 * conozca un solo mnemonico.  Cada arquitectura declara los suyos; el analisis
 * pregunta por la CLASE y vale igual para todas.
 *
 * La clase no depende de la forma de los operandos: @c Transfiere cubre tanto
 * `mov rax, rbx` como `mov rax, [rbx]`, y distinguir copia de carga es mirar si
 * la fuente lleva corchetes -- cosa que el parser comun ya sabe hacer y no
 * necesita tabla.
 */
enum class AsmTransferencia : uint8_t {
    Ninguna = 0, ///< no propaga un puntero de forma que se pueda seguir.
    Transfiere,  ///< dst = fuente (copiarlo, o traerlo de memoria).
    Suma,        ///< dst = dst + fuente.
    Resta,       ///< dst = dst - fuente.
    Direccion,   ///< dst = la DIRECCION que describe la fuente, sin leerla.
};

/**
 * @brief Como transfiere @p mnem su valor, segun @p arch.
 *
 * @param mnem Mnemonico en minusculas.
 * @param arch Arquitectura.
 * @return La clase; @c Ninguna para todo lo que no propague punteros.
 */
AsmTransferencia asm_transferencia(const std::string &mnem,
                                   const std::string &arch);

/**
 * @brief Bytes que declara una pista de tamano escrita junto a un operando de
 *        memoria (`qword ptr [rdi]`), o 0 si no hay ninguna.
 *
 * Las pistas son SINTAXIS, y cada arquitectura tiene la suya (o ninguna: en
 * ARM el ancho lo dicen el registro y el mnemonico).  Por eso se pregunta por
 * arquitectura en vez de buscar las palabras de una dentro del texto de otra.
 *
 * @param operando Texto del operando de memoria, con lo que lleve delante.
 * @param arch     Arquitectura.
 * @return Bytes declarados, o 0.
 */
uint32_t asm_pista_de_tamano(const std::string &operando,
                             const std::string &arch);

/**
 * @brief Registro que cuenta las repeticiones de un prefijo `rep`, canonico.
 *
 * Es dato de la ARQUITECTURA (en x86 es `rcx`), y hace falta para poder decir
 * cuantas veces se repite un acceso en vez de rendirse ante un `rep movsb`.
 *
 * @param arch Arquitectura.
 * @return El registro, o cadena vacia si esa arquitectura no repite asi.
 */
std::string asm_contador_de_repeticion(const std::string &arch);

/**
 * @brief Cuantos BYTES toca el acceso a memoria de una linea.
 *
 * El ancho no lo dice el operando de memoria, lo dice el OTRO: `mov [rdi], rax`
 * mueve 8 bytes y `mov [rdi], eax` mueve 4, con los mismos corchetes.  Cuando
 * el otro operando es uno que eligio el compilador (`$N`), su ancho lo dice la
 * CLASE con la que se declaro, que llega en @p clases_operando.
 *
 * @param ops Operandos de la linea, en orden textual.
 * @param idx_mem Indice del operando de memoria dentro de @p ops.
 * @param clases_operando Pares (marcador, clase declarada); puede ir vacio.
 * @param arch Arquitectura del cuerpo.
 * @return Ancho en bytes, o 0 si no se puede determinar -- que NO significa
 *         cero bytes, significa que no se afirma cuantos.
 */
uint32_t asm_ancho_acceso_bytes(
    const std::vector<std::string> &ops, size_t idx_mem,
    const std::vector<std::pair<std::string, std::string>> &clases_operando,
    const std::string &arch);

/**
 * @brief Cuanto MIDE un operando de la clase @p clase, en bits.
 *
 * La clase es lo que escribio el programador (@c "reg", @c "xmm", @c "ymm",
 * @c "zmm") o un registro concreto (@c "rax").  El ancho lo resuelve la base
 * de instrucciones, que conoce los registros de CADA arquitectura; aqui no se
 * compara contra nombres de x86, que dejaria a las demas sin respuesta el dia
 * que la pidan.
 *
 * @param clase Clase declarada del operando.
 * @return Ancho en bits, o 0 si no se reconoce.
 */
uint32_t asm_ancho_bits_de_clase(const std::string &clase);

/**
 * @brief Normaliza los literales numericos de un cuerpo NASM Intel a hex
 *        explicito (@c 0x...), detectando la base de entrada.
 *
 * El ensamblador (Keystone) interpreta los enteros BARE como HEX (no
 * decimal); para que @c shl rdx, 32 signifique 32 (no 0x32) y para soportar
 * binario/octal, reescribimos cada literal a @c 0x<hex>:
 *   - @c 0x.. / @c 0X..  -> hexadecimal
 *   - @c 0b.. / @c 0B..  -> binario
 *   - @c 0o.. / @c 0O..  -> octal
 *   - resto              -> DECIMAL (convencion NASM)
 * Permite @c _ como separador de digitos.  NO toca identificadores ni
 * nombres de registro (r8/r15/xmm0): un literal es un digito cuyo char
 * previo no es parte de un identificador.
 *
 * @param body Cuerpo NASM Intel.
 * @return Cuerpo con todos los literales en @c 0x<hex>.
 */
std::string asm_normalize_numbers(const std::string &body);

/**
 * @brief Efectos de un mnemonico sobre registros/memoria/flags.
 *
 * Los registros implicitos van en forma CANONICA (rax..r15, vN).
 * @c known=false indica que el mnemonico no esta en la tabla -> el analizador
 * pide clobbers explicitos (conservador).
 */
struct AsmEffects {
    /// Registros escritos que NO aparecen como operandos (p.ej. @c rdtsc
    /// escribe rax:rdx, @c cmpxchg escribe rax implicito).  Canonicos.
    std::vector<std::string> implicit_write;

    /// Registros LEIDOS que NO aparecen como operandos (p.ej. @c syscall lee el
    /// numero de servicio en RAX y los argumentos en RDI/RSI/RDX/R10/R8/R9).
    /// Sin esto el regalloc no sabe que un `register("rdi")` vive HASTA el asm y
    /// lo elimina (DCE) o no coloca el arg en su registro.  Canonicos.
    std::vector<std::string> implicit_read;

    /// Que OPERANDOS escribe la instruccion, como bitmask: bit0=1er operando,
    /// bit1=2o, bit2=3o, ...  Un mask generaliza casos que un solo bool no
    /// cubre: @c xchg escribe los dos (0b011), @c casp/@c ldxp escriben pares,
    /// una CAS escribe el destino.  0 = no escribe ningun operando.
    uint8_t operand_write_mask = 0;

    bool touches_mem = false;   ///< toca memoria implicitamente
    bool touches_flags = false; ///< modifica RFLAGS/condition codes
    bool is_call = false;       ///< call/syscall: clobber de caller-saved
    /// Entrada/salida por PUERTO (`in`/`out`).  No toca memoria, pero es un
    /// efecto observable del exterior: no se puede eliminar ni reordenar como
    /// si fuera aritmetica, y quien la use no es codigo autonomo.  Sin
    /// distinguirla, tabular estas instrucciones las haria parecer PURAS.
    bool port_io = false;
    /// BARRERA de memoria explicita (`mfence`/`lfence`/`sfence`).  No lee ni
    /// escribe nada, pero ORDENA lo que hay a su alrededor: tratarla como una
    /// instruccion cualquiera permitiria justo lo que existe para impedir.
    bool barrier = false;

    /// Registros por los que la instruccion LEE memoria sin decirlo en sus
    /// operandos.  Canonicos.
    ///
    /// Que una instruccion no escriba los corchetes no significa que no se sepa
    /// por donde accede: `movsb` lee por `rsi` y escribe por `rdi` porque lo
    /// dice la arquitectura, igual de fijo que si estuviera escrito.  Ponerlo
    /// aqui es la diferencia entre saberlo y rendirse -- rendirse seria dar por
    /// bueno "toca memoria en algun sitio", que es rodear el analisis en vez de
    /// hacerlo.
    std::vector<std::string> implicit_mem_read;
    /// Registros por los que ESCRIBE memoria implicitamente.  Ver el anterior.
    std::vector<std::string> implicit_mem_write;
    /**
     * Bytes que toca CADA acceso implicito, o 0 si no se sabe.
     *
     * Lo dice la propia instruccion -- una `movsq` mueve ocho bytes y una
     * `movsb` uno --, asi que se declara junto a por donde accede.  Quien
     * analiza el bloque no tiene por que saber que una `q` final significa ocho
     * bytes en esta arquitectura: eso es justo lo que hace que anadir la
     * siguiente arquitectura obligue a tocar el analisis.
     */
    uint16_t mem_bytes_implicito = 0;

    /**
     * Alineacion que la instruccion EXIGE de su direccion de memoria, en
     * bytes.  0 = no exige ninguna.
     *
     * No es una preferencia ni un consejo de rendimiento: `movdqa` sobre una
     * direccion que no es multiplo de 16 no va mas lenta, lanza una excepcion
     * y el programa cae.  Que el efecto de una instruccion incluya lo que
     * PIDE, y no solo lo que hace, es lo que permite comprobarlo en vez de
     * descubrirlo ejecutando -- que es como se descubrio la ultima vez.
     *
     * @c kAlignAnchoOperando significa "tanto como mida su operando
     * vectorial", que es como lo define la arquitectura: la misma `vmovdqa`
     * exige 16 con xmm, 32 con ymm y 64 con zmm.  Ponerlo como un numero fijo
     * seria elegir uno de los tres y equivocarse en los otros dos.
     */
    uint16_t align_req = 0;

    bool known = false;         ///< false si no esta en la tabla
};

/// Valor de @c AsmEffects::align_req que significa "el ancho de su operando
/// vectorial" en vez de un numero concreto.
static constexpr uint16_t kAlignAnchoOperando = 0xFFFFu;

/**
 * @brief Consulta la tabla de efectos de un mnemonico para una arquitectura.
 *
 * @param mnemonic Nombre de la instruccion (cualquier case).
 * @param arch     Arquitectura de destino.  @c "x86_64" / @c "x86" (32-bit) /
 *                 @c "x86_16" comparten la MISMA tabla de efectos (el efecto de
 *                 @c add / @c mov / @c cmp no cambia con el ancho; lo que cambia
 *                 -- ancho de registro, tamano del slot de pila -- se resuelve
 *                 fuera de aqui).  @c "arm64" tiene su propia tabla (LL/SC,
 *                 branches, cset/csel, barreras...).  Un arch no reconocido cae
 *                 a la tabla x86.
 * @return @c AsmEffects con @c known=false si el mnemonico no esta tabulado
 *         para ese arch.
 */
AsmEffects asm_effects_for(const std::string &mnemonic,
                           const std::string &arch);

/**
 * @brief Atajo de @ref asm_effects_for para x86-64 (compat de llamadas viejas).
 */
AsmEffects asm_effects_for(const std::string &mnemonic);

/**
 * @brief Resultado de inferir clobbers de un cuerpo de inline asm.
 */
/**
 * @brief Una instruccion del cuerpo que EXIGE su direccion alineada.
 *
 * Se recoge para poder decirlo.  Una exigencia que el compilador conoce y no
 * comunica no sirve de nada: el programa sigue cayendo en ejecucion, solo que
 * ahora ademas se sabia.
 */
struct AsmAlignReq {
    std::string mnemonic; ///< la instruccion, tal como se escribio.
    std::string operando; ///< el operando de memoria, con sus corchetes.
    /**
     * De donde sale la direccion: registro canonico o marcador `$N`, ya
     * extraido por @ref asm_base_de_memoria.  Vacio = no se pudo determinar.
     *
     * Sale de aqui y no lo saca quien lee: leer el interior de unos corchetes
     * es la MISMA operacion que hace el analisis de accesos del bloque, y
     * quien tenga las ligaduras lo unico que necesita es un nombre por el que
     * preguntar.
     */
    std::string base;
    /**
     * Alineacion exigida, EN BYTES y ya resuelta.  0 = no se pudo determinar.
     *
     * Sale resuelta a proposito.  Que una instruccion exija tanto como mida su
     * operando -- `vmovdqa` pide 16 con xmm, 32 con ymm y 64 con zmm -- es
     * conocimiento sobre INSTRUCCIONES, y vive donde vive el resto: aqui.
     * Devolver el nombre del operando y que otro dedujera el ancho partiria
     * una sola regla entre dos ficheros, y una regla en dos sitios acaba
     * siendo dos reglas.
     */
    uint16_t bytes = 0;
};

/**
 * @brief Resultado de inferir clobbers de un cuerpo de inline asm.
 */
struct AsmInferResult {
    std::vector<std::string> clobber_regs; ///< nombres GCC-ready (rax.., xmmN)
    bool clobber_memory = false;
    bool clobber_flags = false;
    std::vector<std::string> unknown_mnemonics; ///< para emitir warning
    /// Instrucciones que exigen alineacion.  Vacio = ninguna la pide.
    std::vector<AsmAlignReq> align_reqs;
};

/**
 * @brief Infiere los clobbers de un cuerpo NASM Intel.
 *
 * Tokeniza linea a linea (descarta comentarios @c ; y @c //, labels
 * @c name: y prefijos @c lock/rep/...), consulta @c asm_effects_for por
 * mnemonico, UNE los registros escritos (implicitos + 1er operando si la
 * instruccion lo escribe) y EXCLUYE los registros canonicos ligados por
 * @c register() (estan en @p bound_canon: son operandos, no clobbers).
 * @c touches_mem se activa si algun operando tiene @c [...] o el mnemonico
 * toca memoria.  Un @c call/syscall marca clobber conservador de TODO el
 * caller-saved del ABI.  Mnemonicos desconocidos se acumulan en
 * @c unknown_mnemonics para que el caller emita un warning pidiendo
 * clobbers explicitos.
 *
 * @param nasm_body   Cuerpo verbatim del bloque @c asm.
 * @param bound_canon Registros canonicos (rax.., vN) ligados por register().
 * @return Clobbers inferidos (nombres listos para la clobber-list de GCC).
 */
AsmInferResult asm_infer_clobbers(const std::string &nasm_body,
                                  const std::vector<std::string> &bound_canon);

/**
 * @brief Igual que la anterior, sabiendo ademas de que CLASE es cada operando.
 *
 * Hay instrucciones cuyo efecto depende del ancho de su operando -- las que
 * exigen alineacion son el caso claro --, y en el cuerpo el operando se llama
 * como lo bautizo quien escribio el bloque, no `xmm0`.  Con el mapa
 * `nombre -> clase` la inferencia puede resolverlo ella misma en vez de
 * devolver una respuesta a medias para que la complete otro.
 *
 * @param nasm_body Cuerpo NASM Intel.
 * @param bound_canon Registros ya ligados por `register(...)`, canonicos.
 * @param clases_operando Pares (nombre en el cuerpo, clase declarada:
 *        `"reg"`, `"xmm"`, `"ymm"`, `"zmm"`, `"mem"`...).
 * @return El mismo resultado, con las exigencias ya resueltas a bytes.
 */
AsmInferResult asm_infer_clobbers(
    const std::string &nasm_body, const std::vector<std::string> &bound_canon,
    const std::vector<std::pair<std::string, std::string>> &clases_operando);

} // namespace vx

#endif // VX_ASM_EFFECTS_H
