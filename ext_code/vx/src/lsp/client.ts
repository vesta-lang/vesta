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
     * El servidor esta parado porque se pidio pararlo, y tiene que SEGUIR
     * parado.
     *
     * Sin esta marca, cualquier cambio de ajuste lo volvia a levantar.  Y quien
     * lo para lo hace para RECONSTRUIR el binario, asi que resucitarlo a media
     * compilacion es exactamente el fallo del que se venia huyendo: el
     * enlazador se encuentra el `.exe` abierto y no puede escribirlo.
     */
    private stoppedByUser = false;

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
     * @brief Si esta parado porque alguien lo paro, y no por otra razon.
     *
     * Lo pregunta quien podria arrancarlo por su cuenta -- el observador de
     * ajustes --, para distinguirlo de "no arranco porque no encuentro el
     * binario", donde volver a intentarlo SI es lo correcto.
     */
    public get isStoppedByUser(): boolean {
        return this.stoppedByUser;
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
            this.output.appendLine(vscode.l10n.t(
                'The language server is disabled ({0}).  Only grammar highlighting remains.',
                'vesta.server.enable'));
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
        this.output.appendLine(vscode.l10n.t(
            'Language server: {0}  (found via: {1})', location.path, location.origin));

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
            this.output.appendLine(
                vscode.l10n.t('Standard library: {0}', this.stdlib));
        } else {
            this.output.appendLine(vscode.l10n.t(
                'The Vesta standard library was not located; the std.* imports may stay unresolved.  It can be set with {0}.',
                'vesta.stdlibPath'));
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
            vscode.l10n.t('Vesta language server'),
            serverOptions,
            clientOptions,
        );

        try {
            await this.client.start();
            this.output.appendLine(vscode.l10n.t('Language server started.'));
            return true;
        } catch (err) {
            this.output.appendLine(
                vscode.l10n.t('The server could not be started: {0}', describeError(err)));
            void vscode.window.showErrorMessage(
                vscode.l10n.t('Vesta: the language server could not be started ({0}).',
                              describeError(err)),
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
            this.output.appendLine(
                vscode.l10n.t('Stopping the server failed: {0}', describeError(err)));
        }
    }

    /**
     * @brief Lo para y lo DEJA parado, soltando el binario que tenia abierto.
     *
     * Existe porque pararlo y que nadie lo resucite son dos cosas, y hasta
     * ahora solo estaba la primera: el editor mantiene el `vesta_lsp.exe`
     * abierto mientras corre, asi que reconstruirlo fallaba con un permiso
     * denegado, y matar el proceso a mano no servia -- el cliente lo levanta
     * otra vez en cuanto se le muere, que es lo que tiene que hacer cuando la
     * muerte NO se pidio.
     *
     * Vuelve con "Reiniciar el servidor", que limpia la marca.
     *
     * @return Ruta del binario que queda libre, o undefined si no habia
     *         servidor levantado -- que se distingue a proposito de "lo pare",
     *         porque quien va a reconstruir necesita saber si de verdad se
     *         solto algo.
     */
    public async stopForRebuild(): Promise<string | undefined> {
        const released = this.location?.path;
        this.stoppedByUser = true;
        await this.stop();
        this.output.appendLine(
            released
                ? vscode.l10n.t('Server stopped on request; {0} is now free', released)
                : vscode.l10n.t('Server stopped on request; none was running'),
        );
        return released;
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

    /**
     * @brief Detiene el servidor y lo vuelve a levantar con la configuracion
     *        actual.
     *
     * Es tambien la forma de traerlo de vuelta despues de pararlo: se pide a
     * mano, asi que limpia la marca de "dejalo parado".
     */
    public async restart(): Promise<void> {
        this.stoppedByUser = false;
        await this.stop();
        this.missingReported = false;
        const started = await this.start();
        if (started) {
            void vscode.window.showInformationMessage(
                vscode.l10n.t('Vesta: language server restarted.'));
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
            throw new Error(vscode.l10n.t(
                'the Vesta language server is not running; check the {0} setting',
                'vesta.server.path'));
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
        this.output.appendLine(vscode.l10n.t(
            'The vesta_lsp executable was not found.  It was looked for in the {0} setting, the VESTA_LSP environment variable, the PATH, the install roots and the repository build directories.',
            'vesta.server.path'));
        if (this.missingReported) {
            return;
        }
        this.missingReported = true;
        const configure = vscode.l10n.t('Set the path');
        const showLog = vscode.l10n.t('Show the log');
        void vscode.window
            .showWarningMessage(
                vscode.l10n.t(
                    'Vesta: the language server (vesta_lsp) was not found.  Grammar highlighting keeps working.'),
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
