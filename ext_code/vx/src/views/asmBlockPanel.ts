/**
 * @file asmBlockPanel.ts
 * @brief Un bloque de ensamblador escrito a mano, con su flujo dibujado.
 *
 * Leer un bloque de asm es ir saltando: se ve un salto, se busca su etiqueta
 * arriba o abajo, y se vuelve.  Con bloques de treinta instrucciones eso se
 * hace con el dedo en la pantalla.
 *
 * El compilador YA construye el grafo de flujo de cada bloque para poder
 * analizarlo -- sabe que instruccion salta a cual --, asi que las flechas no
 * hay que deducirlas aqui: se dibujan las que el ya calculo.  Y con ellas va lo
 * que sabe de cada instruccion: su clase en la base, lo que cuesta en la
 * microarquitectura elegida, que registros y banderas toca, y si nada se puede
 * mover a su alrededor.
 *
 * Lo que el grafo NO pudo resolver -- un salto indirecto, una etiqueta que no
 * esta -- se dice arriba: a partir de ahi las flechas no cuentan todo el flujo,
 * y ensenarlas sin avisar seria afirmar de mas.
 */

import * as vscode from 'vscode';

import { VestaLanguageClient, describeError } from '../lsp/client';
import { AsmBlockResponse, VestaMethod } from '../lsp/protocol';
import { createNonce } from '../util/html';
import { inspectTarget } from '../util/settings';

/** Peticion que la pagina manda al editor. */
interface PanelRequest {
    /** 'refresh' vuelve a pedir; 'reveal' lleva el cursor a una linea. */
    type: 'refresh' | 'reveal';
    line?: number;
}

/**
 * @class AsmBlockPanel
 * @brief Panel unico con el bloque de ensamblador y su flujo.
 */
export class AsmBlockPanel {
    /** Panel vivo, si lo hay. */
    private static current: AsmBlockPanel | undefined;

    /** Tipo con el que se registra el panel en el editor. */
    private static readonly viewType = 'vesta.asmBlock';

    /** Documento del bloque que se esta mirando. */
    private documentUri: vscode.Uri;

    /** Linea de dentro del bloque, para volver a pedirlo. */
    private line: number;

    /**
     * @brief Construye el panel y engancha sus eventos.
     * @param panel  Panel creado por el editor.
     * @param client Cliente del servidor de lenguaje.
     * @param uri    Documento.
     * @param line   Linea de dentro del bloque.
     */
    private constructor(
        private readonly panel: vscode.WebviewPanel,
        private readonly client: VestaLanguageClient,
        uri: vscode.Uri,
        line: number,
    ) {
        this.documentUri = uri;
        this.line = line;

        this.panel.onDidDispose(() => {
            if (AsmBlockPanel.current === this) {
                AsmBlockPanel.current = undefined;
            }
        });

        this.panel.webview.onDidReceiveMessage((message: PanelRequest) => {
            void this.handleRequest(message);
        });
    }

    /**
     * @brief Abre el bloque que contiene una linea.
     * @param client Cliente del servidor de lenguaje.
     * @param uri    Documento.
     * @param line   Linea de dentro del bloque, contando desde uno.
     */
    public static async show(
        client: VestaLanguageClient,
        uri: vscode.Uri,
        line: number,
    ): Promise<void> {
        const column = vscode.window.activeTextEditor
            ? vscode.ViewColumn.Beside
            : vscode.ViewColumn.One;

        if (!AsmBlockPanel.current) {
            const panel = vscode.window.createWebviewPanel(
                AsmBlockPanel.viewType,
                'Vesta: el bloque de ensamblador',
                column,
                { enableScripts: true, retainContextWhenHidden: true, localResourceRoots: [] },
            );
            AsmBlockPanel.current = new AsmBlockPanel(panel, client, uri, line);
            AsmBlockPanel.current.panel.webview.html = buildHtml();
        }

        const current = AsmBlockPanel.current;
        current.documentUri = uri;
        current.line = line;
        current.panel.reveal(column, false);
        await current.refresh();
    }

    /** @brief Cierra el panel si esta abierto. */
    public static dispose(): void {
        AsmBlockPanel.current?.panel.dispose();
        AsmBlockPanel.current = undefined;
    }

    /**
     * @brief Atiende una peticion de la pagina.
     * @param message Peticion.
     */
    private async handleRequest(message: PanelRequest): Promise<void> {
        if (message.type === 'refresh') {
            await this.refresh();
            return;
        }
        if (message.type === 'reveal' && (message.line ?? 0) > 0) {
            const documento = await vscode.workspace.openTextDocument(
                this.documentUri,
            );
            const editor = await vscode.window.showTextDocument(documento, {
                viewColumn: vscode.ViewColumn.One,
                preserveFocus: true,
            });
            const pos = new vscode.Position(Math.max(0, (message.line ?? 1) - 1), 0);
            editor.selection = new vscode.Selection(pos, pos);
            editor.revealRange(
                new vscode.Range(pos, pos),
                vscode.TextEditorRevealType.InCenterIfOutsideViewport,
            );
        }
    }

    /** @brief Vuelve a pedir el bloque y repinta. */
    private async refresh(): Promise<void> {
        this.panel.webview.postMessage({ kind: 'loading' });
        const target = inspectTarget();
        try {
            const response = await this.client.request<AsmBlockResponse>(
                VestaMethod.AsmBlock,
                {
                    uri: this.documentUri.toString(),
                    line: this.line,
                    cpu: target.cpu ?? '',
                    arch: target.arch ?? '',
                },
            );
            this.panel.webview.postMessage({ kind: 'data', payload: response });
        } catch (err) {
            this.panel.webview.postMessage({
                kind: 'error',
                message: describeError(err),
            });
        }
    }
}

/**
 * @brief Construye la pagina del panel.
 * @return El HTML completo.
 */
function buildHtml(): string {
    const nonce = createNonce();
    return `<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta http-equiv="Content-Security-Policy"
      content="default-src 'none'; style-src 'unsafe-inline'; script-src 'nonce-${nonce}';">
<style>
    :root { color-scheme: light dark; }
    body {
        margin: 0;
        font-family: var(--vscode-font-family);
        font-size: 12px;
        color: var(--vscode-editor-foreground);
        background: var(--vscode-editor-background);
        display: flex; flex-direction: column; height: 100vh; overflow: hidden;
    }
    header {
        display: flex; align-items: center; gap: 10px; padding: 6px 10px;
        border-bottom: 1px solid var(--vscode-panel-border, rgba(128,128,128,.35));
        background: var(--vscode-editorWidget-background, transparent);
        flex: 0 0 auto; flex-wrap: wrap;
    }
    button {
        font-family: inherit; font-size: inherit; cursor: pointer;
        color: var(--vscode-button-secondaryForeground, inherit);
        background: var(--vscode-button-secondaryBackground, transparent);
        border: 1px solid var(--vscode-input-border, rgba(128,128,128,.4));
        border-radius: 2px; padding: 2px 6px;
    }
    #maquina { margin-left: auto; opacity: .75; }
    #aviso {
        padding: 4px 10px; flex: 0 0 auto;
        color: var(--vscode-charts-orange, #d29922);
    }
    #cuerpo { display: flex; flex: 1 1 auto; min-height: 0; }
    #codigo { flex: 1 1 auto; overflow: auto; position: relative; padding: 8px 0; }
    .tirador {
        flex: 0 0 4px; cursor: col-resize;
        background: var(--vscode-panel-border, rgba(128,128,128,.25));
    }
    .tirador:hover, .tirador.arrastrando { background: var(--vscode-focusBorder, #3b82f6); }
    #detalle {
        flex: 0 0 22em; overflow: auto; padding: 8px 10px;
    }
    #detalle h3 {
        margin: 0 0 6px; font-size: 11px; text-transform: uppercase;
        letter-spacing: .05em; opacity: .6;
    }
    #detalle table { border-collapse: collapse; }
    #detalle td { padding: 1px 10px 1px 0; vertical-align: top; }
    #detalle td.k { opacity: .6; white-space: nowrap; }

    /* El codigo con su cuneta de flechas. */
    .linea {
        display: flex; white-space: pre; line-height: 1.5; cursor: pointer;
        font-family: var(--vscode-editor-font-family, monospace);
    }
    .linea:hover { background: var(--vscode-list-hoverBackground, rgba(128,128,128,.12)); }
    .linea.sel { background: var(--vscode-list-activeSelectionBackground, rgba(90,150,255,.22)); }
    .linea.destino { background: var(--vscode-editor-findMatchHighlightBackground, rgba(255,190,60,.3)); }
    .nlinea { flex: 0 0 3.5em; text-align: right; padding-right: 8px; opacity: .4; user-select: none; }
    /* La cuneta: aqui se dibujan las flechas.  Monoespaciada a proposito -- las
     * lineas verticales tienen que quedar alineadas entre filas. */
    .flechas {
        flex: 0 0 auto; user-select: none; white-space: pre;
        color: var(--vscode-charts-purple, #c586c0);
    }
    .linea.sel .flechas, .flechas.viva { color: var(--vscode-charts-orange, #d29922); }
    .etiqueta { color: var(--vscode-charts-green, #9cdcfe); font-style: italic; }
    .txt { flex: 1 1 auto; }
    .tk-mnem  { color: var(--vscode-debugTokenExpression-name, #4ec9b0); }
    .tk-reg   { color: var(--vscode-charts-blue, #569cd6); }
    .tk-num   { color: var(--vscode-debugTokenExpression-number, #b5cea8); }
    .tk-sym   { color: var(--vscode-charts-orange, #dcdcaa); }
    .tk-size  { color: var(--vscode-charts-purple, #c586c0); }
    .coste { opacity: .45; padding-left: 2em; }
    .marca { font-size: 10px; padding: 0 5px; border-radius: 8px; margin-right: 4px; }
    .marca.barrera { background: var(--vscode-charts-red, #f85149); color: #2b0a08; }
    .marca.nomod { background: var(--vscode-charts-orange, #d29922); color: #241a02; }
    #mensaje { padding: 16px; }
    #mensaje.error { color: var(--vscode-errorForeground); }
    .oculto { display: none; }
</style>
</head>
<body>
<header>
    <button id="recargar" title="Volver a leer el bloque">Actualizar</button>
    <span id="resumen"></span>
    <span id="maquina"></span>
</header>
<div id="aviso" class="oculto"></div>
<div id="mensaje" class="oculto"></div>
<div id="cuerpo">
    <div id="codigo"></div>
    <div class="tirador" id="tirador"></div>
    <div id="detalle"></div>
</div>
<script nonce="${nonce}">
(function () {
    var api = acquireVsCodeApi();
    var elRecargar = document.getElementById('recargar');
    var elResumen = document.getElementById('resumen');
    var elMaquina = document.getElementById('maquina');
    var elAviso = document.getElementById('aviso');
    var elMensaje = document.getElementById('mensaje');
    var elCuerpo = document.getElementById('cuerpo');
    var elCodigo = document.getElementById('codigo');
    var elDetalle = document.getElementById('detalle');
    var elTirador = document.getElementById('tirador');

    var instrucciones = [];
    var carriles = [];   // un carril por salto, para no cruzarlos
    var anchoCuneta = 0;
    var seleccion = -1;

    elRecargar.addEventListener('click', function () {
        api.postMessage({ type: 'refresh' });
    });

    elTirador.addEventListener('mousedown', function (ev) {
        ev.preventDefault();
        elTirador.classList.add('arrastrando');
        var x0 = ev.clientX;
        var ancho0 = elDetalle.offsetWidth;
        function mover(e) {
            elDetalle.style.flex = '0 0 ' + Math.max(120, ancho0 - (e.clientX - x0)) + 'px';
        }
        function soltar() {
            elTirador.classList.remove('arrastrando');
            document.removeEventListener('mousemove', mover);
            document.removeEventListener('mouseup', soltar);
        }
        document.addEventListener('mousemove', mover);
        document.addEventListener('mouseup', soltar);
    });

    window.addEventListener('message', function (evento) {
        var d = evento.data;
        if (d.kind === 'loading') { mensaje('Leyendo el bloque...', false); return; }
        if (d.kind === 'error') { mensaje(d.message, true); return; }
        var p = d.payload || {};
        if (p.error) { mensaje(p.error, true); return; }
        if (!p.found) {
            mensaje('El cursor no esta dentro de un bloque de ensamblador.', false);
            return;
        }
        instrucciones = p.instructions || [];
        elMensaje.classList.add('oculto');
        elCuerpo.classList.remove('oculto');
        elResumen.textContent = instrucciones.length + ' instrucciones, lineas ' +
            p.firstLine + '-' + p.lastLine;
        elMaquina.textContent = p.isa + '  ' + (p.microarch || '');
        pintarAviso(p);
        calcularCarriles();
        pintar();
    });

    function mensaje(texto, esError) {
        elMensaje.textContent = texto;
        elMensaje.className = esError ? 'error' : '';
        elMensaje.classList.remove('oculto');
        elCuerpo.classList.add('oculto');
        elAviso.classList.add('oculto');
    }

    /** Donde el grafo deja de valer.  Callarlo seria dibujar un flujo a medias
     *  sin decir que lo es. */
    function pintarAviso(p) {
        var partes = [];
        if (p.hasIndirect) {
            partes.push('hay un salto indirecto: su destino no se sabe, asi que ' +
                        'faltan flechas');
        }
        if (p.hasUnresolved) {
            partes.push('hay un salto a una etiqueta que no esta en el bloque');
        }
        if ((p.unknownTerminators || []).length) {
            partes.push('sin clasificar: ' + p.unknownTerminators.join(', '));
        }
        if (partes.length === 0) { elAviso.classList.add('oculto'); return; }
        elAviso.classList.remove('oculto');
        elAviso.textContent = partes.join('   |   ');
    }

    /**
     * Reparte los saltos en CARRILES.
     *
     * Dos saltos que se solapan no pueden compartir columna o sus lineas se
     * confunden en una sola.  Se busca para cada uno el primer carril libre en
     * todo su recorrido, que es lo mismo que hace un desensamblador cuando
     * dibuja esto.
     */
    function calcularCarriles() {
        carriles = [];
        var ocupacion = []; // por carril, lista de tramos [desde, hasta]
        for (var i = 0; i < instrucciones.length; i++) {
            var in_ = instrucciones[i];
            var destino = in_.targetIndex;
            if (destino === undefined || destino === null || destino < 0) {
                carriles.push(-1);
                continue;
            }
            var desde = Math.min(i, destino), hasta = Math.max(i, destino);
            var carril = 0;
            for (;;) {
                var libre = true;
                var tramos = ocupacion[carril] || [];
                for (var t = 0; t < tramos.length; t++) {
                    if (!(hasta < tramos[t][0] || desde > tramos[t][1])) {
                        libre = false;
                        break;
                    }
                }
                if (libre) { break; }
                carril++;
            }
            if (!ocupacion[carril]) { ocupacion[carril] = []; }
            ocupacion[carril].push([desde, hasta]);
            carriles.push(carril);
        }
        anchoCuneta = ocupacion.length;
    }

    /**
     * El dibujo de la cuneta para una fila.
     *
     * Cada carril aporta un caracter: la esquina de salida, la punta de
     * llegada, o la linea que pasa de largo.
     */
    function cuneta(fila) {
        if (anchoCuneta === 0) { return ''; }
        var celdas = [];
        for (var k = 0; k < anchoCuneta; k++) { celdas.push(' '); }
        var vivo = false;
        for (var i = 0; i < instrucciones.length; i++) {
            var carril = carriles[i];
            if (carril < 0) { continue; }
            var destino = instrucciones[i].targetIndex;
            var desde = Math.min(i, destino), hasta = Math.max(i, destino);
            if (fila < desde || fila > hasta) { continue; }
            var c;
            if (fila === i) {
                // De donde SALE el salto.
                c = (destino > i) ? '+' : '+';
            } else if (fila === destino) {
                // A donde LLEGA: la punta mira hacia el codigo.
                c = '>';
            } else {
                c = '|';
            }
            celdas[carril] = c;
            if (i === seleccion || destino === seleccion || fila === seleccion) {
                vivo = true;
            }
        }
        return { texto: celdas.join(''), vivo: vivo };
    }

    function pintar() {
        elCodigo.innerHTML = '';
        for (var i = 0; i < instrucciones.length; i++) {
            elCodigo.appendChild(hacerLinea(i));
        }
        pintarDetalle(seleccion);
    }

    function hacerLinea(i) {
        var in_ = instrucciones[i];
        var div = document.createElement('div');
        div.className = 'linea' + (i === seleccion ? ' sel' : '');
        if (seleccion >= 0) {
            var selDestino = instrucciones[seleccion].targetIndex;
            if (selDestino === i) { div.className += ' destino'; }
        }

        var n = document.createElement('span');
        n.className = 'nlinea';
        n.textContent = in_.line > 0 ? String(in_.line) : '';
        div.appendChild(n);

        var dib = cuneta(i);
        var f = document.createElement('span');
        f.className = 'flechas' + (dib.vivo ? ' viva' : '');
        f.textContent = (dib.texto || '') + ' ';
        div.appendChild(f);

        var t = document.createElement('span');
        t.className = 'txt';
        // Las etiquetas que preceden a esta instruccion, en su propia linea
        // visual: es como estan escritas.
        var etiquetas = in_.labels || [];
        if (etiquetas.length) {
            var e = document.createElement('span');
            e.className = 'etiqueta';
            e.textContent = etiquetas.join(': ') + ':   ';
            t.appendChild(e);
        }
        pintarInstruccion(t, in_.text);
        if (in_.cost) {
            var c = document.createElement('span');
            c.className = 'coste';
            c.textContent = 'lat ' + in_.cost.latency +
                '  cada ' + in_.cost.reciprocalThroughput +
                '  ' + in_.cost.uops + ' uops';
            t.appendChild(c);
        }
        div.appendChild(t);

        div.addEventListener('click', function () {
            seleccion = i;
            pintar();
        });
        div.addEventListener('dblclick', function () {
            if (in_.line > 0) { api.postMessage({ type: 'reveal', line: in_.line }); }
        });
        return div;
    }

    var RE_REG = /^(?:[re]?[abcd]x|[re]?[sd]i|[re]?[sb]p|r(?:8|9|1[0-5])[dwb]?|[abcd][lh]|[xyz]mm\\d+|rip|[wx]\\d+|v\\d+|sp|lr|pc)$/i;
    var RE_TAM = /^(?:byte|word|dword|qword|ptr|rel|short|near|far|lock|rep)$/i;

    /** Colorea una instruccion.  Lo justo para distinguir de un vistazo. */
    function pintarInstruccion(destino, texto) {
        var piezas = texto.split(/([A-Za-z_.$][\\w.$]*|0[xX][0-9a-fA-F]+|\\d+)/);
        var primera = true;
        for (var i = 0; i < piezas.length; i++) {
            var p = piezas[i];
            if (!p) { continue; }
            var clase = '';
            if (i % 2 === 1) {
                if (/^[0-9]/.test(p)) { clase = 'tk-num'; }
                else if (RE_TAM.test(p)) { clase = 'tk-size'; }
                else if (RE_REG.test(p)) { clase = 'tk-reg'; }
                else if (primera) { clase = 'tk-mnem'; }
                else { clase = 'tk-sym'; }
                if (clase !== 'tk-size') { primera = false; }
            }
            if (clase) {
                var s = document.createElement('span');
                s.className = clase;
                s.textContent = p;
                destino.appendChild(s);
            } else {
                destino.appendChild(document.createTextNode(p));
            }
        }
    }

    /** Todo lo que se sabe de la instruccion elegida. */
    function pintarDetalle(i) {
        elDetalle.innerHTML = '';
        if (i < 0 || i >= instrucciones.length) {
            var p = document.createElement('div');
            p.style.opacity = '.6';
            p.textContent = 'Pulsa una instruccion para ver lo que se sabe de ella.';
            elDetalle.appendChild(p);
            return;
        }
        var in_ = instrucciones[i];

        var h = document.createElement('h3');
        h.textContent = in_.text;
        elDetalle.appendChild(h);

        if (in_.barrier) { elDetalle.appendChild(marca('barrera: nada la cruza', 'barrera')); }
        if (in_.modeled === false) {
            elDetalle.appendChild(marca('operandos sin modelar', 'nomod'));
        }

        var filas = [];
        if (!in_.known) {
            filas.push(['la base', 'no conoce esta instruccion']);
        } else if (in_.iclass) {
            filas.push(['clase', in_.iclass]);
        }
        filas.push(['flujo', in_.flow + (in_.target ? ' -> ' + in_.target : '')]);
        if (in_.cost) {
            filas.push(['latencia', String(in_.cost.latency)]);
            filas.push(['se repite cada', String(in_.cost.reciprocalThroughput)]);
            filas.push(['uops', String(in_.cost.uops)]);
        }
        if ((in_.reads || []).length) { filas.push(['lee', in_.reads.join(', ')]); }
        if ((in_.writes || []).length) { filas.push(['escribe', in_.writes.join(', ')]); }
        if (in_.readsMemory) { filas.push(['memoria', 'la lee']); }
        if (in_.writesMemory) { filas.push(['memoria', 'la escribe']); }
        if ((in_.flagsRead || []).length) { filas.push(['lee banderas', in_.flagsRead.join(', ')]); }
        if ((in_.flagsWritten || []).length) { filas.push(['deja banderas', in_.flagsWritten.join(', ')]); }
        if ((in_.labels || []).length) { filas.push(['etiquetas', in_.labels.join(', ')]); }

        var tabla = document.createElement('table');
        for (var k = 0; k < filas.length; k++) {
            var tr = document.createElement('tr');
            var td1 = document.createElement('td');
            td1.className = 'k';
            td1.textContent = filas[k][0];
            var td2 = document.createElement('td');
            td2.textContent = filas[k][1];
            tr.appendChild(td1);
            tr.appendChild(td2);
            tabla.appendChild(tr);
        }
        elDetalle.appendChild(tabla);
    }

    function marca(texto, clase) {
        var s = document.createElement('span');
        s.className = 'marca ' + clase;
        s.textContent = texto;
        return s;
    }
}());
</script>
</body>
</html>`;
}
