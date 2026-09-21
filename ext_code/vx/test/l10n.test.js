/**
 * @file l10n.test.js
 * @brief Que todo lo que el usuario LEE tenga sus dos idiomas.
 *
 * El modo de fallo de la localizacion es el peor de todos: no falla.  Una clave
 * que falta en el paquete de traduccion no da error ni deja hueco -- el editor
 * se queda con la cadena original --, asi que la extension sale medio traducida
 * y parece entera.  Solo se nota si alguien mira, y nadie mira.
 *
 * Por eso esto no comprueba que "haya traducciones", sino las tres igualdades
 * que tienen que cumplirse a la vez:
 *
 *   - cada `%clave%` del manifiesto esta en los DOS `package.nls`;
 *   - no sobra ninguna traduccion sin original, que delata un renombrado a
 *     medias -- se cambio la clave y la vieja se quedo ahi, traducida y muerta;
 *   - cada literal que pasa por `vscode.l10n.t(...)` esta en el paquete `es`.
 *
 * El idioma base es el INGLES, como en `catalog/diagnostics.toml`
 * (`languages = ["en", "es"]`, el primero es el de respaldo).  Lo que se
 * traduce es el texto; los identificadores, los codigos y las rutas no.
 */

const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');

let passed = 0;
const failures = [];

/**
 * @brief Registra una comprobacion.
 * @param condition Resultado.
 * @param description Que se comprobaba.
 * @param detail Informacion extra para el fallo.
 */
function expect(condition, description, detail) {
    if (condition) {
        passed++;
        console.log('  ok    ' + description);
    } else {
        const msg = detail ? description + ' -- ' + detail : description;
        failures.push(msg);
        console.log('  FALLA ' + msg);
    }
}

/** @brief Lee un JSON del arbol de la extension. */
function readJson(relative) {
    return JSON.parse(fs.readFileSync(path.join(ROOT, relative), 'utf8'));
}

/** @brief Todos los `.ts` de `src/`, recursivamente. */
function sourceFiles(dir) {
    const out = [];
    for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
        const full = path.join(dir, entry.name);
        if (entry.isDirectory()) {
            out.push(...sourceFiles(full));
        } else if (entry.name.endsWith('.ts')) {
            out.push(full);
        }
    }
    return out;
}

const manifest = readJson('package.json');
const nlsBase = readJson('package.nls.json');
const nlsEs = readJson('package.nls.es.json');
const bundleEs = readJson(path.join('l10n', 'bundle.l10n.es.json'));

// --- El manifiesto: cada %clave% existe en los dos idiomas ------------------
// Una clave ocupa el valor ENTERO (`"%vesta.compile.title%"`), y esa es la
// diferencia que hay que respetar: dentro de un texto, `%...%` es una variable
// de entorno de Windows -- `%ProgramFiles%` sale en la ayuda del ajuste de la
// ruta --, y tratarla como clave pedia traducir el nombre de una carpeta.
const usedKeys = new Set();
const collectKeys = value => {
    if (typeof value === 'string') {
        const whole = /^%([^%]+)%$/.exec(value);
        if (whole) {
            usedKeys.add(whole[1]);
        }
    } else if (Array.isArray(value)) {
        value.forEach(collectKeys);
    } else if (value && typeof value === 'object') {
        Object.values(value).forEach(collectKeys);
    }
};
collectKeys(manifest);

expect(usedKeys.size > 0, 'el manifiesto usa claves de traduccion',
       String(usedKeys.size));

const missingBase = [...usedKeys].filter(k => !(k in nlsBase));
expect(missingBase.length === 0,
       'toda clave del manifiesto tiene texto base',
       missingBase.join(', '));

const missingEs = [...usedKeys].filter(k => !(k in nlsEs));
expect(missingEs.length === 0,
       'toda clave del manifiesto esta traducida al espanol',
       missingEs.join(', '));

// Al reves: una traduccion sin original es una clave que se renombro y dejo
// atras su version vieja.  No se ve nunca, y cuando alguien la busca la
// encuentra y cree que sigue en uso.
const orphanEs = Object.keys(nlsEs).filter(k => !(k in nlsBase));
expect(orphanEs.length === 0,
       'ninguna traduccion se quedo sin original',
       orphanEs.join(', '));

const unusedBase = Object.keys(nlsBase).filter(k => !usedKeys.has(k));
expect(unusedBase.length === 0,
       'ningun texto base sobra en el manifiesto',
       unusedBase.join(', '));

// --- El codigo: cada literal de `l10n.t` esta en el paquete `es` ------------
// Solo se reconocen los literales ESCRITOS: un `l10n.t(variable)` no se puede
// extraer, y por eso no se escribe asi.  La comprobacion de abajo lo exige.
const files = sourceFiles(path.join(ROOT, 'src'));
const literals = new Map();
const dynamic = [];
for (const file of files) {
    const text = fs.readFileSync(file, 'utf8');
    for (const m of text.matchAll(/l10n\.t\(\s*(['"`])((?:\\.|(?!\1).)*)\1/g)) {
        literals.set(m[2], path.relative(ROOT, file));
    }
    for (const m of text.matchAll(/l10n\.t\(\s*([A-Za-z_$])/g)) {
        dynamic.push(path.relative(ROOT, file) + ': ' + m[1] + '...');
    }
}

expect(literals.size > 0, 'el codigo pide texto traducible',
       String(literals.size));

expect(dynamic.length === 0,
       'ningun texto llega a l10n.t por variable',
       dynamic.join(', '));

const untranslated = [...literals.keys()].filter(s => !(s in bundleEs));
expect(untranslated.length === 0,
       'todo texto del codigo esta traducido al espanol',
       untranslated.map(s => JSON.stringify(s.slice(0, 40))).join(', '));

const orphanBundle = Object.keys(bundleEs).filter(s => !literals.has(s));
expect(orphanBundle.length === 0,
       'ninguna traduccion del codigo se quedo sin original',
       orphanBundle.map(s => JSON.stringify(s.slice(0, 40))).join(', '));

// --- Los huecos van numerados -----------------------------------------------
// Un `{0}` que se pierde al traducir deja el dato FUERA del mensaje, y el
// mensaje sigue leyendose bien: "queda libre ." en vez de la ruta.
const placeholderMismatch = [];
for (const [source, translated] of Object.entries(bundleEs)) {
    const target = typeof translated === 'string' ? translated : translated.message;
    const inSource = (source.match(/\{\d+\}/g) || []).sort().join(',');
    const inTarget = (String(target).match(/\{\d+\}/g) || []).sort().join(',');
    if (inSource !== inTarget) {
        placeholderMismatch.push(JSON.stringify(source.slice(0, 40)));
    }
}
expect(placeholderMismatch.length === 0,
       'la traduccion conserva los huecos del original',
       placeholderMismatch.join(', '));

// --- Cobertura: ninguna frase se quedo fuera del catalogo -------------------
// Lo de arriba comprueba COHERENCIA, y esa es su ceguera: no puede ver una
// cadena que nunca paso por `l10n.t`.  Pasaba entero con tres frases migradas
// de cincuenta y seis, y lo que quedaba escrito a mano no salia por ningun
// lado.
//
// Se busca al reves: una cadena del codigo que PAREZCA una frase en espanol y
// no este en el catalogo.  Basta con dos palabras seguidas y una de ellas de
// la lista de abajo -- palabras que en ingles no existen --, y eso deja fuera
// lo que no es texto: una clase CSS, el valor de un enum o el nombre de un
// campo son UNA palabra.
const SPANISH_WORDS = [
    'que', 'para', 'por', 'con', 'sin', 'como', 'donde', 'cuando', 'esta',
    'este', 'del', 'los', 'las', 'una', 'uno', 'pero', 'ademas', 'tambien',
    'cada', 'mas', 'muy', 'hay', 'son', 'ser', 'aqui', 'lo', 'al', 'se',
    'no', 'linea', 'lineas', 'funcion', 'modulo', 'codigo', 'fichero',
];
const spanishLike = new Set(SPANISH_WORDS);

/** @brief El fichero sin sus comentarios, que SI van en espanol. */
function withoutComments(text) {
    return text
        .replace(/\/\*[\s\S]*?\*\//g, ' ')
        .replace(/(^|[^:])\/\/[^\n]*/g, '$1');
}

/** @brief Si una cadena se lee como una frase en espanol. */
function looksSpanish(text) {
    const words = text.toLowerCase().split(/[^a-z]+/).filter(Boolean);
    if (words.length < 2) {
        return false;
    }
    return words.some(w => spanishLike.has(w));
}

const untranslatedLiterals = [];
for (const file of files) {
    const text = withoutComments(fs.readFileSync(file, 'utf8'));
    /* Un `case 'sin clasificar':` compara con un DATO que manda el servidor,
     * no ensena texto: traducirlo romperia la comparacion, que es justo lo
     * contrario de lo que este test pide. */
    const caseValues = new Set(
        [...text.matchAll(/case\s+(['"])((?:\\.|(?!\1).)*)\1\s*:/g)].map(m => m[2]));
    for (const m of text.matchAll(/(['"])((?:\\.|(?!\1)[^\n])*)\1/g)) {
        const value = m[2];
        if (literals.has(value) || caseValues.has(value) || !looksSpanish(value)) {
            continue;
        }
        untranslatedLiterals.push(
            path.relative(ROOT, file) + ': ' + JSON.stringify(value.slice(0, 50)));
    }
}

expect(untranslatedLiterals.length === 0,
       'ninguna frase del codigo se escribio fuera del catalogo',
       untranslatedLiterals.join('\n            '));

console.log('\n' + passed + ' comprobaciones pasadas, ' + failures.length + ' fallidas');
for (const f of failures) {
    console.log('  - ' + f);
}
process.exit(failures.length === 0 ? 0 : 1);
