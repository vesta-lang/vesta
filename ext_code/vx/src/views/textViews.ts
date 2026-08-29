/**
 * @file textViews.ts
 * @brief Documentos virtuales de solo lectura para las vistas en texto.
 *
 * El IR, el bytecode, el ensamblador y los informes del compilador se muestran
 * como documentos normales del editor, no como paneles web: asi se pueden
 * buscar, copiar, comparar y dividir en columnas con las mismas teclas que
 * cualquier otro fichero, sin que la extension tenga que reimplementar nada de
 * eso.  El contenido vive en memoria y se sustituye al volver a pedir la misma
 * vista, de modo que el usuario no acumula pestanas repetidas.
 */

import * as vscode from 'vscode';

/**
 * @class VestaTextViewProvider
 * @brief Proveedor de contenido de los documentos virtuales de la extension.
 */
export class VestaTextViewProvider implements vscode.TextDocumentContentProvider {
    /** Esquema con el que se registran estos documentos. */
    public static readonly scheme = 'vesta-view';

    /** Contenido vivo de cada vista, indexado por su direccion completa. */
    private readonly contents = new Map<string, string>();

    /** Notificador de cambios; el editor recarga la vista al dispararse. */
    private readonly changed = new vscode.EventEmitter<vscode.Uri>();

    /** Evento que el editor observa para refrescar el contenido. */
    public readonly onDidChange = this.changed.event;

    /**
     * @brief Devuelve el contenido de una vista ya generada.
     * @param uri Direccion de la vista.
     * @return El texto guardado, o vacio si la vista ya no existe.
     */
    public provideTextDocumentContent(uri: vscode.Uri): string {
        return this.contents.get(uri.toString()) ?? '';
    }

    /**
     * @brief Publica una vista y la abre en una columna al lado.
     *
     * @param slug       Identificador estable de la vista (ir-post, bytecode...).
     *                   Dos peticiones con el mismo slug y fichero reutilizan la
     *                   misma pestana.
     * @param fileName   Nombre que se mostrara en la pestana, con extension.
     * @param content    Texto a mostrar.
     * @param languageId Lenguaje con el que colorearlo, si se conoce.
     */
    public async show(
        slug: string,
        fileName: string,
        content: string,
        languageId?: string,
    ): Promise<void> {
        const uri = vscode.Uri.parse(
            `${VestaTextViewProvider.scheme}:/${encodeURIComponent(slug)}/${encodeURIComponent(fileName)}`,
        );
        this.contents.set(uri.toString(), content);
        this.changed.fire(uri);

        const document = await vscode.workspace.openTextDocument(uri);
        if (languageId) {
            try {
                await vscode.languages.setTextDocumentLanguage(document, languageId);
            } catch {
                // El lenguaje pedido puede no estar instalado (por ejemplo el
                // ensamblador de la maquina virtual, que va en otra extension).
                // Sin el, el documento se ve igual, solo que sin colores.
            }
        }
        await vscode.window.showTextDocument(document, {
            viewColumn: vscode.ViewColumn.Beside,
            preview: true,
            preserveFocus: false,
        });
    }

    /** @brief Libera los recursos del proveedor. */
    public dispose(): void {
        this.contents.clear();
        this.changed.dispose();
    }
}
