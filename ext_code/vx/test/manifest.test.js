/**
 * @file manifest.test.js
 * @brief Que lo que la barra lateral ofrece exista de verdad.
 *
 * El arbol de acciones y la lista de comandos del manifiesto son dos sitios que
 * tienen que decir lo mismo.  Cuando no lo dicen, no falla nada: la fila
 * aparece, se pulsa y no pasa NADA -- el editor no encuentra el comando y se lo
 * calla --.  Eso es peor que un error, porque parece que la herramienta no
 * funciona en vez de parecer que esta mal escrita.
 *
 * Tambien se comprueba lo que el manifiesto promete en disco: un icono que no
 * esta deja el apartado sin nada donde pulsar.
 */

const assert = require('assert');
const fs = require('fs');
const path = require('path');

const RAIZ = path.resolve(__dirname, '..');

let pasadas = 0;
const fallos = [];

/**
 * @brief Registra una comprobacion.
 * @param condicion Resultado.
 * @param descripcion Que se comprobaba.
 * @param detalle Informacion extra para el fallo.
 */
function exigir(condicion, descripcion, detalle) {
    if (condicion) {
        pasadas++;
        console.log('  ok    ' + descripcion);
    } else {
        const msg = detalle ? descripcion + ' -- ' + detalle : descripcion;
        fallos.push(msg);
        console.log('  FALLA ' + msg);
    }
}

const manifiesto = JSON.parse(
    fs.readFileSync(path.join(RAIZ, 'package.json'), 'utf8'));
const contribuye = manifiesto.contributes || {};

// --- El apartado propio en la barra de la izquierda ----------------------
const contenedores = (contribuye.viewsContainers || {}).activitybar || [];
const vesta = contenedores.find(c => c.id === 'vesta');
exigir(!!vesta, 'la extension aporta su apartado en la barra de actividad');
if (vesta) {
    exigir(!!vesta.icon, 'el apartado declara un icono');
    const icono = path.join(RAIZ, vesta.icon);
    exigir(fs.existsSync(icono), 'el icono existe en disco', vesta.icon);
    if (fs.existsSync(icono)) {
        const svg = fs.readFileSync(icono, 'utf8');
        exigir(svg.includes('<svg'), 'el icono es un SVG');
        // La barra pinta el icono como una mascara de un color: un relleno
        // fijo se ve igual en cualquier tema, incluido aquel en el que no se
        // distingue del fondo.
        exigir(svg.includes('currentColor'),
               'el icono se pinta con el color del tema');
    }
}

const vistas = (contribuye.views || {}).vesta || [];
exigir(vistas.length > 0, 'el apartado tiene alguna vista dentro');
const acciones = vistas.find(v => v.id === 'vesta.actions');
exigir(!!acciones, 'la vista de acciones esta declarada');

// La vista tiene que poder abrirse sin haber tocado ningun .vx.
const activaciones = manifiesto.activationEvents || [];
exigir(activaciones.includes('onView:vesta.actions'),
       'la extension se activa al abrir el apartado',
       'sin esto el panel sale vacio hasta tocar un fichero');

// --- Cada fila del arbol dispara un comando que existe -------------------
const arbol = fs.readFileSync(
    path.join(RAIZ, 'src', 'views', 'actionsTree.ts'), 'utf8');
const usados = [...arbol.matchAll(/comando:\s*'([^']+)'/g)].map(m => m[1]);
const declarados = new Set((contribuye.commands || []).map(c => c.command));

exigir(usados.length > 0, 'el arbol ofrece acciones', String(usados.length));
const huerfanos = usados.filter(c => !declarados.has(c));
exigir(huerfanos.length === 0,
       'toda accion del arbol es un comando declarado',
       huerfanos.join(', '));

// Y al reves: un comando que no esta en ningun sitio visible solo se puede
// usar escribiendo su nombre, que es justo lo que se queria evitar.  Se
// permiten los que se disparan solos desde el editor.
const SOLO_DE_DENTRO = new Set([
    'vesta.runCell',       // lo ofrece la lente de codigo, sobre la celda
    'vesta.showMachineView', // ademas esta en el arbol
]);
const enMenus = new Set();
for (const lista of Object.values(contribuye.menus || {})) {
    for (const entrada of lista) {
        if (entrada.command) enMenus.add(entrada.command);
    }
}
const escondidos = [...declarados].filter(
    c => !usados.includes(c) && !enMenus.has(c) && !SOLO_DE_DENTRO.has(c));
exigir(escondidos.length === 0,
       'ningun comando queda solo en la paleta',
       escondidos.join(', '));

// --- Los iconos de las filas son codicons reales -------------------------
// Un nombre inventado no falla: la fila sale sin icono y descuadrada.
const iconos = [...arbol.matchAll(/icono:\s*'([^']+)'/g)].map(m => m[1]);
exigir(iconos.every(i => /^[a-z0-9-]+$/.test(i)),
       'los iconos tienen forma de codicon',
       iconos.filter(i => !/^[a-z0-9-]+$/.test(i)).join(', '));

// --- Formatear al guardar --------------------------------------------------
// El servidor sabe dar formato (`documentFormattingProvider`), pero eso solo
// pone la orden en el menu: para que se aplique AL GUARDAR hay que pedirlo, y
// se pide solo para Vesta -- tocar el ajuste global de quien instale la
// extension seria meterse donde no llaman.
const cfgDef = contribuye.configurationDefaults || {};
const porLenguaje = cfgDef['[vesta]'] || {};
exigir(porLenguaje['editor.formatOnSave'] === true,
       'se formatea al guardar un .vx');
exigir(porLenguaje['editor.insertSpaces'] === false,
       'y el editor inserta TABULADORES, como manda R1');
exigir(porLenguaje['editor.tabSize'] === 4,
       'con la anchura que el estandar mide (R3)');
exigir(Object.keys(cfgDef).every(k => k.startsWith('[')),
       'los ajustes por defecto son SOLO por lenguaje',
       Object.keys(cfgDef).filter(k => !k.startsWith('[')).join(', '));

console.log('\n' + pasadas + ' comprobaciones pasadas, ' + fallos.length +
            ' fallidas');
for (const f of fallos) {
    console.log('  - ' + f);
}
process.exit(fallos.length === 0 ? 0 : 1);
