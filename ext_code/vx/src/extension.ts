/**
 * @file extension.ts
 * @brief Punto de entrada de la extension de Vesta para el editor.
 *
 * Levanta el servidor de lenguaje, registra las vistas del compilador y
 * engancha los comandos.  Todo lo que el servidor ya sabe hacer (diagnosticos,
 * resaltado semantico, navegacion, autocompletado) llega solo por el protocolo;
 * aqui se anade lo que el protocolo no cubre: las vistas propias `vesta/*`, la
 * navegacion a los modulos y la compilacion desde el editor.
 *
 * La extension funciona aunque el servidor no aparezca: en ese caso quedan la
 * gramatica, los fragmentos y la configuracion de edicion.
 */

import * as vscode from 'vscode';

import { VESTA_LANGUAGE_ID, VestaLanguageClient } from './lsp/client';
import { VestaInlayHintsProvider } from './features/paramHints';
import { CompilerFactsProvider } from './features/compilerFacts';
import { VestaTextViewProvider } from './views/textViews';
import { DiagramPanel } from './views/diagramPanel';
import { MachineViewPanel } from './views/machinePanel';
import { registerInspectCommands } from './commands/inspect';
import { registerNavigationCommands } from './commands/navigation';
import { compileActiveFile, forgetTerminal, runActiveFile } from './commands/build';

/** Cliente del servidor de lenguaje mientras la extension esta activa. */
let client: VestaLanguageClient | undefined;

/** Ajustes que obligan a reiniciar el servidor cuando cambian. */
const RESTART_SETTINGS = [
    'vesta.server.path',
    'vesta.server.arguments',
    'vesta.server.enable',
    'vesta.stdlibPath',
];

/**
 * @brief Arranca la extension.
 * @param context Contexto de la extension.
 */
export async function activate(context: vscode.ExtensionContext): Promise<void> {
    client = new VestaLanguageClient(context);

    // Documentos virtuales donde se muestran las vistas en texto.
    const views = new VestaTextViewProvider();
    context.subscriptions.push(
        vscode.workspace.registerTextDocumentContentProvider(VestaTextViewProvider.scheme, views),
        new vscode.Disposable(() => views.dispose()),
    );

    registerInspectCommands(context, { client, views });
    registerNavigationCommands(context, client);

    const activeClient = client;
    context.subscriptions.push(
        vscode.commands.registerCommand('vesta.restartServer', () => activeClient.restart()),
        vscode.commands.registerCommand('vesta.showServerLog', () => activeClient.showLog()),
        vscode.commands.registerCommand('vesta.compile', () => compileActiveFile(activeClient)),
        vscode.commands.registerCommand('vesta.run', () => runActiveFile(activeClient)),
    );

    // El nombre de los parametros en las llamadas viene de un metodo propio del
    // servidor, no del estandar, asi que hay que traducirlo aqui.
    const selector = { scheme: 'file', language: VESTA_LANGUAGE_ID };
    const inlayHints = new VestaInlayHintsProvider(activeClient);
    // Y lo que el compilador deduce de cada valor -- limites, alineacion, a
    // donde apunta -- puesto en la linea de la que habla, mientras se escribe.
    const compilerFacts = new CompilerFactsProvider(activeClient);
    context.subscriptions.push(
        vscode.languages.registerInlayHintsProvider(selector, inlayHints),
        vscode.languages.registerInlayHintsProvider(selector, compilerFacts),
        new vscode.Disposable(() => inlayHints.dispose()),
        new vscode.Disposable(() => compilerFacts.dispose()),
    );

    // Un terminal cerrado a mano no debe volver a usarse.
    context.subscriptions.push(vscode.window.onDidCloseTerminal(forgetTerminal));

    context.subscriptions.push(
        vscode.workspace.onDidChangeConfiguration(async event => {
            if (RESTART_SETTINGS.some(setting => event.affectsConfiguration(setting))) {
                await activeClient.restart();
            }
            if (event.affectsConfiguration('vesta.inlayHints.parameterNames')) {
                inlayHints.refresh();
            }
            if (event.affectsConfiguration('vesta.inlayHints.compilerFacts') ||
                event.affectsConfiguration('vesta.inlayHints.compilerFactsUnknown')) {
                compilerFacts.refresh();
            }
        }),
    );

    await activeClient.start();
}

/**
 * @brief Detiene la extension y cierra el servidor.
 * @return Promesa que termina cuando el servidor ha parado.
 */
export async function deactivate(): Promise<void> {
    DiagramPanel.dispose();
    MachineViewPanel.dispose();
    await client?.stop();
    client = undefined;
}
