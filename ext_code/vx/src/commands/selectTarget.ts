/**
 * @file selectTarget.ts
 * @brief Elegir con que se compila lo que ensenan las vistas.
 *
 * Las vistas del compilador no responden por "el codigo": responden por el
 * codigo que sale de UNA combinacion concreta -- arquitectura, nivel de
 * optimizacion, juego de instrucciones de coma flotante, microarquitectura --.
 * Cambiar cualquiera de esas cosas cambia lo que se ve, asi que la eleccion
 * tiene que estar a mano y verse.
 *
 * La lista de opciones NO se escribe aqui: se le pregunta al compilador, que
 * lleva su base de instrucciones con las microarquitecturas que tiene
 * cronometradas.  Una lista escrita en la extension envejeceria en cuanto
 * alguien anadiera una, y el sintoma seria que el editor ofrece menos de lo que
 * el compilador sabe hacer, sin que nada avise.
 */

import * as vscode from 'vscode';

import { VestaLanguageClient, describeError } from '../lsp/client';
import { TargetArch, TargetsResponse, VestaMethod } from '../lsp/protocol';

/** Valor con el que se dice "lo de por defecto" en los ajustes. */
const POR_DEFECTO = '';

/**
 * @brief Registra el comando de eleccion de objetivo.
 * @param context Contexto de la extension.
 * @param client  Cliente del servidor de lenguaje.
 */
export function registerTargetCommand(
    context: vscode.ExtensionContext,
    client: VestaLanguageClient,
): void {
    context.subscriptions.push(
        vscode.commands.registerCommand('vesta.selectTarget', async () => {
            try {
                await elegirObjetivo(client);
            } catch (err) {
                void vscode.window.showErrorMessage(
                    vscode.l10n.t('Vesta: {0}', describeError(err)));
            }
        }),
    );
}

/**
 * @brief Pregunta la combinacion y la guarda en los ajustes.
 * @param client Cliente del servidor de lenguaje.
 */
async function elegirObjetivo(client: VestaLanguageClient): Promise<void> {
    const catalogo = await client.request<TargetsResponse>(VestaMethod.Targets, {});
    if (catalogo.error) {
        void vscode.window.showErrorMessage(vscode.l10n.t('Vesta: {0}', catalogo.error));
        return;
    }
    const arquitecturas = catalogo.architectures ?? [];
    if (arquitecturas.length === 0) {
        void vscode.window.showWarningMessage(
            vscode.l10n.t('Vesta: the compiler declared no architecture at all.'),
        );
        return;
    }

    const arch = await elegirArquitectura(arquitecturas);
    if (arch === undefined) {
        return;
    }
    const elegida = arquitecturas.find(a => a.id === arch);

    const micro = await elegirMicroarquitectura(elegida);
    if (micro === undefined) {
        return;
    }
    const isa = await elegirIsaFlotante(catalogo.floatIsas ?? []);
    if (isa === undefined) {
        return;
    }
    const nivel = await elegirNivel(catalogo.optLevels ?? [0, 1, 2, 3]);
    if (nivel === undefined) {
        return;
    }

    // Se guarda en el espacio de trabajo: es una eleccion del proyecto que se
    // esta mirando, no de la maquina.
    const cfg = vscode.workspace.getConfiguration('vesta');
    const destino = vscode.ConfigurationTarget.Workspace;
    await cfg.update('inspect.arch', arch, destino);
    await cfg.update('inspect.cpu', micro, destino);
    await cfg.update('inspect.floatIsa', isa, destino);
    await cfg.update('inspect.opt', nivel, destino);

    const partes = [
        arch || vscode.l10n.t('default architecture'),
        micro || vscode.l10n.t('no microarchitecture pinned'),
        isa || vscode.l10n.t('default floating point'),
        nivel === POR_DEFECTO
            ? vscode.l10n.t('default optimisation')
            : vscode.l10n.t('optimisation O{0}', nivel),
    ];
    void vscode.window.showInformationMessage(
        vscode.l10n.t('Vesta: {0}', partes.join(' | ')));
}

/**
 * @brief Deja elegir la arquitectura.
 * @param arquitecturas Las que declara el compilador.
 * @return El identificador elegido, o undefined si se cancelo.
 */
async function elegirArquitectura(
    arquitecturas: TargetArch[],
): Promise<string | undefined> {
    interface Item extends vscode.QuickPickItem {
        value: string;
    }
    const items: Item[] = [
        {
            label: vscode.l10n.t("The host's"),
            detail: vscode.l10n.t('Without pinning any'),
            value: POR_DEFECTO,
        },
    ];
    for (const a of arquitecturas) {
        items.push({
            label: a.name,
            description: a.id,
            // Decir cuando solo se conoce el juego de instrucciones evita
            // ofrecer una vista que despues no puede responder.
            detail: a.codegen
                ? vscode.l10n.t('{0} microarchitecture(s) with timings', a.microarchs.length)
                : vscode.l10n.t(
                    'no code generator: only its instruction set is known'),
            value: a.id,
        });
    }
    const elegido = await vscode.window.showQuickPick(items, {
        placeHolder: vscode.l10n.t('For which architecture'),
    });
    return elegido?.value;
}

/**
 * @brief Deja elegir la microarquitectura de la arquitectura escogida.
 * @param arch Arquitectura elegida, si se fijo alguna.
 * @return El nombre elegido, o undefined si se cancelo.
 */
async function elegirMicroarquitectura(
    arch: TargetArch | undefined,
): Promise<string | undefined> {
    const disponibles = arch?.microarchs ?? [];
    if (disponibles.length === 0) {
        // Nada que elegir: se limpia la que hubiera para no arrastrar una de
        // otra arquitectura.
        return POR_DEFECTO;
    }
    interface Item extends vscode.QuickPickItem {
        value: string;
    }
    const items: Item[] = [
        {
            label: vscode.l10n.t('None in particular'),
            detail: vscode.l10n.t('Whatever follows from the instruction set'),
            value: POR_DEFECTO,
        },
        ...disponibles.map(m => ({ label: m, value: m })),
    ];
    const elegido = await vscode.window.showQuickPick(items, {
        placeHolder: vscode.l10n.t('Which microarchitecture'),
        matchOnDescription: true,
    });
    return elegido?.value;
}

/**
 * @brief Deja elegir el juego de instrucciones de coma flotante.
 * @param disponibles Los que declara el compilador.
 * @return El elegido, o undefined si se cancelo.
 */
async function elegirIsaFlotante(
    disponibles: string[],
): Promise<string | undefined> {
    interface Item extends vscode.QuickPickItem {
        value: string;
    }
    const items: Item[] = [
        { label: vscode.l10n.t('The default one'), value: POR_DEFECTO },
        ...disponibles.map(i => ({ label: i, value: i })),
    ];
    const elegido = await vscode.window.showQuickPick(items, {
        placeHolder: vscode.l10n.t('Which floating point'),
    });
    return elegido?.value;
}

/**
 * @brief Deja elegir el nivel de optimizacion.
 * @param niveles Los que declara el compilador.
 * @return El nivel como texto ('' = el de por defecto), o undefined si se
 *         cancelo.
 */
async function elegirNivel(niveles: number[]): Promise<string | undefined> {
    interface Item extends vscode.QuickPickItem {
        value: string;
    }
    const explicacion: Record<number, string> = {
        0: vscode.l10n.t('unoptimised: the code just as it was lowered'),
        1: vscode.l10n.t('the basics'),
        2: vscode.l10n.t('the level things are normally compiled at'),
        3: vscode.l10n.t('everything there is'),
    };
    const items: Item[] = [
        { label: vscode.l10n.t('The default one'), value: POR_DEFECTO },
        ...niveles.map(n => ({
            label: `O${n}`,
            detail: explicacion[n] ?? '',
            value: String(n),
        })),
    ];
    const elegido = await vscode.window.showQuickPick(items, {
        placeHolder: vscode.l10n.t('At which optimisation level'),
    });
    return elegido?.value;
}
