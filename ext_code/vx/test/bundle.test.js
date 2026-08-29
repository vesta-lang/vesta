/**
 * @file bundle.test.js
 * @brief Comprueba que el paquete agrupado sigue siendo cargable.
 *
 * Agrupar puede romper cosas que compilan bien: una dependencia que se pide en
 * tiempo de ejecucion y no aparece en el analisis, o el modulo `vscode` colado
 * dentro del paquete cuando lo tiene que poner el editor.  Nada de eso lo ve el
 * comprobador de tipos, y el sintoma llega tarde: la extension no activa en la
 * maquina de otro.
 *
 * Aqui se carga el fichero agrupado en Node con un `vscode` fingido, y se exige
 * que exponga sus dos puntos de entrada.
 *
 * Uso:  node test/bundle.test.js
 */

'use strict';

const fs = require('fs');
const path = require('path');
const Module = require('module');

const RAIZ = path.resolve(__dirname, '..');
const PAQUETE = path.join(RAIZ, 'dist', 'extension.js');

/**
 * Espacio de nombres del editor donde cualquier miembro es una funcion que no
 * hace nada.  Vale para `vscode.window`, `vscode.workspace` y compania.
 * @returns {object} Objeto que responde a cualquier nombre.
 */
function espacioDeNombres() {
    return new Proxy({}, { get: () => () => undefined });
}

/**
 * Interfaz que el editor inyecta, fingida.
 *
 * No se enumera: el cliente del protocolo hereda de clases del editor que van
 * cambiando de version en version, y una lista escrita a mano solo diria que
 * la lista esta incompleta.  Cualquier nombre desconocido se resuelve a una
 * clase vacia, que sirve tanto para instanciar como para heredar; lo que se
 * comprueba aqui es que el paquete CARGA, no que el editor funcione.
 *
 * @returns {object} Un `vscode` suficiente para cargar el modulo.
 */
function vscodeFingido() {
    const conocidos = {
        window: espacioDeNombres(),
        workspace: new Proxy(
            { workspaceFolders: [] },
            { get: (destino, nombre) => (nombre in destino ? destino[nombre] : () => undefined) },
        ),
        commands: espacioDeNombres(),
        languages: espacioDeNombres(),
        env: espacioDeNombres(),
        extensions: espacioDeNombres(),
        Uri: espacioDeNombres(),
        ViewColumn: { One: 1, Beside: -2 },
        ProgressLocation: { Window: 10, Notification: 15 },
        InlayHintKind: { Type: 1, Parameter: 2 },
        TextEditorRevealType: { InCenterIfOutsideViewport: 2 },
        StatusBarAlignment: { Left: 1, Right: 2 },
        version: '1.91.0',
    };
    return new Proxy(conocidos, {
        get: (destino, nombre) =>
            nombre in destino ? destino[nombre] : class { },
    });
}

/** Punto de entrada. */
function main() {
    const fallos = [];
    let pasadas = 0;

    const exigir = (condicion, descripcion, detalle = '') => {
        if (condicion) {
            pasadas++;
            console.log(`  ok    ${descripcion}`);
        } else {
            fallos.push(detalle ? `${descripcion} -- ${detalle}` : descripcion);
            console.log(`  FALLA ${descripcion}${detalle ? ' -- ' + detalle : ''}`);
        }
        // Se devuelve para poder cortar cuando el fallo deja sin sentido lo
        // que viene detras.
        return condicion;
    };

    if (!exigir(fs.existsSync(PAQUETE), 'el fichero agrupado existe',
        'ejecuta antes: npm run build')) {
        process.exit(1);
    }

    // Interceptar la peticion del modulo que inyecta el editor.
    const cargarOriginal = Module._load;
    Module._load = function (peticion, padre, esPrincipal) {
        if (peticion === 'vscode') {
            return vscodeFingido();
        }
        return cargarOriginal.call(this, peticion, padre, esPrincipal);
    };

    let extension;
    try {
        extension = require(PAQUETE);
    } catch (err) {
        exigir(false, 'el fichero agrupado se carga sin fallar', err.message);
        Module._load = cargarOriginal;
        console.log(`${pasadas} comprobaciones pasadas, ${fallos.length} fallidas`);
        process.exit(1);
    }
    Module._load = cargarOriginal;

    exigir(true, 'el fichero agrupado se carga sin fallar');
    exigir(typeof extension.activate === 'function', 'expone activate');
    exigir(typeof extension.deactivate === 'function', 'expone deactivate');

    const texto = fs.readFileSync(PAQUETE, 'utf8');
    exigir(
        texto.includes('vscode-languageclient') || texto.includes('LanguageClient'),
        'el cliente del protocolo viaja dentro del paquete',
        'si no, la extension no arrancaria el servidor',
    );

    /* Las flechas del flujo: lo que se aprendio probandolas.
     *
     * Costo tres intentos y las tres veces el fallo era invisible para el
     * comprobador de tipos.  Antes esto se comprobaba mirando el fuente con
     * expresiones regulares, que fijaba el NOMBRE de las variables y no la
     * propiedad: al renombrarlas la prueba fallaba sin que nada estuviera mal.
     * Ahora se EJECUTA el dibujo -- por eso vive aparte del editor -- y se mira
     * lo que sale.
     */
    const dibujo = require(path.join(RAIZ, 'out', 'flowLayout.js'));

    /* Dos saltos que se solapan, uno corto dentro de uno largo.  Lo que sale,
     * dibujado (las lineas 1 y 9 quedan fuera de los dos):
     *
     *     2   ,-->   a donde llega la rama
     *     3   |
     *     4   | ,-   de donde sale el salto corto
     *     5   | '->  a donde llega
     *     6   |
     *     7   '---   de donde sale la rama
     */
    const saltos = dibujo.repartirCarriles([
        { fromLine: 8, toLine: 3, flow: 'rama' },
        { fromLine: 5, toLine: 6, flow: 'salto' },
    ]);
    exigir(saltos.length === 2, 'se reparten los dos saltos');
    exigir(new Set(saltos.map(s => s.carril)).size === 2,
           'dos saltos que se solapan van en carriles distintos',
           'compartiendo carril sus lineas se confunden en una');

    /* Lo que se dibuja es el DESTINO, no el salto.  Un bloque escrito a mano
     * manda a la misma etiqueta desde varios sitios -- tres `jmp .less` es lo
     * normal --, y una vertical por salto daba tres rayas paralelas que
     * acababan en el mismo punto sin que nada lo dijera.  Aqui, seis saltos a
     * dos etiquetas tienen que salir en DOS trazos, no en seis. */
    const converge = dibujo.repartirCarriles([
        { fromLine: 10, toLine: 40, flow: 'rama' },
        { fromLine: 20, toLine: 40, flow: 'rama' },
        { fromLine: 30, toLine: 40, flow: 'salto' },
        { fromLine: 11, toLine: 50, flow: 'rama' },
        { fromLine: 21, toLine: 50, flow: 'rama' },
        { fromLine: 31, toLine: 50, flow: 'salto' },
    ]);
    exigir(converge.length === 2,
           'los saltos a una misma etiqueta comparten trazo',
           `${converge.length} trazos para 2 destinos`);
    exigir(dibujo.carrilesUsados(converge) === 2,
           'y por tanto ocupan dos carriles, no seis',
           `${dibujo.carrilesUsados(converge)} carriles`);
    /* Un origen a mitad del recorrido no puede pintarse como una vertical
     * cualquiera: el trazo sigue de largo Y ademas sale hacia el codigo.  Sin
     * esa distincion el tramo horizontal salia de la nada. */
    const enMedio = dibujo.dibujarLinea(converge, 19, 2);
    exigir(enMedio.some(c => c.trazo === '\u251c'),
           'un origen a mitad del trazo se dibuja como empalme');

    /* Salidas del bloque: en Vesta un `asm` puede saltar a una funcion del
     * modulo.  El destino no esta en el bloque, asi que no hay flecha entre dos
     * lineas -- pero tampoco es nada: un bloque cuyo unico salto se va a otra
     * funcion salia sin una sola marca, como si no tuviera flujo. */
    const soloSalida = new Set([4]);
    const sinSaltos = dibujo.carrilesUsados([], true);
    exigir(sinSaltos === 1,
           'una salida se dibuja aunque no haya ningun salto interno');
    const marcada = dibujo.dibujarLinea([], 4, sinSaltos, soloSalida);
    exigir(marcada[0].trazo === '\u25c0',
           'la salida apunta hacia FUERA, no al codigo');
    exigir(marcada[marcada.length - 1].trazo !== ' ',
           'y llega hasta el codigo');

    /* La salida va en una columna PROPIA, la de mas afuera.  Metida en el
     * carril 0 tapaba la vertical de un salto que pasara por esa linea y
     * partia la flecha en dos. */
    const conSalto = dibujo.repartirCarriles(
        [{ fromLine: 2, toLine: 9, flow: 'rama' }]);
    const anchoConSalida = dibujo.carrilesUsados(conSalto, true);
    exigir(anchoConSalida === dibujo.carrilesUsados(conSalto) + 1,
           'la salida se lleva su propia columna');
    const cruzada = dibujo.dibujarLinea(conSalto, 5, anchoConSalida,
                                        new Set([5]));
    exigir(cruzada[0].trazo === '\u25c0' && cruzada[1].trazo === '\u253c',
           'y cruza el carril del salto sin borrarlo');

    const carriles = dibujo.carrilesUsados(saltos);
    const lineas = [];
    for (let linea = 1; linea <= 9; linea++) {
        lineas.push(dibujo.dibujarLinea(saltos, linea, carriles));
    }

    /* ESTO es lo que descuadraba el bloque: si una linea sin trazo devolviera
     * menos celdas, el codigo de esa linea quedaria corrido respecto del de la
     * de al lado -- y un bloque de ensamblador esta alineado a mano en
     * columnas --.  La linea 1 esta fuera de todo salto: tiene que medir igual
     * que las demas. */
    const anchuras = new Set(lineas.map(l => l.length));
    exigir(anchuras.size === 1,
           'todas las lineas del bloque miden lo mismo',
           `anchuras distintas: ${[...anchuras].join(', ')}`);
    exigir(lineas[0].every(c => c.trazo === ' '),
           'una linea fuera de todo salto sale en blanco, pero ocupa');

    // Cada extremo tiene que TOCAR el codigo: sin el tramo horizontal se ve
    // una raya que no llega a ninguna instruccion.
    const ultima = l => l[l.length - 1].trazo;
    exigir(ultima(lineas[1]) === '\u25b6',
           'a donde llega un salto se pinta la punta');
    exigir(ultima(lineas[6]) !== ' ',
           'de donde sale un salto tambien toca el codigo');

    // Un color por carril: con varios saltos anidados es lo unico que permite
    // seguir cada uno con la vista.
    exigir(new Set(dibujo.COLORES).size === dibujo.COLORES.length,
           'ningun color se repite entre carriles');
    const colorDe = c => c.color;
    const colores = new Set(
        lineas.flat().filter(c => c.trazo !== ' ').map(colorDe));
    exigir(colores.size === 2,
           'cada salto se dibuja de su color',
           `${colores.size} colores para 2 saltos`);

    const flechas = fs.readFileSync(
        path.join(RAIZ, 'src', 'features', 'flowArrows.ts'), 'utf8');
    exigir(
        !flechas.includes('gutterIconPath'),
        'no se usa el icono de cuneta',
        'ahi el dibujo se encoge hasta no verse',
    );
    /* Al escribir, cada pulsacion cambia el bloque.  Sin esperar se pide una
     * compilacion por tecla y ninguna llega a tiempo; y descartando las que
     * llegan tarde el dibujo se queda en el texto anterior -- las lineas
     * nuevas sin su parte de la cuneta, y un salto recien escrito sin
     * aparecer --.  Se espera, y lo que cambio mientras se preguntaba se
     * vuelve a preguntar. */
    exigir(/alCambiar/.test(flechas) && /setTimeout/.test(flechas),
           'se espera a que pare la mano antes de repintar');
    exigir(/repetir\.(add|delete)/.test(flechas),
           'un cambio durante la peticion se vuelve a preguntar',
           'si no, el dibujo se queda en el texto anterior');

    console.log(`${pasadas} comprobaciones pasadas, ${fallos.length} fallidas`);
    for (const fallo of fallos) {
        console.log(`  - ${fallo}`);
    }
    process.exit(fallos.length === 0 ? 0 : 1);
}

main();
