/**
 * @file flowArrows.ts
 * @brief Las flechas del flujo, dibujadas SOBRE el codigo.
 *
 * Leer un bloque de ensamblador escrito a mano es ir saltando: se ve un salto,
 * se busca su etiqueta arriba o abajo, y se vuelve.  Con treinta instrucciones
 * eso se hace con el dedo en la pantalla.
 *
 * Van en el propio editor, no en un panel: en un panel obligan a mirar a otro
 * sitio, y lo que se queria era no apartar la vista del codigo.
 *
 * TRES COSAS QUE COSTARON UN INTENTO CADA UNA:
 *
 * - **Ancho fijo.**  Solo se dibujaba en las lineas que llevaban algo, asi que
 *   una linea con flecha quedaba corrida respecto de la de al lado y el bloque
 *   -- alineado a mano en columnas -- se descuadraba entero.  Ahora TODAS las
 *   lineas del bloque llevan la misma anchura, con espacios donde no hay trazo.
 *
 * - **No en la cuneta.**  La cuneta del editor es de un caracter de ancho: un
 *   dibujo de cinco carriles se encoge ahi hasta no verse.
 *
 * - **El tramo horizontal.**  Con solo la linea vertical se veia QUE habia un
 *   salto y no de donde a donde: el trazo pasaba de largo y no tocaba ninguna
 *   instruccion.  Cada extremo sale de su carril y CORRE hasta el codigo --
 *   cruzando los carriles que haya en medio --, que es como se dibuja esto en
 *   un desensamblador y lo que convierte una raya en una flecha.
 *
 * - **Un trazo por DESTINO, no por salto.**  Tres `jmp .less` daban tres
 *   verticales paralelas que acababan en el mismo punto sin que nada lo
 *   dijera: seis carriles para dos etiquetas.  Ahora se agrupan (ver
 *   `flowLayout.ts`), que ademas devuelve al codigo la anchura sobrante.
 *
 * Quien calcula el flujo es el compilador: ya construye el grafo de cada bloque
 * para analizarlo, asi que aqui solo se reparten los trazos en carriles y se
 * pinta.  Deducirlo por segunda vez seria tener dos ideas de lo mismo -- y por
 * eso un `jmp` a un registro sale sin flecha cuando el compilador no ha podido
 * saber a donde va: el dibujo dice lo que se sabe, ni mas ni menos.
 */

import * as vscode from 'vscode';

import { VESTA_LANGUAGE_ID, VestaLanguageClient } from '../lsp/client';
import { AsmFlowResponse, VestaMethod } from '../lsp/protocol';
import { flowArrowsEnabled, inspectTarget } from '../util/settings';
import {
    Grupo,
    MAX_COLUMNAS,
    carrilesUsados,
    dibujarLinea,
    repartirCarriles,
} from './flowLayout';

/**
 * Lo que se espera tras un cambio antes de volver a preguntar.
 *
 * Corto a proposito: mas alla de esto el dibujo se nota atrasado, y menos
 * convierte cada pulsacion en una compilacion del bloque.
 */
const ESPERA_MS = 150;

/**
 * @class FlowArrowsDecorator
 * @brief Mantiene las flechas de flujo pintadas sobre los bloques de asm.
 */
export class FlowArrowsDecorator {
    /**
     * Un adorno por COLUMNA, creados en orden.
     *
     * El editor pinta los adornos que caen en la misma posicion en el orden en
     * que se crearon sus tipos, asi que crearlos en orden de columna es lo que
     * garantiza que el dibujo salga de izquierda a derecha.  El caracter y el
     * color van en cada rango, no en el tipo: si fueran del tipo haria falta
     * uno por cada combinacion y el orden dejaria de ser el de las columnas.
     */
    private readonly columnas: vscode.TextEditorDecorationType[] = [];

    /** Documentos con una peticion en curso, para no pedir lo mismo dos veces. */
    private readonly enVuelo = new Set<string>();

    /**
     * Documentos que cambiaron MIENTRAS se les preguntaba.
     *
     * Descartar esas peticiones dejaba el dibujo del texto anterior: las lineas
     * nuevas se quedaban sin su parte de la cuneta -- y por tanto sin desplazar
     * --, con lo que el bloque se descuadraba al escribir dentro.  Y un salto
     * recien escrito no aparecia hasta el siguiente cambio.  Se apunta y se
     * vuelve a preguntar al terminar.
     */
    private readonly repetir = new Set<string>();

    /**
     * Espera antes de preguntar, por documento.
     *
     * Escribir son muchos cambios seguidos y cada uno vale una compilacion del
     * bloque: sin esperar, se pide una por pulsacion y ninguna llega a tiempo.
     */
    private readonly esperas = new Map<string, ReturnType<typeof setTimeout>>();

    /**
     * @brief Construye el decorador y reserva sus adornos.
     * @param client Cliente del servidor de lenguaje.
     */
    constructor(private readonly client: VestaLanguageClient) {
        for (let i = 0; i < MAX_COLUMNAS; i++) {
            this.columnas.push(
                vscode.window.createTextEditorDecorationType({
                    rangeBehavior: vscode.DecorationRangeBehavior.ClosedClosed,
                }),
            );
        }
    }

    /** @brief Suelta todos los adornos. */
    public dispose(): void {
        for (const espera of this.esperas.values()) {
            clearTimeout(espera);
        }
        this.esperas.clear();
        for (const tipo of this.columnas) {
            tipo.dispose();
        }
        this.columnas.length = 0;
    }

    /**
     * @brief Repinta tras un cambio, esperando a que pare de escribirse.
     *
     * Cada pulsacion cambia el bloque y valdria una compilacion: sin esperar se
     * pide una por tecla y ninguna llega a tiempo.  Con la espera se pide una
     * sola cuando la mano se detiene, que es cuando se mira.
     *
     * @param editor Editor que cambio.
     */
    public alCambiar(editor: vscode.TextEditor | undefined): void {
        if (!editor) {
            return;
        }
        const clave = editor.document.uri.toString();
        const anterior = this.esperas.get(clave);
        if (anterior !== undefined) {
            clearTimeout(anterior);
        }
        this.esperas.set(
            clave,
            setTimeout(() => {
                this.esperas.delete(clave);
                void this.refresh(editor);
            }, ESPERA_MS),
        );
    }

    /**
     * @brief Vuelve a pintar las flechas de un editor.
     *
     * Si el ajuste esta apagado, o el documento no es Vesta, se limpian: dejar
     * dibujos de un estado anterior es peor que no dibujar.
     *
     * @param editor Editor a repintar.
     */
    public async refresh(editor: vscode.TextEditor | undefined): Promise<void> {
        if (!editor) {
            return;
        }
        if (
            !flowArrowsEnabled() ||
            editor.document.languageId !== VESTA_LANGUAGE_ID ||
            !this.client.isRunning
        ) {
            this.limpiar(editor);
            return;
        }

        const clave = editor.document.uri.toString();
        if (this.enVuelo.has(clave)) {
            // Se apunta para volver a preguntar: el texto de ahora no es el que
            // se esta preguntando.
            this.repetir.add(clave);
            return;
        }
        this.enVuelo.add(clave);
        let respuesta: AsmFlowResponse;
        try {
            respuesta = await this.client.request<AsmFlowResponse>(
                VestaMethod.AsmFlow,
                { uri: clave, arch: inspectTarget().arch ?? '' },
            );
        } catch {
            // Un dibujo que no llega no merece molestar: el codigo se lee igual.
            return;
        } finally {
            this.enVuelo.delete(clave);
            if (this.repetir.delete(clave)) {
                // Cambio mientras se preguntaba: lo que llega ya es viejo.
                void this.refresh(editor);
            }
        }

        const porColumna: vscode.DecorationOptions[][] = this.columnas.map(
            () => [],
        );
        for (const bloque of respuesta.blocks ?? []) {
            this.repartir(editor.document, bloque, porColumna);
        }
        for (let i = 0; i < this.columnas.length; i++) {
            editor.setDecorations(this.columnas[i], porColumna[i]);
        }
    }

    /** @brief Quita las flechas de un editor. */
    private limpiar(editor: vscode.TextEditor): void {
        for (const tipo of this.columnas) {
            editor.setDecorations(tipo, []);
        }
    }

    /**
     * @brief Reparte las lineas de un bloque entre las columnas del dibujo.
     * @param documento Documento en curso.
     * @param bloque    Bloque con sus saltos.
     * @param salida    Rangos por columna.
     */
    private repartir(
        documento: vscode.TextDocument,
        bloque: NonNullable<AsmFlowResponse['blocks']>[number],
        salida: vscode.DecorationOptions[][],
    ): void {
        const grupos = repartirCarriles(bloque.jumps ?? []);
        /* Las lineas por las que el flujo se va a otra funcion del modulo.
         * Cuentan como flujo aunque no haya ningun salto interno: un bloque que
         * acaba en `jmp otra_funcion` salia sin una sola marca. */
        const salidas = new Set<number>(
            (bloque.exits ?? [])
                .filter(e => e.line > 0)
                .map(e => e.line - 1),
        );
        if (grupos.length === 0 && salidas.size === 0) {
            return;
        }
        const carriles = carrilesUsados(grupos, salidas.size > 0);
        const primera = (bloque.firstLine ?? 1) - 1;
        const ultima = (bloque.lastLine ?? 1) - 1;

        for (let linea = primera; linea <= ultima; linea++) {
            if (linea < 0 || linea >= documento.lineCount) {
                continue;
            }
            const celdas = dibujarLinea(grupos, linea, carriles, salidas);
            const ayuda = explicar(grupos, linea, bloque);
            const rango = new vscode.Range(linea, 0, linea, 0);
            /* TODAS las columnas, incluso las vacias: es lo que mantiene la
             * anchura constante y con ella la alineacion del bloque. */
            for (let col = 0; col < celdas.length; col++) {
                salida[col].push({
                    range: rango,
                    hoverMessage: ayuda,
                    renderOptions: {
                        before: {
                            contentText: celdas[col].trazo,
                            color: celdas[col].color,
                            width: '1ch',
                        },
                    },
                });
            }
        }
    }
}

/**
 * @brief Que dice la flecha de esta linea, al posar el cursor.
 *
 * Sobre el destino se dice de CUANTOS sitios se llega, que es lo que el dibujo
 * no puede decir: una etiqueta a la que se salta desde tres puntos tiene una
 * sola vertical, y sin esto no se sabria que son tres.
 *
 * @param grupos Grupos del bloque.
 * @param linea  Linea (contando desde cero).
 * @param bloque Bloque al que pertenece.
 * @return El texto del emergente.
 */
function explicar(
    grupos: Grupo[],
    linea: number,
    bloque: NonNullable<AsmFlowResponse['blocks']>[number],
): vscode.MarkdownString {
    const md = new vscode.MarkdownString();
    for (const grupo of grupos) {
        if (grupo.hasta === linea) {
            const desde = grupo.fuentes
                .map(f => String(f.linea + 1))
                .join(', ');
            parrafo(
                md,
                grupo.fuentes.length === 1
                    ? `aqui llega ${articulo(grupo.fuentes[0].clase)} desde la linea ${desde}`
                    : `aqui se llega desde ${grupo.fuentes.length} sitios: lineas ${desde}`,
            );
        }
        for (const fuente of grupo.fuentes) {
            if (fuente.linea !== linea) {
                continue;
            }
            const sentido = grupo.hasta > fuente.linea ? 'baja' : 'sube';
            parrafo(
                md,
                `${fuente.clase} que ${sentido} a la linea ${grupo.hasta + 1}`,
            );
        }
    }
    /* Por aqui el flujo se va a otra funcion del modulo.  El destino no esta en
     * el bloque, asi que no hay linea a la que apuntar -- pero el nombre si se
     * sabe, y decirlo es la diferencia entre "aqui no pasa nada" y "aqui se
     * acaba el bloque". */
    for (const salida of bloque.exits ?? []) {
        if (salida.line - 1 === linea) {
            parrafo(
                md,
                `de aqui el flujo SALE del bloque, a \`${salida.symbol}\``,
            );
        }
    }
    // Donde el analisis deja de valer, dicho donde se ve el dibujo.
    if (bloque.hasIndirect) {
        parrafo(
            md,
            '_hay un salto indirecto en este bloque -- a un registro, no a una ' +
            'etiqueta --: su destino no se sabe hasta ejecutarlo, asi que ' +
            'faltan flechas_',
        );
    }
    if (bloque.hasUnresolved) {
        parrafo(
            md,
            '_hay un salto a una etiqueta local (`.algo`) que el bloque no ' +
            'define: eso no lo resuelve nadie_',
        );
    }
    return md;
}

/**
 * @brief Anade una linea al emergente como parrafo suelto.
 *
 * En Markdown un salto de linea solo no separa: hacen falta dos.  Se hace aqui
 * para que no aparezca un `\n\n` suelto en cada mensaje.
 *
 * @param md    Emergente que se va construyendo.
 * @param texto Lo que se anade.
 */
function parrafo(md: vscode.MarkdownString, texto: string): void {
    md.appendMarkdown(texto);
    md.appendMarkdown('\n\n');
}

/**
 * @brief El articulo que le toca a una clase de salto.
 * @param clase Clase que informa el servidor.
 * @return La clase con su articulo delante.
 */
function articulo(clase: string): string {
    return clase === 'rama' ? 'una rama' : `un ${clase}`;
}
