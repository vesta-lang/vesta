/**
 * @file grammar.test.js
 * @brief Comprueba la gramatica con el mismo motor que usa el editor.
 *
 * Una gramatica mal escrita no da un error visible: el editor la carga, algo
 * dentro no compila y el resaltado se apaga a medias, sin decir nada.  Aqui se
 * carga con el mismo motor de expresiones que usa el editor -- que es MAS
 * estricto que el de JavaScript, por ejemplo con el contexto previo de anchura
 * variable -- y se comprueba que cada construccion del lenguaje cae en el
 * ambito que le toca.
 *
 * Uso:  node test/grammar.test.js
 */

'use strict';

const fs = require('fs');
const path = require('path');
const oniguruma = require('vscode-oniguruma');
const textmate = require('vscode-textmate');

const RAIZ = path.resolve(__dirname, '..');

/** Gramaticas de la extension, por su ambito. */
const GRAMATICAS = {
    'source.vesta': path.join(RAIZ, 'syntaxes', 'vesta.tmLanguage.json'),
    'source.nasm': path.join(RAIZ, 'syntaxes', 'nasm.tmLanguage.json'),
};

/**
 * Casos: por cada linea de codigo, que fragmento debe llevar que ambito.
 * El ambito se comprueba por prefijo, para no atarse a la ultima parte.
 */
const CASOS = [
    // Declaraciones y modificadores.
    ['class Animal : Nombre {', 'Animal', 'entity.name.type'],
    ['class Animal : Nombre {', 'class', 'storage.type'],
    ['public static const i64 x = 0;', 'public', 'storage.modifier'],
    ['internal i64 y = 0;', 'internal', 'storage.modifier'],
    ['struct Punto { f64 x; }', 'Punto', 'entity.name.type'],
    ['union Bits { u64 raw; }', 'union', 'storage.type'],
    ['concept Numerico<T> = true;', 'concept', 'storage.type'],
    ['impl Ordered for Punto {', 'impl', 'keyword.other'],
    ['extension Punto {', 'extension', 'keyword.other'],
    ['namespace std.collections;', 'std.collections', 'entity.name.namespace'],

    // Importaciones, en sus tres formas.
    ['import std.io;', 'import', 'keyword.control.import'],
    ['import std.memory only memset, memcpy;', 'only', 'keyword.control.import'],
    ['import std.syscall only *;', '*', 'keyword.operator.wildcard'],
    ['import "vx_async" only vasync_run;', '"vx_async"', 'string.quoted.double.module'],

    // Tipos: primitivos, de propiedad, colecciones y los anchos de biblioteca.
    ['i64 a = 0;', 'i64', 'support.type.primitive'],
    ['uint32_t b = 0;', 'uint32_t', 'support.type.primitive'],
    ['unique<i64> p = unique_box(1);', 'unique', 'support.type.ownership'],
    ['borrow_mut<i64> m = lend_mut(p);', 'borrow_mut', 'support.type.ownership'],
    ['ArrayList xs = arraylist(16);', 'ArrayList', 'support.class.collection'],
    ['u128 grande = 0;', 'u128', 'support.type.stdlib'],
    ['usize n = 0;', 'usize', 'support.type.stdlib'],

    // Literales.
    ['i64 h = 0xDEADBEEF;', '0xDEADBEEF', 'constant.numeric.hexadecimal'],
    ['i64 b = 0b1010;', '0b1010', 'constant.numeric.binary'],
    ['i64 o = 0o755;', '0o755', 'constant.numeric.octal'],
    ['f64 f = 0x1.8p+1;', '0x1.8p+1', 'constant.numeric.float.hexadecimal'],
    ['f64 g = 1e-9;', '1e-9', 'constant.numeric.float'],
    ['bool v = true;', 'true', 'constant.language'],
    ['string s = "hola";', '"', 'string.quoted.double'],
    ['string r = r"sin\\escapes";', 'r"', 'string.quoted.other.raw'],
    ["char c = 'a';", "'a'", 'string.quoted.single'],

    // Cadenas: escapes e interpolacion con especificador de formato.
    ['println("linea\\n");', '\\n', 'constant.character.escape'],
    ['println("valor ${x}");', '${', 'punctuation.definition.template-expression'],
    ['println("v ${n:hex:>20}");', 'hex', 'support.constant.format'],
    ['println("v ${n:hex:>20}");', '>20', 'keyword.operator.format'],

    // Anotaciones: con y sin argumentos.
    ['@Override', '@', 'punctuation.definition.annotation'],
    ['@Override', 'Override', 'storage.type.annotation'],
    ['@Target(windows, x86_64)', 'windows', 'support.constant.annotation-argument'],
    ['@complexity(O(n), n = arg0)', 'complexity', 'storage.type.annotation'],
    ['@overlay struct LdrData {', 'overlay', 'storage.type.annotation'],
    // La declaracion que sigue a una anotacion sin argumentos NO se tine de
    // argumento: fue un fallo real de la primera version de la gramatica.
    ['@Naked i64 via_call(i64 a) {', 'i64', 'support.type.primitive'],

    // Control de flujo y concurrencia.
    ['if (n < 2) return n;', 'if', 'keyword.control'],
    ['for (x in xs) {', 'in', 'keyword.control'],
    ['foreach (i64 x : xs) {', 'foreach', 'keyword.control'],
    ['match (v) {', 'match', 'keyword.control'],
    ['spawn here {', 'here', 'keyword.control.concurrency'],
    ['spawn on(2) {', 'on', 'keyword.control.concurrency'],
    ['i32 r = await fut;', 'await', 'keyword.control.concurrency'],
    ['synchronized (obj) {', 'synchronized', 'keyword.control.concurrency'],

    // Operadores propios del lenguaje.
    ['i64 v = parse(s)?;', '?', 'keyword.operator.other'],
    ['i64 w = !!opt;', '!!', 'keyword.operator.logical'],
    ['for (i in 0..10) {', '..', 'keyword.operator.range'],
    ['i64 suma(i64... xs) {', '...', 'keyword.operator.range'],
    ['fn(i64) -> i64 f = g;', '->', 'keyword.operator.other'],
    ['public i64 edad => this.a;', '=>', 'keyword.operator.other'],
    ['if (a == b) {', '==', 'keyword.operator.comparison'],
    ['if (a != b) {', '!=', 'keyword.operator.comparison'],
    ['x <<= 2;', '<<=', 'keyword.operator.assignment'],
    ['i64 z = a << 2;', '<<', 'keyword.operator.bitwise'],

    // Comentarios y preprocesador.
    ['// comentario', '// comentario', 'comment.line'],
    ['/** doc */', '/**', 'comment.block.documentation'],
    ['#define MAX 10', 'define', 'keyword.control.directive'],
    ['#include "otro.vx"', 'include', 'keyword.control.directive.include'],

    // Comptime y builtins.
    ['comptime i64 k = 2;', 'comptime', 'keyword.other.comptime'],
    ['static_assert(k == 2, "mal");', 'static_assert', 'keyword.other.comptime'],
    ['i64 t = sizeof<i64>();', 'sizeof', 'support.function.builtin.memory'],
    ['println("hola");', 'println', 'support.function.builtin.io'],
    ['i64 s = sqrt(x);', 'sqrt', 'support.function.builtin.math'],
    ['unique<i64> p = unique_box(1);', 'unique_box', 'support.function.builtin.ownership'],

    // Etiquetas de salto.
    ['fin:', 'fin', 'entity.name.label'],
    ['goto fin;', 'goto', 'keyword.control'],
];

/** Casos de varias lineas: bloques que dependen de su contexto. */
const CASOS_BLOQUE = [
    {
        nombre: 'bloque de ensamblador NASM',
        lineas: [
            'register("rax") i64 r;',
            'asm volatile {',
            '    mov rax, gs:[0x60]      ; el PEB',
            '.bucle:',
            '    lock inc qword [rsi + rdi*8 + 16]',
            '    movaps xmm0, [rel datos]',
            '    ret',
            '}',
        ],
        esperado: [
            // Lo de fuera del bloque sigue siendo Vesta.
            [0, 'register', 'storage.modifier'],
            [0, '"rax"', 'variable.language.register'],
            [1, 'asm', 'keyword.other.asm'],
            [1, 'volatile', 'storage.modifier'],
            // Y lo de dentro es NASM.
            [2, 'mov', 'keyword.operator.word.mnemonic.nasm'],
            [2, 'rax', 'variable.language.register.gp.nasm'],
            [2, 'gs', 'variable.language.register.system.nasm'],
            [2, '0x60', 'constant.numeric.hexadecimal.nasm'],
            [2, '; el PEB', 'comment.line.semicolon.nasm'],
            [3, '.bucle', 'entity.name.label.local.nasm'],
            [4, 'lock', 'storage.modifier.prefix.nasm'],
            [4, 'inc', 'keyword.operator.word.mnemonic.nasm'],
            [4, 'qword', 'storage.modifier.size.nasm'],
            [4, 'rsi', 'variable.language.register.gp.nasm'],
            [5, 'movaps', 'keyword.operator.word.mnemonic.nasm'],
            [5, 'xmm0', 'variable.language.register.vector.nasm'],
            [5, 'rel', 'storage.modifier.size.nasm'],
        ],
    },
    {
        // Lo que hay en pantalla MIENTRAS se escribe: la cadena esta abierta
        // porque aun no se ha tecleado la comilla de cierre.  Si esa cadena
        // sobrevive al salto de linea, el resto del fichero queda marcado como
        // cadena, y entonces el editor deja de colorear Y de sugerir -- trae
        // las sugerencias apagadas dentro de cadenas.  El sintoma no apunta a
        // la causa: parece que el servidor no responde.
        nombre: 'cadena a medio escribir',
        lineas: [
            'i32 main() {',
            '    println("hola',
            '    i64 x = 0;',
            '    return x;',
            '}',
        ],
        esperado: [
            [1, '"hola', 'string.quoted.double'],
            // La linea siguiente ya NO es cadena: vuelve a ser codigo.
            [2, 'i64', 'support.type.primitive'],
            [3, 'return', 'keyword.control'],
        ],
    },
    {
        nombre: 'cadena cruda a medio escribir',
        lineas: ['string s = r"sin cerrar', 'i64 y = 1;'],
        esperado: [[1, 'i64', 'support.type.primitive']],
    },
    {
        nombre: 'cadena de varias lineas, que SI continua',
        lineas: ['string s = """primera', 'sigue dentro', '""";', 'i64 z = 2;'],
        esperado: [
            [1, 'sigue dentro', 'string.quoted.triple'],
            [3, 'i64', 'support.type.primitive'],
        ],
    },
    {
        // Forma tomada de examples_codes_vx/aot/17_boot16.vx, tal cual.
        nombre: 'bloque de ensamblador con nombre',
        lineas: [
            '@bits(16) @section(".text", "rx")',
            'asm boot {',
            '    cli',
            '    mov sp, 0x7C00',
            '    mov ah, 0x0E          ; BIOS teletype',
            'hang:',
            '    jmp hang',
            '    times 510-($-$$) db 0',
            '    dw 0xAA55',
            '}',
        ],
        esperado: [
            [0, '@bits', 'storage.type.annotation'],
            [0, '".text"', 'string.quoted.double'],
            [1, 'asm', 'keyword.other.asm'],
            [1, 'boot', 'entity.name.function.asm'],
            [2, 'cli', 'keyword.operator.word.mnemonic.nasm'],
            [3, 'sp', 'variable.language.register'],
            [4, '; BIOS teletype', 'comment.line.semicolon.nasm'],
            [5, 'hang', 'entity.name.label.nasm'],
            [7, 'times', 'keyword.control.directive.data.nasm'],
            [7, '$-$$', 'constant.language.position.nasm'],
            [8, 'dw', 'keyword.control.directive.data.nasm'],
            [8, '0xAA55', 'constant.numeric.hexadecimal.nasm'],
        ],
    },
    {
        // Misma forma que usa la biblioteca (`asm ( rdi d = dst, ... ) {`),
        // con nombres largos para que la comprobacion no pueda confundirlos
        // con las letras de los propios registros.
        nombre: 'ensamblador con lista de operandos',
        lineas: [
            'asm ( rdi destino = dst, reg contador = n, ) {',
            '    rep movsb',
            '}',
        ],
        esperado: [
            [0, 'asm', 'keyword.other.asm'],
            [0, 'rdi', 'variable.language.register'],
            [0, 'destino', 'variable.parameter.asm'],
            [0, 'reg', 'storage.type.asm-operand'],
            [0, 'contador', 'variable.parameter.asm'],
            [1, 'rep', 'storage.modifier.prefix.nasm'],
            [1, 'movsb', 'keyword.operator.word.mnemonic.nasm'],
        ],
    },
    {
        nombre: 'ensamblador con calificadores y comentario de Vesta',
        lineas: [
            'asm volatile noinfer {',
            '    mov al, 0x50            // marca de modo protegido',
            '    out 0xE9, al',
            '}',
        ],
        esperado: [
            [0, 'volatile', 'storage.modifier'],
            [0, 'noinfer', 'storage.modifier'],
            // Dentro del ensamblador valen los dos comentarios: el de NASM y
            // el del lenguaje.  En el corpus se usan ambos.
            [1, '// marca', 'comment.line.double-slash'],
            [2, 'out', 'keyword.operator.word.mnemonic.nasm'],
        ],
    },
    {
        // Forma tomada de la biblioteca: bytes vx_exc_state { times 24 db 0 }
        nombre: 'bloque de datos con nombre',
        lineas: [
            'bytes tabla {',
            "    db 0x01, 'A', \"Hi\"",
            '    dq main',
            '}',
        ],
        esperado: [
            [0, 'bytes', 'storage.type'],
            [0, 'tabla', 'entity.name.constant.bytes'],
            [1, 'db', 'keyword.control.directive.data.nasm'],
            [1, '"Hi"', 'string.quoted.double.nasm'],
            [2, 'dq', 'keyword.control.directive.data.nasm'],
        ],
    },
    {
        nombre: 'clausula de restriccion',
        lineas: ['T maximo<T>(T a, T b) where T: Ordered {'],
        esperado: [
            [0, 'where', 'keyword.other'],
            [0, 'Ordered', 'entity.name.type.concept'],
        ],
    },
];

/**
 * Carga la gramatica en el motor del editor.
 * @returns {Promise<object>} La gramatica lista para tokenizar.
 */
async function cargarGramatica() {
    const wasm = fs.readFileSync(
        path.join(RAIZ, 'node_modules', 'vscode-oniguruma', 'release', 'onig.wasm'),
    );
    await oniguruma.loadWASM(wasm.buffer);

    const registro = new textmate.Registry({
        onigLib: Promise.resolve({
            createOnigScanner: fuentes => new oniguruma.OnigScanner(fuentes),
            createOnigString: cadena => new oniguruma.OnigString(cadena),
        }),
        loadGrammar: async ambito => {
            // La de Vesta empotra la de NASM, asi que hay que saber servir las
            // dos: si la empotrada no se resuelve, el bloque asm se queda sin
            // resaltar y no salta ningun error.
            const ruta = GRAMATICAS[ambito];
            if (!ruta) {
                return null;
            }
            return textmate.parseRawGrammar(fs.readFileSync(ruta, 'utf8'), ruta);
        },
    });

    const gramatica = await registro.loadGrammar('source.vesta');
    if (!gramatica) {
        throw new Error('la gramatica no se pudo cargar');
    }
    return gramatica;
}

/**
 * Tokeniza unas lineas arrastrando el estado, como hace el editor.
 * @param {object} gramatica Gramatica cargada.
 * @param {string[]} lineas Lineas de codigo.
 * @returns {Array} Un array de tokens por linea.
 */
function tokenizar(gramatica, lineas) {
    let estado = textmate.INITIAL;
    const salida = [];
    for (const linea of lineas) {
        const resultado = gramatica.tokenizeLine(linea, estado);
        salida.push(resultado.tokens);
        estado = resultado.ruleStack;
    }
    return salida;
}

/**
 * Busca el ambito que cubre un fragmento dentro de una linea tokenizada.
 * @param {string} linea Texto de la linea.
 * @param {Array} tokens Tokens de esa linea.
 * @param {string} fragmento Texto a localizar.
 * @param {string} prefijo Ambito esperado, por prefijo.
 * @returns {{ok: boolean, ambitos: string[]}} Resultado y ambitos hallados.
 */
function ambitoDe(linea, tokens, fragmento, prefijo) {
    const inicio = linea.indexOf(fragmento);
    if (inicio < 0) {
        return { ok: false, ambitos: ['(el fragmento no esta en la linea)'] };
    }
    const ambitos = [];
    for (const token of tokens) {
        // Un token cuenta si se solapa con el fragmento buscado.
        if (token.endIndex <= inicio || token.startIndex >= inicio + fragmento.length) {
            continue;
        }
        for (const ambito of token.scopes) {
            ambitos.push(ambito);
            if (ambito.startsWith(prefijo)) {
                return { ok: true, ambitos };
            }
        }
    }
    return { ok: false, ambitos };
}

/**
 * Pasa la gramatica por todos los ejemplos del repositorio.
 *
 * Comprueba dos cosas que los casos sueltos no ven: que ningun fichero real
 * hace saltar al motor, y que ninguno deja una construccion ABIERTA al llegar
 * al final -- una cadena o un bloque que no cierra tine el resto del fichero y
 * es de los fallos mas molestos de un resaltado, porque solo se nota lejos de
 * donde esta la causa.
 *
 * @param {object} gramatica Gramatica cargada.
 * @returns {{revisados: number, fallos: string[]}} Resumen de la pasada.
 */
function pasarPorElCorpus(gramatica) {
    const directorio = path.resolve(RAIZ, '..', '..', 'examples_codes_vx');
    const fallos = [];
    if (!fs.existsSync(directorio)) {
        return { revisados: 0, fallos };
    }

    const ficheros = fs.readdirSync(directorio).filter(n => n.endsWith('.vx'));
    for (const nombre of ficheros) {
        const ruta = path.join(directorio, nombre);
        let lineas;
        try {
            lineas = fs.readFileSync(ruta, 'utf8').split(/\r?\n/);
        } catch {
            continue;
        }
        let estado = textmate.INITIAL;
        try {
            for (const linea of lineas) {
                const resultado = gramatica.tokenizeLine(linea, estado);
                estado = resultado.ruleStack;
            }
        } catch (err) {
            fallos.push(`${nombre}: el motor fallo (${err.message})`);
            continue;
        }
        // La pila vuelve a su base cuando todo lo que se abrio se cerro.
        if (typeof estado.depth === 'number' && estado.depth !== 1) {
            fallos.push(
                `${nombre}: queda una construccion abierta al final del fichero ` +
                `(profundidad ${estado.depth})`,
            );
        }
    }
    return { revisados: ficheros.length, fallos };
}

/** Punto de entrada. */
async function main() {
    const gramatica = await cargarGramatica();
    let pasadas = 0;
    const fallos = [];

    for (const [linea, fragmento, prefijo] of CASOS) {
        const [tokens] = tokenizar(gramatica, [linea]);
        const { ok, ambitos } = ambitoDe(linea, tokens, fragmento, prefijo);
        if (ok) {
            pasadas++;
        } else {
            fallos.push(
                `'${fragmento}' en "${linea}" deberia ser ${prefijo}; ` +
                `llego [${[...new Set(ambitos)].join(', ')}]`,
            );
        }
    }

    for (const caso of CASOS_BLOQUE) {
        const tokens = tokenizar(gramatica, caso.lineas);
        for (const [indice, fragmento, prefijo] of caso.esperado) {
            const { ok, ambitos } = ambitoDe(caso.lineas[indice], tokens[indice], fragmento, prefijo);
            if (ok) {
                pasadas++;
            } else {
                fallos.push(
                    `[${caso.nombre}] '${fragmento}' deberia ser ${prefijo}; ` +
                    `llego [${[...new Set(ambitos)].join(', ')}]`,
                );
            }
        }
    }

    const corpus = pasarPorElCorpus(gramatica);
    if (corpus.revisados > 0) {
        if (corpus.fallos.length === 0) {
            pasadas++;
            console.log(`  ok    ${corpus.revisados} ejemplos del corpus tokenizan sin dejar nada abierto`);
        } else {
            fallos.push(...corpus.fallos);
        }
    }

    console.log(`${pasadas} comprobaciones pasadas, ${fallos.length} fallidas`);
    for (const fallo of fallos) {
        console.log(`  - ${fallo}`);
    }
    process.exit(fallos.length === 0 ? 0 : 1);
}

main().catch(err => {
    console.error(err);
    process.exit(2);
});
