/**
 * @file navigation.ts
 * @brief Navegacion hacia los modulos: la biblioteca estandar y los imports.
 *
 * Ir a la definicion de un simbolo ya lo resuelve el servidor de lenguaje, y
 * llega tanto a los simbolos del propio fichero como a los de los modulos
 * importados.  Lo que el protocolo no cubre es saltar al FICHERO de un import:
 * ahi no hay ningun simbolo bajo el cursor, sino una ruta de modulo.  Estos
 * comandos cierran ese hueco y, de paso, dejan ver donde vive de verdad cada
 * pieza, que es la pregunta que aparece en cuanto hay dos copias de la
 * biblioteca en la misma maquina.
 */

import * as fs from 'fs';
import * as path from 'path';
import * as vscode from 'vscode';

import { VestaLanguageClient } from '../lsp/client';
import { discoverVestaVm } from '../lsp/discovery';
import { vmPathSetting } from '../util/settings';

/** Tope de ficheros que se listan al explorar la biblioteca. */
const MAX_STDLIB_FILES = 2000;

/**
 * @brief Registra los comandos de navegacion.
 * @param context Contexto de la extension.
 * @param client  Cliente del servidor de lenguaje.
 */
export function registerNavigationCommands(
    context: vscode.ExtensionContext,
    client: VestaLanguageClient,
): void {
    context.subscriptions.push(
        vscode.commands.registerCommand('vesta.openStdlib', () => openStdlibModule(client)),
        vscode.commands.registerCommand('vesta.openImport', () => openImportUnderCursor(client)),
        vscode.commands.registerCommand('vesta.showPaths', () => showPaths(client)),
    );
}

/**
 * @brief Deja elegir un modulo de la biblioteca estandar y lo abre.
 * @param client Cliente del servidor de lenguaje.
 */
async function openStdlibModule(client: VestaLanguageClient): Promise<void> {
    const stdlib = client.stdlibPath;
    if (!stdlib) {
        await reportMissingStdlib();
        return;
    }

    const files = collectVestaFiles(stdlib);
    if (files.length === 0) {
        void vscode.window.showWarningMessage(
            `Vesta: no hay ningun modulo .vx en ${stdlib}.`,
        );
        return;
    }

    interface Item extends vscode.QuickPickItem {
        file: string;
    }
    const items: Item[] = files.map(file => {
        const relative = path.relative(stdlib, file);
        return {
            // El nombre del modulo tal y como se escribe en el import.
            label: moduleNameFromRelativePath(relative),
            description: relative,
            file,
        };
    });
    items.sort((a, b) => a.label.localeCompare(b.label));

    const choice = await vscode.window.showQuickPick(items, {
        placeHolder: `Modulos de ${stdlib}`,
        matchOnDescription: true,
    });
    if (choice) {
        await openFile(choice.file);
    }
}

/**
 * @brief Abre el fichero del modulo importado en la linea del cursor.
 *
 * Reconoce las dos formas del lenguaje: la ruta con puntos (`import std.io;`)
 * y la ruta entrecomillada de un modulo del proyecto (`import "vx_async"`).
 *
 * @param client Cliente del servidor de lenguaje.
 */
async function openImportUnderCursor(client: VestaLanguageClient): Promise<void> {
    const editor = vscode.window.activeTextEditor;
    if (!editor) {
        return;
    }
    const line = editor.document.lineAt(editor.selection.active.line).text;
    const moduleRef = parseImportLine(line);
    if (!moduleRef) {
        void vscode.window.showInformationMessage(
            'Vesta: el cursor no esta sobre una linea de importacion.',
        );
        return;
    }

    const resolved = resolveModuleFile(moduleRef, editor.document.uri.fsPath, client.stdlibPath);
    if (!resolved) {
        void vscode.window.showWarningMessage(
            `Vesta: no se encontro el fichero del modulo "${moduleRef.name}".`,
        );
        return;
    }
    await openFile(resolved);
}

/**
 * @brief Ensena que binarios y que biblioteca esta usando la extension.
 * @param client Cliente del servidor de lenguaje.
 */
async function showPaths(client: VestaLanguageClient): Promise<void> {
    const server = client.binaryLocation;
    const stdlib = client.stdlibPath;
    const vm = discoverVestaVm(vmPathSetting(), client.searchRootsForTools);

    interface Item extends vscode.QuickPickItem {
        target?: string;
        isDirectory?: boolean;
    }
    const items: Item[] = [
        {
            label: 'Servidor de lenguaje',
            description: server ? server.path : 'no encontrado',
            detail: server ? `Origen: ${server.origin}` : 'Se puede fijar con vesta.server.path',
            target: server?.path,
        },
        {
            label: 'Biblioteca estandar',
            description: stdlib ?? 'no encontrada',
            detail: stdlib
                ? 'Es la que resuelve los import std.* y a la que salta ir a la definicion'
                : 'Se puede fijar con vesta.stdlibPath',
            target: stdlib,
            isDirectory: true,
        },
        {
            label: 'Maquina virtual',
            description: vm ? vm.path : 'no encontrada',
            detail: vm ? `Origen: ${vm.origin}` : 'Se puede fijar con vesta.vmPath',
            target: vm?.path,
        },
    ];

    const choice = await vscode.window.showQuickPick(items, {
        placeHolder: 'Rutas en uso; elige una para abrirla',
    });
    if (!choice?.target) {
        return;
    }
    if (choice.isDirectory) {
        await vscode.commands.executeCommand(
            'revealFileInOS',
            vscode.Uri.file(choice.target),
        );
    } else {
        await vscode.commands.executeCommand('revealFileInOS', vscode.Uri.file(choice.target));
    }
}

/** Referencia a un modulo tal y como aparece en un import. */
interface ModuleRef {
    /** Texto del modulo, sin comillas. */
    name: string;
    /** true si venia entrecomillado (modulo del proyecto, ruta de fichero). */
    quoted: boolean;
}

/**
 * @brief Extrae la referencia de modulo de una linea de importacion.
 * @param line Texto completo de la linea.
 * @return La referencia, o undefined si la linea no es un import.
 */
export function parseImportLine(line: string): ModuleRef | undefined {
    const quoted = /^\s*(?:public\s+|internal\s+)?(?:extern\s+)?import\s+"([^"]+)"/.exec(line);
    if (quoted) {
        return { name: quoted[1], quoted: true };
    }
    const dotted = /^\s*(?:public\s+|internal\s+)?import\s+([A-Za-z_][A-Za-z0-9_.]*)/.exec(line);
    if (dotted) {
        return { name: dotted[1], quoted: false };
    }
    return undefined;
}

/**
 * @brief Traduce una referencia de modulo al fichero que la contiene.
 *
 * Se prueban las mismas raices que usa el compilador: el directorio del
 * fichero que importa y sus ancestros para los modulos del proyecto, y el
 * directorio de la biblioteca para los que empiezan por `std`.
 *
 * @param moduleRef Referencia extraida del import.
 * @param fromFile  Fichero que contiene el import.
 * @param stdlib    Directorio de la biblioteca estandar, si se conoce.
 * @return Ruta del fichero, o undefined si no aparece.
 */
export function resolveModuleFile(
    moduleRef: ModuleRef,
    fromFile: string,
    stdlib: string | undefined,
): string | undefined {
    const relative = moduleRef.quoted
        ? moduleRef.name.replace(/[\\/]/g, path.sep)
        : moduleRef.name.split('.').join(path.sep);

    const roots: string[] = [];
    let dir = path.dirname(fromFile);
    for (let depth = 0; depth < 12; depth++) {
        roots.push(dir);
        const parent = path.dirname(dir);
        if (parent === dir) {
            break;
        }
        dir = parent;
    }
    if (stdlib) {
        roots.push(stdlib);
        // Un `import std.io` se escribe con el prefijo, pero el fichero vive en
        // `<stdlib>/std/io.vx`; anadir el padre cubre las dos escrituras.
        roots.push(path.dirname(stdlib));
    }

    for (const root of roots) {
        const direct = path.join(root, relative + '.vx');
        if (isFile(direct)) {
            return direct;
        }
        // Un modulo puede estar partido en un directorio con su fichero raiz.
        const asDirectory = path.join(root, relative, path.basename(relative) + '.vx');
        if (isFile(asDirectory)) {
            return asDirectory;
        }
    }
    return undefined;
}

/**
 * @brief Recorre un directorio recogiendo los ficheros Vesta.
 * @param root Directorio raiz.
 * @return Rutas de los ficheros encontrados, acotadas por un tope.
 */
function collectVestaFiles(root: string): string[] {
    const found: string[] = [];
    const pending: string[] = [root];
    while (pending.length > 0 && found.length < MAX_STDLIB_FILES) {
        const dir = pending.pop() as string;
        let entries: fs.Dirent[];
        try {
            entries = fs.readdirSync(dir, { withFileTypes: true });
        } catch {
            continue;
        }
        for (const entry of entries) {
            const full = path.join(dir, entry.name);
            if (entry.isDirectory()) {
                pending.push(full);
            } else if (entry.isFile() && entry.name.endsWith('.vx')) {
                found.push(full);
            }
        }
    }
    return found;
}

/**
 * @brief Convierte una ruta relativa de la biblioteca en nombre de modulo.
 * @param relative Ruta relativa al directorio de la biblioteca.
 * @return El nombre tal y como se escribiria en un import.
 */
function moduleNameFromRelativePath(relative: string): string {
    return relative.replace(/\.vx$/, '').split(/[\\/]/).join('.');
}

/** @brief Indica si la ruta existe y es un fichero. */
function isFile(candidate: string): boolean {
    try {
        return fs.statSync(candidate).isFile();
    } catch {
        return false;
    }
}

/**
 * @brief Abre un fichero en el editor, junto al que ya estaba.
 * @param file Ruta del fichero.
 */
async function openFile(file: string): Promise<void> {
    const document = await vscode.workspace.openTextDocument(vscode.Uri.file(file));
    await vscode.window.showTextDocument(document, { preview: false });
}

/** @brief Explica que no hay biblioteca y ofrece configurarla. */
async function reportMissingStdlib(): Promise<void> {
    const configure = 'Configurar la ruta';
    const choice = await vscode.window.showWarningMessage(
        'Vesta: no se localizo la biblioteca estandar. Se busca en VX_STDLIB_DIR, ' +
        'junto al servidor de lenguaje y en el arbol del repositorio.',
        configure,
    );
    if (choice === configure) {
        await vscode.commands.executeCommand('workbench.action.openSettings', 'vesta.stdlibPath');
    }
}
