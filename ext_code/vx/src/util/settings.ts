/**
 * @file settings.ts
 * @brief Lectura de la configuracion de la extension y del documento activo.
 *
 * Los ajustes se leen en el momento de usarlos, no se cachean: el usuario
 * puede cambiar el objetivo de las vistas (sistema, arquitectura, nivel de
 * runtime) entre dos peticiones y lo natural es que la siguiente ya lo use.
 */

import * as vscode from 'vscode';

import { AotTier, DiagramFormat, InspectTarget } from '../lsp/protocol';
import { VESTA_LANGUAGE_ID } from '../lsp/client';

/** @brief Devuelve la configuracion de la extension. */
function config(): vscode.WorkspaceConfiguration {
    return vscode.workspace.getConfiguration('vesta');
}

/**
 * @brief Objetivo de compilacion de las vistas del compilador.
 * @return Sistema y arquitectura; campos vacios significan el anfitrion.
 */
export function inspectTarget(): InspectTarget {
    const cfg = config();
    // El nivel se guarda como texto para poder decir "el de por defecto" sin
    // inventarse un numero que signifique eso.
    const nivel = cfg.get<string>('inspect.opt', '');
    return {
        os: cfg.get<string>('inspect.os', ''),
        arch: cfg.get<string>('inspect.arch', ''),
        opt: nivel === '' ? undefined : Number(nivel),
        floatIsa: cfg.get<string>('inspect.floatIsa', ''),
        cpu: cfg.get<string>('inspect.cpu', ''),
    };
}

/**
 * @brief Anade a una peticion con que se compila lo que se mira.
 * @param params Parametros de la peticion, que se modifican.
 * @param target Objetivo elegido.
 */
export function applyTarget(
    params: Record<string, unknown>,
    target: InspectTarget,
): void {
    params.os = target.os ?? '';
    params.arch = target.arch ?? '';
    if (target.opt !== undefined && !Number.isNaN(target.opt)) {
        params.opt = target.opt;
    }
    if (target.floatIsa) {
        params.floatIsa = target.floatIsa;
    }
    if (target.cpu) {
        params.cpu = target.cpu;
    }
}

/** @brief Nivel de runtime que asumen las vistas de compilacion anticipada. */
export function aotTier(): AotTier {
    return config().get<AotTier>('aot.tier', 'bare');
}

/** @brief Formato con el que se piden los diagramas al servidor. */
export function diagramFormat(): DiagramFormat {
    return config().get<DiagramFormat>('diagram.format', 'html');
}

/** @brief Indica si los diagramas deben anotar el coste de cada funcion. */
export function diagramCost(): boolean {
    return config().get<boolean>('diagram.cost', false);
}

/** @brief Indica si hay que mostrar el nombre de los parametros en las llamadas. */
export function parameterHintsEnabled(): boolean {
    return config().get<boolean>('inlayHints.parameterNames', true);
}

/** @brief Indica si hay que ensenar en el codigo lo que el compilador sabe. */
export function compilerFactsEnabled(): boolean {
    return config().get<boolean>('inlayHints.compilerFacts', true);
}

/**
 * @brief Indica si tambien se ensena lo que el compilador NO pudo saber.
 *
 * Por defecto no: es valioso para auditar, pero puesto en cada linea tapa lo
 * que si se sabe.
 */
export function compilerFactsShowUnknown(): boolean {
    return config().get<boolean>('inlayHints.compilerFactsUnknown', false);
}

/**
 * @brief Como se ejecuta lo que se lanza desde el editor.
 * @return "vm" (interprete), "jit" o "aot" (nativo).
 */
export function runMode(): string {
    return config().get<string>('run.mode', 'jit');
}

/**
 * @brief Nivel de optimizacion con el que se compila lo que se ejecuta.
 * @return El nivel, o undefined para el de por defecto del compilador.
 */
export function runOptLevel(): number | undefined {
    const valor = config().get<string>('run.opt', '');
    return valor === '' ? undefined : Number(valor);
}

/** @brief Indica si se compila con informacion de depuracion. */
export function runDebug(): boolean {
    return config().get<boolean>('run.debug', false);
}

/** @brief Tiempo maximo, en milisegundos, que puede durar una ejecucion. */
export function runTimeoutMs(): number {
    return config().get<number>('run.timeout', 10000);
}

/** @brief Ruta configurada de la maquina virtual; puede ir vacia. */
export function vmPathSetting(): string {
    return config().get<string>('vmPath', '');
}

/**
 * @brief Indica si se dibujan las flechas de flujo sobre el codigo.
 *
 * Encendidas por defecto: es informacion que el compilador ya tiene y que sin
 * dibujar obliga a seguir cada salto a mano.
 */
export function flowArrowsEnabled(): boolean {
    return config().get<boolean>('flowArrows', true);
}

/** @brief Indica si hay que reutilizar el terminal entre ejecuciones. */
export function reuseTerminal(): boolean {
    return config().get<boolean>('reuseTerminal', true);
}

/**
 * El ultimo fichero Vesta que se tuvo delante.
 *
 * Lo que las vistas del compilador ensenan NO es un fichero Vesta: es un
 * volcado, un diagrama o una pagina.  En cuanto se mira una, el fichero activo
 * deja de ser el programa, asi que la siguiente accion no encontraba sobre que
 * trabajar y contestaba "no hay ningun fichero abierto" -- teniendo el fichero
 * a la izquierda, abierto, delante --.  Con la barra lateral eso pasa
 * constantemente, porque el flujo normal es mirar una vista y pedir otra.
 */
let ultimoVesta: vscode.Uri | undefined;

/**
 * @brief Toma nota de que este es el fichero sobre el que se esta trabajando.
 * @param documento Documento que acaba de ponerse delante.
 */
export function recordarDocumentoVesta(documento: vscode.TextDocument): void {
    if (documento.languageId === VESTA_LANGUAGE_ID) {
        ultimoVesta = documento.uri;
    }
}

/**
 * @brief Devuelve el documento Vesta sobre el que trabajar, avisando si no hay.
 *
 * Todas las vistas del compilador trabajan sobre un fichero concreto, asi que
 * este es el primer paso de casi todos los comandos.  Se busca en tres sitios,
 * en este orden: el editor activo, los editores visibles, y el ultimo fichero
 * Vesta que se tuvo delante -- que es el caso de siempre cuando la accion se
 * pide desde la barra lateral con una vista abierta --.
 *
 * @return El documento, o undefined si no hay ninguno.
 */
export function activeVestaDocument(): vscode.TextDocument | undefined {
    const activo = vscode.window.activeTextEditor;
    if (activo && activo.document.languageId === VESTA_LANGUAGE_ID) {
        recordarDocumentoVesta(activo.document);
        return activo.document;
    }

    // Alguno de los visibles: con la vista al lado, el programa sigue ahi.
    for (const editor of vscode.window.visibleTextEditors) {
        if (editor.document.languageId === VESTA_LANGUAGE_ID) {
            recordarDocumentoVesta(editor.document);
            return editor.document;
        }
    }

    // El ultimo que se miro, si sigue abierto.
    if (ultimoVesta) {
        const guardado = vscode.workspace.textDocuments.find(
            d => d.uri.toString() === ultimoVesta?.toString(),
        );
        if (guardado) {
            return guardado;
        }
        ultimoVesta = undefined; // se cerro: no se insiste
    }

    void vscode.window.showWarningMessage(
        'Vesta: abre un fichero .vx para poder mirar lo que hace el compilador con el.',
    );
    return undefined;
}
