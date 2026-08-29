/**
 * @file build.ts
 * @brief Comandos de compilacion y de ejecucion del fichero activo.
 *
 * La compilacion la hace el propio servidor de lenguaje, que lleva el
 * compilador embebido: no se lanza ningun proceso, se reutiliza el texto que
 * hay en el editor aunque no este guardado y los fallos vuelven como
 * diagnosticos con su posicion.
 *
 * La ejecucion, en cambio, si necesita un proceso aparte: el servidor no puede
 * ejecutar el programa porque compartiria su salida estandar con el canal del
 * protocolo y lo corromperia.  Por eso el programa se lanza en un terminal del
 * editor, donde ademas se ve su salida en vivo.
 */

import * as path from 'path';
import * as vscode from 'vscode';

import { VestaLanguageClient, describeError } from '../lsp/client';
import { CompileResponse, VestaMethod } from '../lsp/protocol';
import { discoverVestaVm } from '../lsp/discovery';
import {
    activeVestaDocument,
    aotTier,
    inspectTarget,
    reuseTerminal,
    vmPathSetting,
} from '../util/settings';

/** Terminal reutilizado entre ejecuciones consecutivas. */
let sharedTerminal: vscode.Terminal | undefined;

/** Modo de compilacion que el usuario puede elegir. */
interface CompileMode {
    /** Etiqueta mostrada en el selector. */
    label: string;
    /** Explicacion breve del modo. */
    detail: string;
    /** Valor que espera el servidor. */
    mode: 'vm' | 'jit' | 'aot';
}

/** Modos ofrecidos por el comando de compilacion. */
const COMPILE_MODES: CompileMode[] = [
    {
        label: 'Bytecode de la maquina virtual',
        detail: 'Genera un .velb; lo ejecuta el interprete o el JIT.',
        mode: 'vm',
    },
    {
        label: 'Ejecutable nativo',
        detail: 'Compilacion anticipada al formato y la arquitectura configurados.',
        mode: 'aot',
    },
];

/**
 * @brief Olvida el terminal cuando el usuario lo cierra a mano.
 * @param terminal Terminal que se acaba de cerrar.
 */
export function forgetTerminal(terminal: vscode.Terminal): void {
    if (terminal === sharedTerminal) {
        sharedTerminal = undefined;
    }
}

/**
 * @brief Compila el fichero activo preguntando el modo.
 * @param client Cliente del servidor de lenguaje.
 */
export async function compileActiveFile(client: VestaLanguageClient): Promise<void> {
    const document = activeVestaDocument();
    if (!document) {
        return;
    }

    const choice = await vscode.window.showQuickPick(
        COMPILE_MODES.map(m => ({ label: m.label, detail: m.detail, mode: m.mode })),
        { placeHolder: 'Como compilar el fichero' },
    );
    if (!choice) {
        return;
    }

    const result = await compile(client, document, choice.mode);
    if (!result) {
        return;
    }
    reportCompileResult(result);
}

/**
 * @brief Compila el fichero activo y lanza el programa en un terminal.
 * @param client Cliente del servidor de lenguaje.
 */
export async function runActiveFile(client: VestaLanguageClient): Promise<void> {
    const document = activeVestaDocument();
    if (!document) {
        return;
    }

    const result = await compile(client, document, 'vm');
    if (!result) {
        return;
    }
    if (!result.ok || !result.output) {
        reportCompileResult(result);
        return;
    }

    const vm = discoverVestaVm(vmPathSetting(), searchRoots());
    if (!vm) {
        const configure = 'Configurar la ruta';
        const choice = await vscode.window.showErrorMessage(
            'Vesta: no se encontro el ejecutable de la maquina virtual.',
            configure,
        );
        if (choice === configure) {
            await vscode.commands.executeCommand('workbench.action.openSettings', 'vesta.vmPath');
        }
        return;
    }

    const terminal = acquireTerminal();
    terminal.show(true);
    terminal.sendText(buildShellCommand(vm.path, '--run', result.output));
}

/**
 * @brief Pide al servidor que compile un documento.
 * @param client   Cliente del servidor de lenguaje.
 * @param document Documento a compilar.
 * @param mode     Modo de compilacion.
 * @return La respuesta del servidor, o undefined si la peticion fallo.
 */
async function compile(
    client: VestaLanguageClient,
    document: vscode.TextDocument,
    mode: 'vm' | 'jit' | 'aot',
): Promise<CompileResponse | undefined> {
    // El artefacto se deja junto al fuente, con su mismo nombre; el servidor
    // devuelve la ruta final con la extension que corresponda al modo.
    const outputPrefix = path.join(
        path.dirname(document.uri.fsPath),
        path.basename(document.uri.fsPath, path.extname(document.uri.fsPath)),
    );

    const target = inspectTarget();
    const params: Record<string, unknown> = {
        uri: document.uri.toString(),
        output: outputPrefix,
        mode,
    };
    if (mode === 'aot') {
        params.tier = aotTier();
        if (target.arch) {
            params.arch = target.arch;
        }
        // El formato del objeto sale del sistema elegido para las vistas; si no
        // se ha elegido ninguno, decide el compilador segun el anfitrion.
        if (target.os === 'windows') {
            params.format = 'pe';
        } else if (target.os === 'linux') {
            params.format = 'elf';
        }
        params.emit = 'exe';
    }

    return vscode.window.withProgress(
        { location: vscode.ProgressLocation.Window, title: 'Vesta: compilando...' },
        async () => {
            try {
                return await client.request<CompileResponse>(VestaMethod.Compile, params);
            } catch (err) {
                void vscode.window.showErrorMessage(`Vesta: ${describeError(err)}`);
                return undefined;
            }
        },
    );
}

/**
 * @brief Cuenta al usuario como fue la compilacion.
 * @param result Respuesta del servidor.
 */
function reportCompileResult(result: CompileResponse): void {
    if (result.error) {
        void vscode.window.showErrorMessage(`Vesta: ${result.error}`);
        return;
    }
    if (!result.ok) {
        const errors = (result.diagnostics ?? []).length;
        const detail = result.message ? `: ${result.message}` : '';
        void vscode.window.showErrorMessage(
            errors > 0
                ? `Vesta: la compilacion fallo con ${errors} diagnostico(s)${detail}.`
                : `Vesta: la compilacion fallo${detail}.`,
        );
        return;
    }

    const millis = result.frontend_us !== undefined
        ? ` en ${(result.frontend_us / 1000).toFixed(1)} ms`
        : '';
    const output = result.output ?? '';
    const reveal = 'Mostrar el artefacto';
    void vscode.window
        .showInformationMessage(`Vesta: compilado${millis} -> ${output}`, reveal)
        .then(choice => {
            if (choice === reveal && output) {
                void vscode.commands.executeCommand('revealFileInOS', vscode.Uri.file(output));
            }
        });
}

/**
 * @brief Devuelve el terminal de ejecucion, reutilizandolo si procede.
 * @return Terminal listo para recibir ordenes.
 */
function acquireTerminal(): vscode.Terminal {
    if (reuseTerminal() && sharedTerminal && sharedTerminal.exitStatus === undefined) {
        return sharedTerminal;
    }
    sharedTerminal = vscode.window.createTerminal({
        name: 'Vesta',
        message: 'Ejecucion de programas Vesta',
    });
    return sharedTerminal;
}

/**
 * @brief Construye la orden a enviar al terminal segun el interprete de ordenes.
 *
 * En Windows el terminal por defecto es PowerShell, que necesita el operador de
 * llamada para ejecutar una ruta entrecomillada.
 *
 * @param exe  Ruta del ejecutable.
 * @param args Argumentos.
 * @return La orden completa.
 */
function buildShellCommand(exe: string, ...args: string[]): string {
    const quoted = args.map(a => `"${a}"`).join(' ');
    const suffix = quoted.length > 0 ? ` ${quoted}` : '';
    return process.platform === 'win32' ? `& "${exe}"${suffix}` : `"${exe}"${suffix}`;
}

/**
 * @brief Carpetas desde las que buscar directorios de compilacion.
 * @return Las carpetas del espacio de trabajo.
 */
function searchRoots(): string[] {
    const roots: string[] = [];
    for (const folder of vscode.workspace.workspaceFolders ?? []) {
        if (folder.uri.scheme === 'file') {
            roots.push(folder.uri.fsPath);
        }
    }
    return roots;
}
