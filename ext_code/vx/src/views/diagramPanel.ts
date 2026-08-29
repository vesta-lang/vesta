/**
 * @file diagramPanel.ts
 * @brief Panel que muestra los diagramas del compilador dentro del editor.
 *
 * El compilador ya sabe emitir el diagrama como una pagina HTML autocontenida
 * (estilos y guiones incrustados, sin nada externo) con desplazamiento, zoom,
 * busqueda y panel de detalle por nodo.  La extension no vuelve a dibujar nada:
 * toma esa pagina y la muestra, que es lo que evita duplicar en TypeScript una
 * herramienta que ya existe y que evoluciona con el compilador.
 *
 * Lo unico que se le anade es la politica de seguridad del panel: se autoriza
 * cada guion de la pagina con un valor unico por carga, en vez de abrir la
 * politica entera.
 */

import * as vscode from 'vscode';

import { applyNonceToScripts, createNonce } from '../util/html';

/**
 * @class DiagramPanel
 * @brief Panel reutilizable donde se pintan los diagramas.
 *
 * Se mantiene un unico panel: pedir otro diagrama sustituye el contenido en vez
 * de llenar el editor de pestanas.
 */
export class DiagramPanel {
    /** Panel vivo, si lo hay. */
    private static current: DiagramPanel | undefined;

    /** Tipo con el que se registra el panel en el editor. */
    private static readonly viewType = 'vesta.diagram';

    /**
     * @brief Construye el panel y se suscribe a su cierre.
     * @param panel Panel creado por el editor.
     */
    private constructor(private readonly panel: vscode.WebviewPanel) {
        this.panel.onDidDispose(() => {
            if (DiagramPanel.current === this) {
                DiagramPanel.current = undefined;
            }
        });
    }

    /**
     * @brief Muestra un diagrama, creando el panel si hace falta.
     * @param title Titulo de la pestana.
     * @param html  Pagina autocontenida que devolvio el compilador.
     */
    public static show(title: string, html: string): void {
        const column = vscode.window.activeTextEditor
            ? vscode.ViewColumn.Beside
            : vscode.ViewColumn.One;

        if (!DiagramPanel.current) {
            const panel = vscode.window.createWebviewPanel(
                DiagramPanel.viewType,
                title,
                column,
                {
                    enableScripts: true,
                    // El diagrama guarda el estado de zoom y de los filtros; sin
                    // esto se perderia al cambiar de pestana.
                    retainContextWhenHidden: true,
                    localResourceRoots: [],
                },
            );
            DiagramPanel.current = new DiagramPanel(panel);
        }

        const current = DiagramPanel.current;
        current.panel.title = title;
        current.panel.webview.html = prepareDiagramHtml(html);
        current.panel.reveal(column, false);
    }

    /** @brief Cierra el panel si esta abierto. */
    public static dispose(): void {
        DiagramPanel.current?.panel.dispose();
        DiagramPanel.current = undefined;
    }
}

/**
 * @brief Adapta la pagina del compilador a las reglas del panel.
 *
 * Inserta la politica de seguridad y autoriza los guiones que la pagina ya
 * traia.  No se toca nada mas: el contenido es el que genero el compilador.
 *
 * @param html Pagina de origen.
 * @return Pagina lista para asignar al panel.
 */
function prepareDiagramHtml(html: string): string {
    const nonce = createNonce();
    const csp =
        '<meta http-equiv="Content-Security-Policy" content="' +
        "default-src 'none'; " +
        "img-src data: blob:; " +
        "font-src data:; " +
        "style-src 'unsafe-inline'; " +
        `script-src 'nonce-${nonce}';` +
        '">';

    const withNonce = applyNonceToScripts(html, nonce);
    const headMatch = /<head[^>]*>/i.exec(withNonce);
    if (headMatch) {
        const at = headMatch.index + headMatch[0].length;
        return withNonce.slice(0, at) + csp + withNonce.slice(at);
    }
    // Sin cabecera declarada la politica va delante: el navegador la aplica
    // igual mientras aparezca antes que el primer guion.
    return csp + withNonce;
}
