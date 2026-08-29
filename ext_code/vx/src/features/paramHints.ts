/**
 * @file paramHints.ts
 * @brief Nombre de cada parametro delante de su argumento en las llamadas.
 *
 * El servidor calcula estas pistas con el metodo propio `vesta/paramHints`, no
 * con el metodo estandar de sugerencias incrustadas, asi que la extension las
 * traduce a lo que el editor entiende.  Es una lectura del indice del
 * servidor, sin recompilar.
 */

import * as vscode from 'vscode';

import { VestaLanguageClient } from '../lsp/client';
import { ParamHintsResponse, VestaMethod } from '../lsp/protocol';
import { parameterHintsEnabled } from '../util/settings';

/**
 * @class VestaInlayHintsProvider
 * @brief Adapta las pistas de parametro del servidor al editor.
 */
export class VestaInlayHintsProvider implements vscode.InlayHintsProvider {
    /** Notificador de cambios; se dispara al cambiar el ajuste. */
    private readonly changed = new vscode.EventEmitter<void>();

    /** Evento que el editor observa para volver a pedir las pistas. */
    public readonly onDidChangeInlayHints = this.changed.event;

    /**
     * @brief Construye el proveedor sobre un cliente ya creado.
     * @param client Cliente del servidor de lenguaje.
     */
    constructor(private readonly client: VestaLanguageClient) {}

    /** @brief Fuerza al editor a volver a pedir las pistas. */
    public refresh(): void {
        this.changed.fire();
    }

    /**
     * @brief Devuelve las pistas visibles en el rango pedido.
     * @param document Documento en curso.
     * @param range    Parte del documento que el editor esta mostrando.
     * @return Las pistas de ese rango; lista vacia si no hay servidor.
     */
    public async provideInlayHints(
        document: vscode.TextDocument,
        range: vscode.Range,
    ): Promise<vscode.InlayHint[]> {
        if (!parameterHintsEnabled() || !this.client.isRunning) {
            return [];
        }

        let response: ParamHintsResponse;
        try {
            response = await this.client.request<ParamHintsResponse>(VestaMethod.ParamHints, {
                uri: document.uri.toString(),
            });
        } catch {
            // Una pista que no llega no es un error que merezca molestar: el
            // documento se lee igual sin ella.
            return [];
        }

        const hints: vscode.InlayHint[] = [];
        for (const raw of response.hints ?? []) {
            const position = new vscode.Position(raw.line, raw.character);
            if (!range.contains(position)) {
                continue;
            }
            const hint = new vscode.InlayHint(
                position,
                raw.label,
                vscode.InlayHintKind.Parameter,
            );
            hint.paddingRight = true;
            hints.push(hint);
        }
        return hints;
    }

    /** @brief Libera los recursos del proveedor. */
    public dispose(): void {
        this.changed.dispose();
    }
}
