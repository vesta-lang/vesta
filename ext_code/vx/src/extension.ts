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
import { InstructionHoverProvider } from './features/instructionHover';
import { FlowArrowsDecorator } from './features/flowArrows';
import { VestaActionsProvider } from './views/actionsTree';
import { VestaTextViewProvider } from './views/textViews';
import { DiagramPanel } from './views/diagramPanel';
import { MachineViewPanel } from './views/machinePanel';
import { registerInspectCommands } from './commands/inspect';
import { registerNavigationCommands } from './commands/navigation';
import { registerTargetCommand } from './commands/selectTarget';
import { registerCellCommands } from './features/cells';
import { compileActiveFile, forgetTerminal, runActiveFile } from './commands/build';
import { recordarDocumentoVesta } from './util/settings';

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

    // Todo lo que la extension sabe hacer, en la barra lateral.  Estaba solo en
    // la paleta de comandos, que exige saber que existe y como se llama.
    context.subscriptions.push(
        vscode.window.registerTreeDataProvider('vesta.actions',
                                               new VestaActionsProvider()),
        // Se apunta cual es el programa que se esta mirando.  Las vistas del
        // compilador no son ficheros Vesta, asi que al abrir una el "fichero
        // activo" deja de serlo y la siguiente accion se quedaria sin sobre
        // que trabajar.
        vscode.window.onDidChangeActiveTextEditor(editor => {
            if (editor) {
                recordarDocumentoVesta(editor.document);
            }
        }),
    );
    if (vscode.window.activeTextEditor) {
        recordarDocumentoVesta(vscode.window.activeTextEditor.document);
    }

    // Documentos virtuales donde se muestran las vistas en texto.
    const views = new VestaTextViewProvider();
    context.subscriptions.push(
        vscode.workspace.registerTextDocumentContentProvider(VestaTextViewProvider.scheme, views),
        new vscode.Disposable(() => views.dispose()),
    );

    registerInspectCommands(context, { client, views });
    registerNavigationCommands(context, client);
    registerTargetCommand(context, client);
    // Ejecutar trozos del propio fichero y ver la salida al lado, sin montar un
    // programa aparte.
    registerCellCommands(context, client);

    /* Las flechas del flujo, SOBRE el codigo.  No es una vista aparte: se
     * dibujan en el propio editor, encima de los bloques de asm, porque es
     * donde se leen. */
    const flechas = new FlowArrowsDecorator(client);
    context.subscriptions.push(
        new vscode.Disposable(() => flechas.dispose()),
        vscode.window.onDidChangeActiveTextEditor(e => void flechas.refresh(e)),
        vscode.workspace.onDidChangeTextDocument(ev => {
            const editor = vscode.window.activeTextEditor;
            if (editor && ev.document === editor.document) {
                void flechas.refresh(editor);
            }
        }),
    );
    void flechas.refresh(vscode.window.activeTextEditor);

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
        // Dentro de un bloque asm, lo que cuesta cada instruccion segun la
        // base que el propio compilador consulta para planificarlas.
        vscode.languages.registerHoverProvider(
            selector,
            new InstructionHoverProvider(activeClient),
        ),
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
            // El objetivo decide tambien los errores, no solo las vistas: hay
            // que decirselo al servidor para que reanalice.
            if (event.affectsConfiguration('vesta.inspect.os') ||
                event.affectsConfiguration('vesta.inspect.arch')) {
                activeClient.notificarObjetivo();
                compilerFacts.refresh();
                void flechas.refresh(vscode.window.activeTextEditor);
            }
            if (event.affectsConfiguration('vesta.flowArrows')) {
                void flechas.refresh(vscode.window.activeTextEditor);
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
