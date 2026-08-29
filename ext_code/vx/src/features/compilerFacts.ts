/**
 * @file compilerFacts.ts
 * @brief Ensena en el propio codigo lo que el compilador sabe de el.
 *
 * El compilador deduce cosas concretas de cada valor -- entre que limites se
 * mueve, si es constante, a donde apunta, con que alineacion queda -- y hasta
 * ahora todo eso solo se podia ver pidiendo un volcado aparte y leyendolo
 * entero.  Aqui aparece al final de la linea de la que habla, mientras se
 * escribe, que es cuando sirve para decidir algo.
 *
 * Dos reglas que no se negocian:
 *
 * - **No se muestra lo que no se sabe.**  El compilador tambien anota donde
 *   miro sin sacar nada; eso es valioso para auditar, pero puesto en cada linea
 *   seria ruido que tapa lo que si dice.  Se puede pedir aparte.
 * - **El ambito viaja con el hecho.**  Uno que solo vale para una arquitectura
 *   o para un backend se marca como tal: ensenarlo sin decirlo seria afirmar de
 *   mas.
 */

import * as vscode from 'vscode';

import { VestaLanguageClient } from '../lsp/client';
import { AsaFact, AsaFactsResponse, VestaMethod } from '../lsp/protocol';
import { compilerFactsEnabled, compilerFactsShowUnknown } from '../util/settings';

/** Tope de hechos que se juntan en una misma linea, para no tapar el codigo. */
const MAX_POR_LINEA = 3;

/**
 * Tope de caracteres de cada etiqueta.  Lo que el catalogo traduce es corto por
 * construccion; lo que llega en crudo puede no serlo, y una anotacion mas larga
 * que la propia linea deja de ayudar.  El texto entero sigue en el emergente.
 */
const MAX_LARGO_ETIQUETA = 46;

/**
 * @class CompilerFactsProvider
 * @brief Lleva los hechos del compilador al final de la linea que los produjo.
 */
export class CompilerFactsProvider implements vscode.InlayHintsProvider {
    /** Notificador de cambios; se dispara al cambiar los ajustes. */
    private readonly changed = new vscode.EventEmitter<void>();

    /** Evento que el editor observa para volver a pedir las anotaciones. */
    public readonly onDidChangeInlayHints = this.changed.event;

    /**
     * @brief Construye el proveedor sobre un cliente ya creado.
     * @param client Cliente del servidor de lenguaje.
     */
    constructor(private readonly client: VestaLanguageClient) {}

    /** @brief Fuerza al editor a volver a pedir las anotaciones. */
    public refresh(): void {
        this.changed.fire();
    }

    /**
     * @brief Devuelve las anotaciones visibles en el rango pedido.
     * @param document Documento en curso.
     * @param range    Parte del documento que el editor esta mostrando.
     * @return Una anotacion por linea con algo que decir.
     */
    public async provideInlayHints(
        document: vscode.TextDocument,
        range: vscode.Range,
    ): Promise<vscode.InlayHint[]> {
        if (!compilerFactsEnabled() || !this.client.isRunning) {
            return [];
        }

        let response: AsaFactsResponse;
        try {
            response = await this.client.request<AsaFactsResponse>(
                VestaMethod.AsaFacts,
                { uri: document.uri.toString() },
            );
        } catch {
            // Un dato que no llega no es un error que merezca molestar: el
            // codigo se lee igual sin el.
            return [];
        }
        if (response.error) {
            return [];
        }

        const mostrarDesconocidos = compilerFactsShowUnknown();
        // Agrupar por linea: varios hechos de la misma linea se juntan en una
        // sola anotacion, que es como se leen.
        const porLinea = new Map<number, AsaFact[]>();
        for (const hecho of response.facts ?? []) {
            if (hecho.line <= 0 || !hecho.label) {
                continue;
            }
            if (!mostrarDesconocidos && hecho.certainty === 'desconocida') {
                continue;
            }
            const indice = hecho.line - 1;
            if (indice < range.start.line || indice > range.end.line) {
                continue;
            }
            const lista = porLinea.get(indice);
            if (lista) {
                lista.push(hecho);
            } else {
                porLinea.set(indice, [hecho]);
            }
        }

        const anotaciones: vscode.InlayHint[] = [];
        for (const [indice, hechos] of porLinea) {
            if (indice >= document.lineCount) {
                continue;
            }
            // Lo mas corto primero: es lo que el catalogo traduce, o sea lo que
            // se lee de un vistazo.  Se ordena UNA vez y con ese mismo orden se
            // arma tanto lo que se ve como lo que se ensena al posar el cursor,
            // para que uno sea la continuacion del otro y no dos listas.
            const ordenados = [...hechos].sort(
                (a, b) => a.label.length - b.label.length,
            );
            const texto = componerEtiqueta(ordenados);
            if (!texto) {
                continue;
            }
            // Al final de la linea: el codigo se lee igual, y lo anotado queda
            // donde la vista ya se detiene.
            const finalDeLinea = document.lineAt(indice).range.end;
            const anotacion = new vscode.InlayHint(finalDeLinea, texto);
            anotacion.paddingLeft = true;
            anotacion.tooltip = componerDetalle(ordenados);
            anotaciones.push(anotacion);
        }
        return anotaciones;
    }

    /** @brief Libera los recursos del proveedor. */
    public dispose(): void {
        this.changed.dispose();
    }
}

/**
 * @brief Junta los hechos de una linea en una etiqueta corta.
 * @param hechos Hechos de esa linea.
 * @return Texto a mostrar, o vacio si no hay nada que decir.
 */
function componerEtiqueta(hechos: AsaFact[]): string {
    const vistos = new Set<string>();
    const partes: string[] = [];
    for (const hecho of hechos) {
        // Dos analisis pueden llegar a lo mismo; decirlo dos veces no aporta.
        if (vistos.has(hecho.label)) {
            continue;
        }
        vistos.add(hecho.label);
        partes.push(acortar(hecho.label) + marcaDeAmbito(hecho));
        if (partes.length >= MAX_POR_LINEA) {
            break;
        }
    }
    return partes.length > 0 ? partes.join('  ') : '';
}

/**
 * @brief Recorta una etiqueta para que quepa al final de la linea.
 * @param texto Etiqueta completa.
 * @return La etiqueta, con puntos suspensivos si hubo que cortarla.
 */
function acortar(texto: string): string {
    return texto.length <= MAX_LARGO_ETIQUETA
        ? texto
        : texto.slice(0, MAX_LARGO_ETIQUETA - 3) + '...';
}

/**
 * @brief Marca que acompana a un hecho que solo vale en cierto objetivo.
 * @param hecho Hecho a marcar.
 * @return Sufijo entre parentesis, o vacio si vale en todos.
 */
function marcaDeAmbito(hecho: AsaFact): string {
    const partes = [hecho.isa, hecho.os, hecho.backend].filter(p => p.length > 0);
    return partes.length > 0 ? ` (${partes.join('/')})` : '';
}

/**
 * @brief Detalle que se ensena al posar el cursor sobre la anotacion.
 *
 * Lleva SIEMPRE el texto completo de cada hecho, sin recortar, y TODOS los de
 * la linea aunque al final solo se muestren unos pocos: lo que se ve es un
 * resumen, y un resumen sin forma de abrirlo esconde en vez de resumir.
 *
 * @param hechos Hechos de esa linea, en el mismo orden en que se muestran.
 * @return Texto con lo que dice cada uno, quien lo afirma, con que certeza y de
 *         donde sale.
 */
function componerDetalle(hechos: AsaFact[]): vscode.MarkdownString {
    const md = new vscode.MarkdownString();
    hechos.forEach((hecho, indice) => {
        // Marcar los que no caben en la linea, para que se entienda que el
        // emergente no repite lo visible sino que lo completa.
        const oculto = indice >= MAX_POR_LINEA;
        md.appendMarkdown(`**${hecho.label}**${oculto ? '  _(no cabe en la linea)_' : ''}\n\n`);
        md.appendMarkdown(`- analisis: \`${hecho.domain}\`\n`);
        md.appendMarkdown(`- afirma: \`${hecho.code}\`\n`);
        md.appendMarkdown(`- certeza: ${hecho.certainty}\n`);
        md.appendMarkdown(`- procedencia: ${hecho.source}\n`);
        const ambito = [hecho.isa, hecho.os, hecho.backend].filter(p => p);
        if (ambito.length > 0) {
            md.appendMarkdown(`- solo en: ${ambito.join(' / ')}\n`);
        }
        md.appendMarkdown('\n');
    });
    return md;
}
