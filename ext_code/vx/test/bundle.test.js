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
 * Interfaz minima que el editor inyecta.  Solo hace falta lo que se toca al
 * CARGAR el modulo (no al activarlo): constructores y espacios de nombres que
 * el codigo referencia en el cuerpo de sus modulos.
 */
function vscodeFingido() {
    const nada = () => undefined;
    return {
        window: { createOutputChannel: nada, createStatusBarItem: nada },
        workspace: { getConfiguration: nada, workspaceFolders: [] },
        commands: { registerCommand: nada, executeCommand: nada },
        languages: { registerInlayHintsProvider: nada },
        Uri: { file: nada, parse: nada },
        EventEmitter: class { },
        Disposable: class { },
        Position: class { },
        Range: class { },
        Selection: class { },
        InlayHint: class { },
        ViewColumn: { One: 1, Beside: -2 },
        ProgressLocation: { Window: 10 },
        InlayHintKind: { Parameter: 2 },
        TextEditorRevealType: { InCenterIfOutsideViewport: 2 },
        StatusBarAlignment: { Right: 2 },
        ThemeColor: class { },
    };
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
