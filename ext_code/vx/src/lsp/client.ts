/**
 * @file client.ts
 * @brief Arranque y ciclo de vida del servidor de lenguaje de Vesta.
 *
 * El servidor (`vesta_lsp`) habla el protocolo por la entrada y la salida
 * estandar.  Esta clase lo levanta, mantiene la conexion y ofrece un unico
 * punto por el que pasan las peticiones propias `vesta/*`, para que el resto
 * de la extension no tenga que saber si el servidor esta vivo ni como se
 * arranco.
 *
 * Cuando el binario no aparece, la extension NO se cae: se queda con el
 * resaltado por gramatica y avisa una sola vez, ofreciendo abrir el ajuste
 * correspondiente.  Un editor sin servidor sigue siendo util; uno que lanza
 * errores en cada pulsacion, no.
 */

import * as vscode from 'vscode';
import {
    LanguageClient,
    LanguageClientOptions,
    RevealOutputChannelOn,
    ServerOptions,
    State,
    TransportKind,
} from 'vscode-languageclient/node';

import { BinaryLocation, discoverLanguageServer, discoverStdlib } from './discovery';

/** Identificador del lenguaje que contribuye esta extension. */
export const VESTA_LANGUAGE_ID = 'vesta';

/**
 * @class VestaLanguageClient
 * @brief Envoltura del cliente del protocolo con el ciclo de vida completo.
 */
export class VestaLanguageClient {
    /** Cliente del protocolo; undefined mientras el servidor no esta levantado. */
    private client: LanguageClient | undefined;

    /** Canal donde el servidor escribe su registro. */
    private readonly output: vscode.LogOutputChannel;

    /** Ubicacion del binario en uso, para poder explicarla al usuario. */
    private location: BinaryLocation | undefined;

    /** Biblioteca estandar que ve el servidor; undefined si no aparece. */
    private stdlib: string | undefined;

    /** Evita repetir el aviso de "no encuentro el servidor" en cada arranque. */
    private missingReported = false;

    /**
     * @brief Construye el cliente sin arrancar nada todavia.
     * @param context Contexto de la extension, para resolver rutas propias.
     */
    constructor(private readonly context: vscode.ExtensionContext) {
        this.output = vscode.window.createOutputChannel('Vesta', { log: true });
        context.subscriptions.push(this.output);
    }

    /** @brief Indica si hay un servidor corriendo y listo para atender. */
    public get isRunning(): boolean {
        return this.client !== undefined && this.client.state === State.Running;
    }

    /** @brief Ubicacion del binario en uso, si el servidor esta levantado. */
    public get binaryLocation(): BinaryLocation | undefined {
        return this.location;
    }

    /**
     * @brief Biblioteca estandar que ve el servidor.
     *
     * Es la que resuelve los `import std.*`, y por tanto la que se abre al ir a
     * la definicion de un simbolo de la biblioteca.
     *
     * @return Ruta del directorio, o undefined si no se localizo.
     */
    public get stdlibPath(): string | undefined {
        return this.stdlib ?? this.resolveStdlib();
    }

    /** @brief Carpetas desde las que se busca; publicas para los comandos. */
    public get searchRootsForTools(): string[] {
        return this.searchRoots();
    }

    /** @brief Muestra el canal de registro del servidor. */
    public showLog(): void {
        this.output.show(true);
    }

    /**
     * @brief Levanta el servidor si esta habilitado y el binario aparece.
     * @return true si el servidor quedo arrancado.
     */
    public async start(): Promise<boolean> {
        if (this.client) {
            return this.isRunning;
        }

        const config = vscode.workspace.getConfiguration('vesta');
        if (!config.get<boolean>('server.enable', true)) {
            this.output.appendLine(
                'El servidor de lenguaje esta desactivado (vesta.server.enable). ' +
                'Solo queda el resaltado por gramatica.',
            );
            return false;
        }

        const location = discoverLanguageServer(
            config.get<string>('server.path', ''),
            this.searchRoots(),
        );
        if (!location) {
            this.reportMissingServer();
            return false;
        }
        this.location = location;
        this.output.appendLine(
            `Servidor de lenguaje: ${location.path}  (origen: ${location.origin})`,
        );

        const args = config.get<string[]>('server.arguments', []);

        // El servidor localiza la biblioteca estandar por su cuenta (relativa a
        // su propio ejecutable), pero quien desarrolla el compilador suele
        // tener una instalada y otra de trabajo.  Pasar la ruta elegida por su
        // variable de entorno hace que el editor y la compilacion no puedan
        // discrepar sobre cual se esta usando.
        this.stdlib = this.resolveStdlib(location.path);
        const env = { ...process.env };
        if (this.stdlib) {
            env.VX_STDLIB_DIR = this.stdlib;
            this.output.appendLine(`Biblioteca estandar: ${this.stdlib}`);
        } else {
            this.output.appendLine(
                'No se localizo la biblioteca estandar en Vesta; los import std.* ' +
                'pueden quedar sin resolver.  Se puede fijar con vesta.stdlibPath.',
            );
        }

        const serverOptions: ServerOptions = {
            run: { command: location.path, args, options: { env }, transport: TransportKind.stdio },
            debug: { command: location.path, args, options: { env }, transport: TransportKind.stdio },
        };

        const clientOptions: LanguageClientOptions = {
            documentSelector: [{ scheme: 'file', language: VESTA_LANGUAGE_ID }],
            outputChannel: this.output,
            // El servidor ya envia sus fallos como diagnosticos; abrir el panel
            // por cada uno taparia el editor sin aportar nada.
            revealOutputChannelOn: RevealOutputChannelOn.Never,
            synchronize: {
                fileEvents: vscode.workspace.createFileSystemWatcher('**/*.vx'),
            },
            // Para que maquina analizar, desde el primer momento.  Los errores
            // salen de COMPILAR, asi que dependen del objetivo: un modulo que
            // solo existe en Linux, leido desde Windows, no tiene ni sus
            // imports ni sus tipos -- cientos de errores ciertos y sin ningun
            // valor para quien lo esta editando.
            initializationOptions: objetivoDeAnalisis(),
        };

        this.client = new LanguageClient(
            'vesta',
            'Servidor de lenguaje de Vesta',
            serverOptions,
            clientOptions,
        );

        try {
            await this.client.start();
            this.output.appendLine('Servidor de lenguaje arrancado.');
            return true;
        } catch (err) {
            this.output.appendLine(`No se pudo arrancar el servidor: ${describeError(err)}`);
            void vscode.window.showErrorMessage(
                `Vesta: no se pudo arrancar el servidor de lenguaje (${describeError(err)}).`,
            );
            this.client = undefined;
            this.location = undefined;
            return false;
        }
    }

    /** @brief Detiene el servidor y libera el cliente. */
    public async stop(): Promise<void> {
        const client = this.client;
        this.client = undefined;
        this.location = undefined;
        this.stdlib = undefined;
        if (!client) {
            return;
        }
        try {
            await client.stop();
        } catch (err) {
            this.output.appendLine(`Fallo al detener el servidor: ${describeError(err)}`);
        }
    }

    /**
     * @brief Le dice al servidor para que maquina analizar.
     *
     * Se llama al cambiar el objetivo en los ajustes.  El servidor tira lo
     * analizado y vuelve a publicar: lo de antes hablaba de otra maquina.
     */
    public notificarObjetivo(): void {
        if (!this.client || !this.isRunning) {
            return;
        }
        void this.client.sendNotification('workspace/didChangeConfiguration', {
            settings: { vesta: { inspect: objetivoDeAnalisis() } },
        });
    }

    /** @brief Detiene el servidor y lo vuelve a levantar con la configuracion actual. */
    public async restart(): Promise<void> {
        await this.stop();
        this.missingReported = false;
        const started = await this.start();
        if (started) {
            void vscode.window.showInformationMessage('Vesta: servidor de lenguaje reiniciado.');
        }
    }

    /**
     * @brief Envia una peticion propia del servidor y devuelve su resultado.
     *
     * El servidor contesta los fallos dentro del resultado (campo `error`), asi
     * que aqui solo se convierten en excepcion los fallos de transporte.
     *
     * @tparam T      Forma esperada de la respuesta.
     * @param method  Nombre completo del metodo, por ejemplo `vesta/ir`.
     * @param params  Parametros de la peticion; siempre llevan `uri`.
     * @return La respuesta del servidor.
     * @throws Error si el servidor no esta corriendo o el transporte falla.
     */
    public async request<T>(method: string, params: Record<string, unknown>): Promise<T> {
        if (!this.client || !this.isRunning) {
            throw new Error(
                'el servidor de lenguaje de Vesta no esta corriendo; ' +
                'revisa el ajuste vesta.server.path',
            );
        }
        return this.client.sendRequest<T>(method, params);
    }

    /**
     * @brief Localiza la biblioteca estandar con la configuracion actual.
     * @param serverPath Ruta del servidor, si ya se conoce.
     * @return Ruta del directorio, o undefined.
     */
    private resolveStdlib(serverPath?: string): string | undefined {
        const configured = vscode.workspace
            .getConfiguration('vesta')
            .get<string>('stdlibPath', '');
        return discoverStdlib(
            configured,
            serverPath ?? this.location?.path,
            this.searchRoots(),
        );
    }

    /**
     * @brief Carpetas desde las que buscar directorios de compilacion.
     *
     * Se incluyen las del espacio de trabajo y la de la propia extension: la
     * primera cubre trabajar sobre un clon del repositorio y la segunda cubre
     * tener la extension dentro de ese mismo clon.
     *
     * @return Lista de carpetas de partida.
     */
    private searchRoots(): string[] {
        const roots: string[] = [];
        for (const folder of vscode.workspace.workspaceFolders ?? []) {
            if (folder.uri.scheme === 'file') {
                roots.push(folder.uri.fsPath);
            }
        }
        roots.push(this.context.extensionPath);
        return roots;
    }

    /** @brief Avisa una sola vez de que el binario del servidor no aparece. */
    private reportMissingServer(): void {
        this.output.appendLine(
            'No se encontro el ejecutable vesta_lsp.  Se ha buscado en el ajuste ' +
            'vesta.server.path, la variable de entorno VESTA_LSP, el PATH, las rutas ' +
            'de instalacion y los directorios de compilacion del repositorio.',
        );
        if (this.missingReported) {
            return;
        }
        this.missingReported = true;
        const configure = 'Configurar la ruta';
        const showLog = 'Ver el registro';
        void vscode.window
            .showWarningMessage(
                'Vesta: no se encontro el servidor de lenguaje (vesta_lsp). ' +
                'El resaltado por gramatica sigue funcionando.',
                configure,
                showLog,
            )
            .then(choice => {
                if (choice === configure) {
                    void vscode.commands.executeCommand(
                        'workbench.action.openSettings',
                        'vesta.server.path',
                    );
                } else if (choice === showLog) {
                    this.showLog();
                }
            });
    }
}

/**
 * @brief El objetivo con el que el servidor tiene que analizar.
 *
 * Es el mismo que eligen las vistas (`vesta.inspect.os` / `vesta.inspect.arch`):
 * mirar el IR de una maquina y que los errores sean de otra seria contarse dos
 * cosas distintas a la vez.  Vacios = la maquina en la que se trabaja.
 *
 * @return Objeto con `os` y `arch`, listo para el servidor.
 */
function objetivoDeAnalisis(): { os: string; arch: string } {
    const cfg = vscode.workspace.getConfiguration('vesta');
    return {
        os: cfg.get<string>('inspect.os', ''),
        arch: cfg.get<string>('inspect.arch', ''),
    };
}

/**
 * @brief Convierte cualquier valor lanzado en un texto legible.
 * @param err Valor capturado en un catch.
 * @return Mensaje descriptivo.
 */
export function describeError(err: unknown): string {
    if (err instanceof Error) {
        return err.message;
    }
    return String(err);
}
