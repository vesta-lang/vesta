/**
 * @file cells.ts
 * @brief Ejecutar trozos de un fichero Vesta y ver el resultado al lado.
 *
 * La idea es la del cuaderno: escribir algo, ejecutarlo y ver lo que sale sin
 * montar un programa aparte.  Pero sin fichero nuevo ni formato nuevo: las
 * celdas se marcan con un COMENTARIO (`// %%`), asi que el fichero sigue
 * siendo un `.vx` normal que compila igual y que otro editor abre igual.
 *
 * Vesta es compilado, asi que una celda no se interpreta: se arma un programa
 * con las declaraciones del fichero mas el trozo elegido, se compila y se
 * ejecuta.  Quien compila es el servidor -- lleva el compilador dentro y ve el
 * texto sin guardar -- y quien ejecuta es la extension, en un proceso aparte:
 * el servidor no puede ejecutar porque su salida estandar es el canal por el
 * que habla con el editor.
 *
 * Tres cosas se eligen y se ven: con que MODO se ejecuta (interprete, JIT o
 * nativo), con que NIVEL de optimizacion, y si se compila con informacion de
 * DEPURACION.  No son detalles: el mismo codigo puede dar otro rendimiento, y
 * a veces otro resultado, segun por donde pase.
 */

import * as cp from 'child_process';
import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';
import * as vscode from 'vscode';

import { VestaLanguageClient } from '../lsp/client';
import { CompileResponse, VestaMethod } from '../lsp/protocol';
import { discoverVestaVm } from '../lsp/discovery';
import {
    runDebug,
    runMode,
    runOptLevel,
    runTimeoutMs,
    vmPathSetting,
} from '../util/settings';
import { VESTA_LANGUAGE_ID } from '../lsp/client';

/** Marca de celda.  Es un comentario: el fichero sigue compilando igual. */
const MARCA = /^\s*\/\/\s*%%(.*)$/;

/** Canal donde se ensena lo que imprimen las celdas. */
let salida: vscode.OutputChannel | undefined;

/**
 * @brief Registra la ejecucion por celdas.
 * @param context Contexto de la extension.
 * @param client  Cliente del servidor de lenguaje.
 */
export function registerCellCommands(
    context: vscode.ExtensionContext,
    client: VestaLanguageClient,
): void {
    salida = vscode.window.createOutputChannel('Vesta: ejecucion');
    context.subscriptions.push(salida);

    context.subscriptions.push(
        vscode.languages.registerCodeLensProvider(
            { scheme: 'file', language: VESTA_LANGUAGE_ID },
            new CellLensProvider(),
        ),
        vscode.commands.registerCommand(
            'vesta.runCell',
            (inicio?: number, fin?: number) => ejecutarCelda(client, inicio, fin),
        ),
        vscode.commands.registerCommand('vesta.runSelection', () =>
            ejecutarSeleccion(client),
        ),
        vscode.commands.registerCommand('vesta.selectRunOptions', () =>
            elegirOpciones(),
        ),
    );
}

/**
 * @class CellLensProvider
 * @brief Pone un boton de ejecucion encima de cada marca de celda.
 */
class CellLensProvider implements vscode.CodeLensProvider {
    /**
     * @brief Busca las marcas y crea un boton por cada una.
     * @param document Documento en curso.
     * @return Los botones.
     */
    public provideCodeLenses(document: vscode.TextDocument): vscode.CodeLens[] {
        const marcas: number[] = [];
        for (let i = 0; i < document.lineCount; i++) {
            if (MARCA.test(document.lineAt(i).text)) {
                marcas.push(i);
            }
        }
        const lentes: vscode.CodeLens[] = [];
        for (let k = 0; k < marcas.length; k++) {
            const inicio = marcas[k] + 1;
            const fin = k + 1 < marcas.length ? marcas[k + 1] - 1 : document.lineCount - 1;
            const titulo = document.lineAt(marcas[k]).text.replace(MARCA, '$1').trim();
            lentes.push(
                new vscode.CodeLens(
                    new vscode.Range(marcas[k], 0, marcas[k], 0),
                    {
                        title: titulo ? `Ejecutar: ${titulo}` : 'Ejecutar celda',
                        command: 'vesta.runCell',
                        arguments: [inicio, fin],
                    },
                ),
            );
        }
        return lentes;
    }
}

/**
 * @brief Ejecuta la celda que va de una linea a otra.
 * @param client Cliente del servidor.
 * @param inicio Primera linea de la celda (contando desde cero).
 * @param fin    Ultima linea de la celda.
 */
async function ejecutarCelda(
    client: VestaLanguageClient,
    inicio?: number,
    fin?: number,
): Promise<void> {
    const editor = vscode.window.activeTextEditor;
    if (!editor || editor.document.languageId !== VESTA_LANGUAGE_ID) {
        void vscode.window.showWarningMessage('Vesta: el fichero activo no es un .vx.');
        return;
    }
    const desde = inicio ?? 0;
    const hasta = fin ?? editor.document.lineCount - 1;
    const rango = new vscode.Range(desde, 0, hasta, Number.MAX_SAFE_INTEGER);
    await ejecutarTrozo(client, editor.document, rango);
}

/**
 * @brief Ejecuta lo que haya seleccionado.
 * @param client Cliente del servidor.
 */
async function ejecutarSeleccion(client: VestaLanguageClient): Promise<void> {
    const editor = vscode.window.activeTextEditor;
    if (!editor || editor.document.languageId !== VESTA_LANGUAGE_ID) {
        void vscode.window.showWarningMessage('Vesta: el fichero activo no es un .vx.');
        return;
    }
    const seleccion = editor.selection;
    if (seleccion.isEmpty) {
        void vscode.window.showInformationMessage(
            'Vesta: selecciona el codigo que quieres ejecutar.',
        );
        return;
    }
    await ejecutarTrozo(client, editor.document, seleccion);
}

/**
 * @brief Arma, compila y ejecuta un trozo del fichero.
 * @param client   Cliente del servidor.
 * @param document Fichero del que sale.
 * @param rango    Trozo a ejecutar.
 */
async function ejecutarTrozo(
    client: VestaLanguageClient,
    document: vscode.TextDocument,
    rango: vscode.Range,
): Promise<void> {
    const trozo = document.getText(rango).trim();
    if (trozo.length === 0) {
        return;
    }
    const modo = runMode();
    const nivel = runOptLevel();
    const depurar = runDebug();

    const canal = salida as vscode.OutputChannel;
    canal.show(true);
    canal.appendLine(
        `\n--- ${new Date().toLocaleTimeString()} | ${nombreDeModo(modo)}` +
        (nivel === undefined ? '' : ` | O${nivel}`) +
        (depurar ? ' | con depuracion' : '') +
        ' ---',
    );

    await vscode.window.withProgress(
        { location: vscode.ProgressLocation.Window, title: 'Vesta: ejecutando...' },
        async () => {
            const programa = componerPrograma(document.getText(), trozo);
            const resultado = await compilarYEjecutar(
                client, programa, modo, nivel, depurar,
            );
            canal.appendLine(resultado.texto);
        },
    );
}

/**
 * @brief Arma el programa: las declaraciones del fichero mas el trozo.
 *
 * El punto de entrada del fichero se QUITA: el que manda es el trozo elegido.
 * Es la unica regla que hay que saber, y decirla es mejor que un fallo raro de
 * "main duplicado" al ejecutar una celda de un fichero que ya tiene programa.
 *
 * @param fichero Texto completo del fichero.
 * @param trozo   Codigo elegido.
 * @return El programa a compilar.
 */
export function componerPrograma(fichero: string, trozo: string): string {
    const declaraciones = quitarMain(fichero);
    // Si el trozo trae su propio punto de entrada, se respeta tal cual.
    if (/^\s*(?:public\s+)?i32\s+main\s*\(/m.test(trozo)) {
        return declaraciones + '\n\n' + trozo + '\n';
    }
    return declaraciones + '\n\n' + envolverEnMain(trozo) + '\n';
}

/**
 * @brief Devuelve el fichero sin su punto de entrada.
 *
 * Se localiza la cabecera y se salta su cuerpo contando llaves, sin parsear:
 * un contador de llaves es exacto aqui porque el cuerpo de una funcion Vesta
 * esta equilibrado, y las llaves de dentro de cadenas o comentarios se
 * descartan al recorrer.
 *
 * @param texto Fichero completo.
 * @return El mismo texto sin la funcion de entrada.
 */
export function quitarMain(texto: string): string {
    const cabecera = /(^|\n)([ \t]*)((?:public\s+)?i32\s+main\s*\([^)]*\)\s*)\{/;
    const m = cabecera.exec(texto);
    if (!m || m.index === undefined) {
        return texto;
    }
    // Posicion de la llave que abre el cuerpo.
    const abre = texto.indexOf('{', m.index + m[1].length);
    if (abre < 0) {
        return texto;
    }
    let profundidad = 0;
    let i = abre;
    let enCadena: string | null = null;
    let enComentarioLinea = false;
    let enComentarioBloque = false;
    for (; i < texto.length; i++) {
        const c = texto[i];
        const siguiente = texto[i + 1];
        if (enComentarioLinea) {
            if (c === '\n') {
                enComentarioLinea = false;
            }
            continue;
        }
        if (enComentarioBloque) {
            if (c === '*' && siguiente === '/') {
                enComentarioBloque = false;
                i++;
            }
            continue;
        }
        if (enCadena) {
            if (c === '\\') {
                i++;
            } else if (c === enCadena) {
                enCadena = null;
            }
            continue;
        }
        if (c === '/' && siguiente === '/') {
            enComentarioLinea = true;
            i++;
            continue;
        }
        if (c === '/' && siguiente === '*') {
            enComentarioBloque = true;
            i++;
            continue;
        }
        if (c === '"' || c === '\'') {
            enCadena = c;
            continue;
        }
        if (c === '{') {
            profundidad++;
        } else if (c === '}') {
            profundidad--;
            if (profundidad === 0) {
                i++;
                break;
            }
        }
    }
    return texto.slice(0, m.index + m[1].length) + texto.slice(i);
}

/**
 * @brief Mete el trozo en un punto de entrada.
 *
 * Si la ultima linea con contenido es una expresion suelta -- ni acaba en `;`
 * ni cierra un bloque -- se imprime.  Es lo que hace que escribir `fib(10)` y
 * darle a ejecutar ensene `55` sin tener que escribir el println.
 *
 * @param trozo Codigo elegido.
 * @return El punto de entrada completo.
 */
export function envolverEnMain(trozo: string): string {
    const lineas = trozo.split(/\r?\n/);
    let ultima = -1;
    for (let i = lineas.length - 1; i >= 0; i--) {
        if (lineas[i].trim().length > 0) {
            ultima = i;
            break;
        }
    }
    if (ultima >= 0) {
        const t = lineas[ultima].trim();
        const esExpresion =
            !t.endsWith(';') && !t.endsWith('{') && !t.endsWith('}') &&
            !t.startsWith('//') && !t.startsWith('#');
        if (esExpresion) {
            lineas[ultima] = `println("\${${t}}");`;
        }
    }
    const cuerpo = lineas.map(l => (l.trim().length > 0 ? '    ' + l : l)).join('\n');
    return `i32 main() {\n${cuerpo}\n    return 0;\n}`;
}

/**
 * @brief Compila el programa y lo ejecuta, y devuelve lo que imprimio.
 * @param client  Cliente del servidor.
 * @param programa Codigo completo.
 * @param modo    "vm" | "jit" | "aot".
 * @param nivel   Nivel de optimizacion, o undefined para el de por defecto.
 * @param depurar Compilar con informacion de depuracion.
 * @return Lo que hay que ensenar.
 */
async function compilarYEjecutar(
    client: VestaLanguageClient,
    programa: string,
    modo: string,
    nivel: number | undefined,
    depurar: boolean,
): Promise<{ texto: string; ok: boolean }> {
    const carpeta = fs.mkdtempSync(path.join(os.tmpdir(), 'vesta-run-'));
    const fuente = path.join(carpeta, 'celda.vx');
    fs.writeFileSync(fuente, programa, 'utf8');

    try {
        // El servidor compila desde disco; hay que abrirle el fichero para que
        // sea ESTE texto el que compile.
        const doc = await vscode.workspace.openTextDocument(vscode.Uri.file(fuente));
        const uri = doc.uri.toString();

        const params: Record<string, unknown> = {
            uri,
            output: path.join(carpeta, 'celda'),
            mode: modo === 'aot' ? 'aot' : 'vm',
            debug: depurar,
        };
        if (nivel !== undefined) {
            params.opt = nivel;
        }
        if (modo === 'aot') {
            params.emit = 'exe';
            params.format = process.platform === 'win32' ? 'pe' : 'elf';
        }

        const compilacion = await client.request<CompileResponse>(
            VestaMethod.Compile,
            params,
        );
        if (compilacion.error) {
            return { texto: compilacion.error, ok: false };
        }
        if (!compilacion.ok || !compilacion.output) {
            const diags = (compilacion.diagnostics ?? [])
                .map(d => `${d.line !== undefined ? `linea ${d.line}: ` : ''}${d.message ?? ''}`)
                .filter(t => t.trim().length > 0);
            return {
                texto: diags.length > 0
                    ? diags.join('\n')
                    : (compilacion.message ?? 'la compilacion no produjo nada'),
                ok: false,
            };
        }

        // El nativo se ejecuta solo; el bytecode lo corre la maquina virtual, y
        // ahi el modo decide si pasa por el JIT o se queda en el interprete.
        if (modo === 'aot') {
            return lanzar(compilacion.output, []);
        }
        const vm = discoverVestaVm(vmPathSetting(), raicesDeBusqueda());
        if (!vm) {
            return {
                texto: 'No se encontro la maquina virtual.  Se fija con vesta.vmPath.',
                ok: false,
            };
        }
        const args = ['--run', compilacion.output];
        if (modo === 'vm') {
            // Interprete puro: sin esto la maquina entra al JIT por su cuenta.
            args.push('-m', 'vm');
        }
        return lanzar(vm.path, args);
    } finally {
        fs.rmSync(carpeta, { recursive: true, force: true });
    }
}

/**
 * @brief Lanza un proceso y recoge lo que imprime, con tiempo maximo.
 * @param exe  Ejecutable.
 * @param args Argumentos.
 * @return Lo impreso y si termino bien.
 */
function lanzar(exe: string, args: string[]): Promise<{ texto: string; ok: boolean }> {
    return new Promise(resolve => {
        const limite = runTimeoutMs();
        const hijo = cp.spawn(exe, args, { windowsHide: true });
        let out = '';
        let err = '';
        let cortado = false;
        const alarma = setTimeout(() => {
            cortado = true;
            hijo.kill();
        }, limite);

        hijo.stdout.on('data', d => (out += d.toString()));
        hijo.stderr.on('data', d => (err += d.toString()));
        hijo.on('error', e => {
            clearTimeout(alarma);
            resolve({ texto: `no se pudo ejecutar: ${e.message}`, ok: false });
        });
        hijo.on('close', codigo => {
            clearTimeout(alarma);
            const texto = [out, err].filter(t => t.length > 0).join('\n').trimEnd();
            if (cortado) {
                resolve({
                    texto:
                        texto +
                        `\n[cortado tras ${limite} ms; se cambia con vesta.run.timeout]`,
                    ok: false,
                });
                return;
            }
            resolve({
                texto: texto.length > 0 ? texto : '(sin salida)',
                ok: codigo === 0,
            });
        });
    });
}

/**
 * @brief Deja elegir modo, nivel de optimizacion y depuracion.
 */
async function elegirOpciones(): Promise<void> {
    interface Item extends vscode.QuickPickItem {
        value: string;
    }
    const modo = await vscode.window.showQuickPick<Item>(
        [
            {
                label: 'Interprete',
                detail: 'La maquina virtual ejecuta el bytecode sin compilar a nativo',
                value: 'vm',
            },
            {
                label: 'JIT',
                detail: 'La maquina virtual compila a nativo lo que se calienta',
                value: 'jit',
            },
            {
                label: 'Nativo',
                detail: 'Se compila a un ejecutable y se lanza',
                value: 'aot',
            },
        ],
        { placeHolder: 'Como se ejecuta' },
    );
    if (!modo) {
        return;
    }
    const nivel = await vscode.window.showQuickPick<Item>(
        [
            { label: 'El de por defecto', value: '' },
            { label: 'O0', detail: 'sin optimizar: el codigo tal y como se bajo', value: '0' },
            { label: 'O1', detail: 'lo basico', value: '1' },
            { label: 'O2', detail: 'el nivel con el que se compila normalmente', value: '2' },
            { label: 'O3', detail: 'todo lo que hay', value: '3' },
        ],
        { placeHolder: 'Con que nivel de optimizacion' },
    );
    if (!nivel) {
        return;
    }
    const depurar = await vscode.window.showQuickPick<Item>(
        [
            { label: 'Sin informacion de depuracion', value: 'no' },
            {
                label: 'Con informacion de depuracion',
                detail: 'Permite parar por linea y ver el fuente al depurar',
                value: 'si',
            },
        ],
        { placeHolder: 'Informacion de depuracion' },
    );
    if (!depurar) {
        return;
    }

    const cfg = vscode.workspace.getConfiguration('vesta');
    const destino = vscode.ConfigurationTarget.Workspace;
    await cfg.update('run.mode', modo.value, destino);
    await cfg.update('run.opt', nivel.value, destino);
    await cfg.update('run.debug', depurar.value === 'si', destino);
    void vscode.window.showInformationMessage(
        `Vesta: ${nombreDeModo(modo.value)}` +
        (nivel.value ? ` | O${nivel.value}` : '') +
        (depurar.value === 'si' ? ' | con depuracion' : ''),
    );
}

/** @brief Nombre legible de un modo de ejecucion. */
function nombreDeModo(modo: string): string {
    if (modo === 'jit') {
        return 'JIT';
    }
    if (modo === 'aot') {
        return 'nativo';
    }
    return 'interprete';
}

/**
 * @brief Carpetas desde las que buscar la maquina virtual.
 * @return Las carpetas del espacio de trabajo.
 */
function raicesDeBusqueda(): string[] {
    const roots: string[] = [];
    for (const folder of vscode.workspace.workspaceFolders ?? []) {
        if (folder.uri.scheme === 'file') {
            roots.push(folder.uri.fsPath);
        }
    }
    return roots;
}
