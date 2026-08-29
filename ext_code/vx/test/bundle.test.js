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

    console.log(`${pasadas} comprobaciones pasadas, ${fallos.length} fallidas`);
    for (const fallo of fallos) {
        console.log(`  - ${fallo}`);
    }
    process.exit(fallos.length === 0 ? 0 : 1);
}

main();
