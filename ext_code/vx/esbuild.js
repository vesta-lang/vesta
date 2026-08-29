/**
 * @file esbuild.js
 * @brief Agrupa la extension en un solo fichero antes de empaquetarla.
 *
 * Sin agrupar, el paquete son casi doscientos ficheros de JavaScript -- casi
 * todos del cliente del protocolo, no nuestros -- y el editor los abre uno a
 * uno al activar la extension.  Agrupados en uno, la activacion deja de
 * depender del numero de ficheros.
 *
 * El modulo `vscode` NO se agrupa: no es una dependencia que se instale, sino
 * la interfaz que el propio editor inyecta en tiempo de ejecucion.  Agruparlo
 * seria meter en el paquete algo que no existe fuera del editor.
 *
 * Uso:
 *   node esbuild.js               agrupa para desarrollar (con mapa de fuente)
 *   node esbuild.js --production  agrupa para publicar (minificado)
 *   node esbuild.js --watch       reagrupa a cada cambio
 */

'use strict';

const esbuild = require('esbuild');

const produccion = process.argv.includes('--production');
const observar = process.argv.includes('--watch');

/** Informa del resultado de cada pasada, tambien en modo observador. */
const informar = {
    name: 'informar',
    setup(build) {
        build.onEnd(resultado => {
            for (const error of resultado.errors) {
                const donde = error.location;
                console.error(
                    donde
                        ? `error: ${error.text}  (${donde.file}:${donde.line}:${donde.column})`
                        : `error: ${error.text}`,
                );
            }
            if (resultado.errors.length === 0) {
                console.log(
                    produccion
                        ? 'agrupado para publicar: dist/extension.js'
                        : 'agrupado: dist/extension.js',
                );
            }
        });
    },
};

/**
 * @brief Construye (o queda observando) el paquete de la extension.
 * @returns {Promise<void>}
 */
async function main() {
    const contexto = await esbuild.context({
        entryPoints: ['src/extension.ts'],
        bundle: true,
        outfile: 'dist/extension.js',
        // El editor carga la extension como modulo de Node en su propio
        // proceso, no en un navegador.
        format: 'cjs',
        platform: 'node',
        // Version de Node del editor soportado; por debajo no hay que bajar.
        target: 'node18',
        external: ['vscode'],
        minify: produccion,
        // En desarrollo el mapa de fuente permite depurar sobre el TypeScript
        // original; en el paquete publicado solo anadiria peso.
        sourcemap: !produccion,
        sourcesContent: false,
        logLevel: 'silent',
        plugins: [informar],
    });

    if (observar) {
        await contexto.watch();
    } else {
        await contexto.rebuild();
        await contexto.dispose();
    }
}

main().catch(err => {
    console.error(err);
    process.exit(1);
});
