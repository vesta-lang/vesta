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
    return {
        os: cfg.get<string>('inspect.os', ''),
        arch: cfg.get<string>('inspect.arch', ''),
    };
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

/** @brief Ruta configurada de la maquina virtual; puede ir vacia. */
export function vmPathSetting(): string {
    return config().get<string>('vmPath', '');
}

/** @brief Indica si hay que reutilizar el terminal entre ejecuciones. */
export function reuseTerminal(): boolean {
    return config().get<boolean>('reuseTerminal', true);
}

/**
 * @brief Devuelve el documento Vesta activo, avisando si no lo hay.
 *
 * Todas las vistas del compilador trabajan sobre un fichero concreto, asi que
 * este es el primer paso de casi todos los comandos.
 *
 * @return El documento activo, o undefined si no hay ninguno en Vesta.
 */
export function activeVestaDocument(): vscode.TextDocument | undefined {
    const editor = vscode.window.activeTextEditor;
    if (!editor) {
        void vscode.window.showWarningMessage('Vesta: no hay ningun fichero abierto.');
        return undefined;
    }
    if (editor.document.languageId !== VESTA_LANGUAGE_ID) {
        void vscode.window.showWarningMessage('Vesta: el fichero activo no es un .vx.');
        return undefined;
    }
    return editor.document;
}
