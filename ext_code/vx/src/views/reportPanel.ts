/**
 * @file reportPanel.ts
 * @brief Lo que cada funcion DECLARA, frente a lo que el compilador MIDE.
 *
 * Una funcion puede declarar que no reserva memoria, que no lanza, que no
 * entra en panico, cuanta pila usa y cuanto cuesta.  El compilador mide esas
 * mismas cosas sobre el codigo que sale.  Lo interesante no es ninguna de las
 * dos listas por separado -- es ponerlas una al lado de la otra: para eso
 * existe un contrato, para que se note cuando dejan de coincidir.
 *
 * Antes esto se ensenaba como una tabla de texto en un documento aparte, y una
 * tabla de texto no se puede filtrar, ni ordenar, ni pulsar.  Aqui cada fila es
 * una funcion, se puede ir a ella, y lo que no cuadra se ve de un vistazo sin
 * tener que compararlo de cabeza.
 */

import * as vscode from 'vscode';

import { VestaLanguageClient, describeError } from '../lsp/client';
import { FunctionReportResponse, VestaMethod } from '../lsp/protocol';
import { createNonce } from '../util/html';

/** Peticion que la pagina manda al editor. */
interface PanelRequest {
    /** 'refresh' vuelve a preguntar; 'goto' abre una funcion por su nombre. */
    type: 'refresh' | 'goto';
    name?: string;
    line?: number;
}

/**
 * @class ReportPanel
 * @brief Panel unico con el informe por funcion.
 */
export class ReportPanel {
    /** Panel vivo, si lo hay. */
    private static current: ReportPanel | undefined;

    /** Tipo con el que se registra el panel en el editor. */
    private static readonly viewType = 'vesta.report';

    /** Documento del que se esta mirando el informe. */
    private documentUri: vscode.Uri;

    /**
     * @brief Construye el panel y engancha sus eventos.
     * @param panel  Panel creado por el editor.
     * @param client Cliente del servidor de lenguaje.
     * @param uri    Documento inicial.
     */
    private constructor(
        private readonly panel: vscode.WebviewPanel,
        private readonly client: VestaLanguageClient,
        uri: vscode.Uri,
    ) {
        this.documentUri = uri;

        this.panel.onDidDispose(() => {
            if (ReportPanel.current === this) {
                ReportPanel.current = undefined;
            }
        });

        this.panel.webview.onDidReceiveMessage((message: PanelRequest) => {
            void this.handleRequest(message);
        });
    }

    /**
     * @brief Abre el informe de un documento, o lo reapunta si ya estaba.
     * @param client Cliente del servidor de lenguaje.
     * @param uri    Documento.
     */
    public static async show(
        client: VestaLanguageClient,
        uri: vscode.Uri,
    ): Promise<void> {
        const column = vscode.window.activeTextEditor
            ? vscode.ViewColumn.Beside
            : vscode.ViewColumn.One;

        if (!ReportPanel.current) {
            const panel = vscode.window.createWebviewPanel(
                ReportPanel.viewType,
                'Vesta: coste y contratos por funcion',
                column,
                { enableScripts: true, retainContextWhenHidden: true, localResourceRoots: [] },
            );
            ReportPanel.current = new ReportPanel(panel, client, uri);
            ReportPanel.current.panel.webview.html = buildHtml();
        }

        const current = ReportPanel.current;
        current.documentUri = uri;
        current.panel.reveal(column, false);
        await current.refresh();
    }

    /** @brief Cierra el panel si esta abierto. */
    public static dispose(): void {
        ReportPanel.current?.panel.dispose();
        ReportPanel.current = undefined;
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
        if (message.type === 'goto') {
            await abrirFuncion(message.name ?? '', this.documentUri,
                               message.line ?? 0);
        }
    }

    /** @brief Vuelve a pedir el informe y repinta. */
    private async refresh(): Promise<void> {
        this.panel.webview.postMessage({ kind: 'loading' });
        try {
            const response = await this.client.request<FunctionReportResponse>(
                VestaMethod.FunctionReport,
                { uri: this.documentUri.toString() },
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
 * @brief Abre una funcion: primero en este fichero, luego donde este.
 *
 * Casi siempre esta aqui mismo y la linea basta, que es instantaneo.  Si no
 * -- una funcion de otro modulo -- se busca por su nombre en el espacio de
 * trabajo.
 *
 * @param nombre Nombre de la funcion.
 * @param propio Documento del que se saco el informe.
 * @param linea  Linea en ese documento, si se sabe.
 */
async function abrirFuncion(
    nombre: string,
    propio: vscode.Uri,
    linea: number,
): Promise<void> {
    if (linea > 0) {
        const documento = await vscode.workspace.openTextDocument(propio);
        const editor = await vscode.window.showTextDocument(documento, {
            viewColumn: vscode.ViewColumn.One,
            preserveFocus: false,
        });
        const pos = new vscode.Position(Math.max(0, linea - 1), 0);
        editor.selection = new vscode.Selection(pos, pos);
        editor.revealRange(new vscode.Range(pos, pos),
                           vscode.TextEditorRevealType.InCenterIfOutsideViewport);
        return;
    }
    if (!nombre) {
        return;
    }
    const encontrados = await vscode.commands.executeCommand<
        vscode.SymbolInformation[]
    >('vscode.executeWorkspaceSymbolProvider', nombre);
    const primero = (encontrados ?? [])[0];
    if (!primero) {
        void vscode.window.showInformationMessage(
            `Vesta: no se encontro ${nombre} en el espacio de trabajo.`,
        );
        return;
    }
    const documento = await vscode.workspace.openTextDocument(
        primero.location.uri,
    );
    const editor = await vscode.window.showTextDocument(documento, {
        viewColumn: vscode.ViewColumn.One,
    });
    editor.selection = new vscode.Selection(primero.location.range.start,
                                            primero.location.range.start);
    editor.revealRange(primero.location.range,
                       vscode.TextEditorRevealType.InCenterIfOutsideViewport);
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
        display: flex;
        flex-direction: column;
        height: 100vh;
        overflow: hidden;
    }
    header {
        display: flex; align-items: center; gap: 8px; padding: 6px 10px;
        border-bottom: 1px solid var(--vscode-panel-border, rgba(128,128,128,.35));
        background: var(--vscode-editorWidget-background, transparent);
        flex: 0 0 auto; flex-wrap: wrap;
    }
    input[type="search"], select, button {
        font-family: inherit; font-size: inherit;
        color: var(--vscode-input-foreground, inherit);
        background: var(--vscode-input-background, transparent);
        border: 1px solid var(--vscode-input-border, rgba(128,128,128,.4));
        border-radius: 2px; padding: 2px 6px;
    }
    input[type="search"] { min-width: 14em; }
    button { cursor: pointer; background: var(--vscode-button-secondaryBackground, transparent); }
    button:hover { background: var(--vscode-toolbar-hoverBackground, rgba(128,128,128,.2)); }
    #cuenta { margin-left: auto; opacity: .75; }
    #tabla { flex: 1 1 auto; overflow: auto; }
    table { border-collapse: collapse; width: 100%; }
    th {
        position: sticky; top: 0; text-align: left; padding: 5px 10px;
        font-size: 10px; text-transform: uppercase; letter-spacing: .05em;
        opacity: .6; background: var(--vscode-editor-background);
        border-bottom: 1px solid var(--vscode-panel-border, rgba(128,128,128,.3));
        cursor: pointer; white-space: nowrap;
    }
    th:hover { opacity: .9; }
    td { padding: 3px 10px; vertical-align: top; }
    tr.fila:hover { background: var(--vscode-list-hoverBackground, rgba(128,128,128,.12)); }
    .fn {
        font-family: var(--vscode-editor-font-family, monospace);
        color: var(--vscode-charts-blue, #569cd6);
        cursor: pointer;
    }
    .fn:hover { text-decoration: underline; }
    .num { text-align: right; font-family: var(--vscode-editor-font-family, monospace); }
    .flojo { opacity: .5; }
    /* Lo declarado y lo medido, uno al lado del otro.  Cuando coinciden se
     * dice una vez; cuando no, se ven los dos y se marca. */
    .cuadra { color: var(--vscode-charts-green, #2ea043); }
    .nocuadra { color: var(--vscode-errorForeground, #f85149); font-weight: 600; }
    .marca {
        font-size: 10px; padding: 0 5px; border-radius: 8px; white-space: nowrap;
    }
    .marca.si { background: var(--vscode-charts-green, #2ea043); color: #08170c; }
    .marca.no { background: var(--vscode-charts-red, #f85149); color: #2b0a08; }
    .marca.quiza { background: var(--vscode-charts-orange, #d29922); color: #241a02; }
    .marca.neutro { background: rgba(128,128,128,.35); }
    #mensaje { padding: 16px; }
    #mensaje.error { color: var(--vscode-errorForeground); }
    .oculto { display: none; }
    .pista { padding: 6px 10px; opacity: .6; border-top: 1px solid var(--vscode-panel-border, rgba(128,128,128,.25)); }
</style>
</head>
<body>
<header>
    <input type="search" id="buscar" placeholder="Buscar funcion...">
    <select id="filtro">
        <option value="">Todas las funciones</option>
        <option value="contrato">Solo las que declaran algo</option>
        <option value="incumple">Solo lo que no cuadra</option>
        <option value="nativo">Solo lo que no compila a nativo</option>
        <option value="caro">Solo lo que no es O(1)</option>
    </select>
    <button id="recargar" title="Volver a analizar">Actualizar</button>
    <span id="cuenta"></span>
</header>
<div id="mensaje" class="oculto"></div>
<div id="tabla"></div>
<div class="pista">Pulsa una funcion para abrirla.  Pulsa una columna para ordenar por ella.</div>
<script nonce="${nonce}">
(function () {
    var api = acquireVsCodeApi();
    var elBuscar = document.getElementById('buscar');
    var elFiltro = document.getElementById('filtro');
    var elRecargar = document.getElementById('recargar');
    var elCuenta = document.getElementById('cuenta');
    var elMensaje = document.getElementById('mensaje');
    var elTabla = document.getElementById('tabla');

    var funciones = [];
    var ordenPor = '';
    var ordenAsc = true;

    elRecargar.addEventListener('click', function () {
        api.postMessage({ type: 'refresh' });
    });
    elBuscar.addEventListener('input', pintar);
    elFiltro.addEventListener('change', pintar);

    window.addEventListener('message', function (evento) {
        var d = evento.data;
        if (d.kind === 'loading') { mensaje('Analizando...', false); return; }
        if (d.kind === 'error') { mensaje(d.message, true); return; }
        var p = d.payload || {};
        if (p.error) { mensaje(p.error, true); return; }
        funciones = p.functions || [];
        if (funciones.length === 0) {
            mensaje('El modulo no tiene funciones que analizar.', false);
            return;
        }
        elMensaje.classList.add('oculto');
        elTabla.classList.remove('oculto');
        pintar();
    });

    function mensaje(texto, esError) {
        elMensaje.textContent = texto;
        elMensaje.className = esError ? 'error' : '';
        elMensaje.classList.remove('oculto');
        elTabla.classList.add('oculto');
    }

    /** Si algo de esta funcion no cuadra con lo que declaro. */
    function incumple(f) {
        if (f.cost && f.cost.mismatch) { return true; }
        var ch = f.checks || [];
        for (var i = 0; i < ch.length; i++) {
            if (ch[i].status === 'incumple') { return true; }
        }
        return false;
    }

    function filtradas() {
        var texto = elBuscar.value.toLowerCase();
        var modo = elFiltro.value;
        var salida = [];
        for (var i = 0; i < funciones.length; i++) {
            var f = funciones[i];
            if (texto && (f.display || f.name).toLowerCase().indexOf(texto) < 0) {
                continue;
            }
            if (modo === 'contrato' &&
                !(f.declared || (f.cost && f.cost.declared))) { continue; }
            if (modo === 'incumple' && !incumple(f)) { continue; }
            if (modo === 'nativo' && f.aot && f.aot.ok) { continue; }
            if (modo === 'caro' && f.cost && f.cost.total === 'O(1)') { continue; }
            salida.push(f);
        }
        if (ordenPor) {
            salida.sort(function (a, b) {
                var va = valorDeOrden(a), vb = valorDeOrden(b);
                if (va < vb) { return ordenAsc ? -1 : 1; }
                if (va > vb) { return ordenAsc ? 1 : -1; }
                return 0;
            });
        }
        return salida;
    }

    function valorDeOrden(f) {
        switch (ordenPor) {
            case 'funcion': return (f.display || f.name).toLowerCase();
            case 'coste': return (f.cost && f.cost.total) || '';
            case 'bucles': return (f.cost && f.cost.loops) || 0;
            case 'reserva': return (f.measured && f.measured.allocTotal) || 0;
            case 'pila':
                // Se ordena por lo que se sabe: el total cuando esta cerrado,
                // y el marco propio cuando no.  Mandar lo abierto al final con
                // un numero enorme era ordenar por el centinela, no por la
                // pila.
                if (!f.measured) { return 0; }
                if (f.measured.stackBounded === false) {
                    return f.measured.stackPartial || 0;
                }
                return f.measured.stackTotal || 0;
            case 'nativo': return (f.aot && f.aot.ok) ? 1 : 0;
            default: return 0;
        }
    }

    var COLUMNAS = [
        { id: 'funcion', titulo: 'Funcion' },
        { id: 'coste',   titulo: 'Cuesta' },
        { id: 'bucles',  titulo: 'Anidamiento' },
        { id: 'reserva', titulo: 'Reserva' },
        { id: 'pila',    titulo: 'Pila' },
        { id: 'hace',    titulo: 'Hace' },
        { id: 'declara', titulo: 'Declara y cumple' },
        { id: 'nativo',  titulo: 'Nativo' }
    ];

    function pintar() {
        var lista = filtradas();
        elCuenta.textContent = lista.length + ' de ' + funciones.length + ' funciones';

        var tabla = document.createElement('table');
        var cab = document.createElement('tr');
        for (var c = 0; c < COLUMNAS.length; c++) {
            (function (col) {
                var th = document.createElement('th');
                th.textContent = col.titulo +
                    (ordenPor === col.id ? (ordenAsc ? '  ^' : '  v') : '');
                th.addEventListener('click', function () {
                    if (ordenPor === col.id) { ordenAsc = !ordenAsc; }
                    else { ordenPor = col.id; ordenAsc = true; }
                    pintar();
                });
                cab.appendChild(th);
            })(COLUMNAS[c]);
        }
        tabla.appendChild(cab);
        for (var i = 0; i < lista.length; i++) {
            tabla.appendChild(hacerFila(lista[i]));
        }
        elTabla.innerHTML = '';
        elTabla.appendChild(tabla);
    }

    function hacerFila(f) {
        var tr = document.createElement('tr');
        tr.className = 'fila';

        // La funcion, que lleva a la funcion.
        var tdF = document.createElement('td');
        var nombre = document.createElement('span');
        nombre.className = 'fn';
        nombre.textContent = f.display || f.name;
        nombre.title = 'Abrir ' + (f.display || f.name);
        nombre.addEventListener('click', function () {
            api.postMessage({ type: 'goto', name: f.name, line: f.line });
        });
        tdF.appendChild(nombre);
        tr.appendChild(tdF);

        var coste = f.cost || {};
        var medido = f.measured || {};
        var declarado = f.declared || {};

        // Lo que cuesta: el total, y el propio si difiere -- que es lo que
        // dice si el coste es suyo o de lo que llama.
        var tdC = document.createElement('td');
        var textoCoste = coste.total || '';
        if (coste.partial && coste.partial !== coste.total) {
            textoCoste += '  (suyo ' + coste.partial + ')';
        }
        tdC.appendChild(document.createTextNode(textoCoste));
        if (coste.declared) {
            tdC.appendChild(document.createTextNode('  '));
            var decl = document.createElement('span');
            decl.className = coste.mismatch ? 'nocuadra' : 'cuadra';
            decl.textContent = 'declara ' + coste.declared;
            decl.title = coste.mismatch
                ? 'Lo declarado no cuadra con lo que el compilador infiere'
                : 'Lo declarado cuadra';
            tdC.appendChild(decl);
        }
        if (coste.confidence && coste.confidence !== 'exacta') {
            var conf = document.createElement('span');
            conf.className = 'flojo';
            conf.textContent = '  ' + coste.confidence;
            tdC.appendChild(conf);
        }
        tr.appendChild(tdC);

        tr.appendChild(celdaNum(coste.loops));
        tr.appendChild(celdaMedidoVsDeclarado(medido.allocTotal,
                                              declarado.allocTotal, ''));
        if (medido.stackBounded === false) {
            /* Lo que se sabe NO se esconde.
             *
             * Antes llegaba el centinela del compilador como numero y se
             * pintaba "18446744073709552000 B", que se lee como un tamano.
             * Pero taparlo entero con "sin cota" es el otro extremo: el marco
             * PROPIO de la funcion si se conoce -- es un hecho medido --, y lo
             * unico que falta es cuanta pila gastan por dentro las llamadas
             * que salen del modulo (una nativa, o recursion).  Se ensena el
             * propio, y al lado que la cadena no se cierra. */
            var tdP = document.createElement('td');
            tdP.className = 'num';
            tdP.appendChild(document.createTextNode(
                String(medido.stackPartial || 0) + ' B  '));
            var abierta = document.createElement('span');
            abierta.className = 'flojo';
            abierta.textContent = '+ llamadas sin acotar';
            abierta.title = 'Este es el marco propio de la funcion, que si se '
                          + 'sabe.  Lo que no se puede cerrar es cuanta pila '
                          + 'gastan por dentro las llamadas que salen del '
                          + 'modulo (una funcion nativa) o la recursion.';
            tdP.appendChild(abierta);
            tr.appendChild(tdP);
        } else {
            tr.appendChild(celdaMedidoVsDeclarado(medido.stackTotal,
                                                  declarado.stackTotal, ' B'));
        }

        // Lo que HACE, en marcas: solo lo que es cierto, para que se lea de un
        // vistazo en vez de leer cuatro "no".
        var tdH = document.createElement('td');
        if (medido.pure) { tdH.appendChild(marca('pura', 'si')); }
        if (medido.throws) { tdH.appendChild(marca('lanza', 'no')); }
        if (medido.panics) { tdH.appendChild(marca('entra en panico', 'no')); }
        if (medido.recursive) { tdH.appendChild(marca('recursiva', 'neutro')); }
        if (medido.dynamicCall) { tdH.appendChild(marca('llamada indirecta', 'neutro')); }
        if (medido.frameOpaque) { tdH.appendChild(marca('marco opaco', 'neutro')); }
        if (medido.effectsKnown === false) {
            tdH.appendChild(marca('efectos sin cerrar', 'quiza'));
        }
        tr.appendChild(tdH);

        // Lo que declara, con el veredicto de cada cosa.
        var tdD = document.createElement('td');
        var checks = f.checks || [];
        for (var i = 0; i < checks.length; i++) {
            var estado = checks[i].status === 'cumple' ? 'si'
                       : checks[i].status === 'incumple' ? 'no' : 'quiza';
            var m = marca(checks[i].contract, estado);
            m.title = checks[i].status + (checks[i].detail ? ': ' + checks[i].detail : '');
            tdD.appendChild(m);
        }
        if (checks.length === 0) {
            var nada = document.createElement('span');
            nada.className = 'flojo';
            nada.textContent = 'no declara nada';
            tdD.appendChild(nada);
        }
        tr.appendChild(tdD);

        // Si el modo nativo puede con ella, y si no, por que.
        var tdN = document.createElement('td');
        var aot = f.aot || {};
        if (aot.ok) {
            tdN.appendChild(marca('si', 'si'));
        } else {
            var motivos = (aot.issues || []).map(function (i) {
                return i.reason + ' (' + i.op + ')';
            });
            var m2 = marca('no', 'no');
            m2.title = motivos.join('\\n');
            tdN.appendChild(m2);
            var texto = document.createElement('span');
            texto.className = 'flojo';
            texto.textContent = '  ' + (motivos[0] || '');
            tdN.appendChild(texto);
        }
        tr.appendChild(tdN);
        return tr;
    }

    function celdaNum(v) {
        var td = document.createElement('td');
        td.className = 'num';
        if (v === undefined || v === null || v === 0) {
            td.className += ' flojo';
            td.textContent = v === undefined || v === null ? '' : '0';
        } else {
            td.textContent = String(v);
        }
        return td;
    }

    /**
     * Lo medido y, si se declaro, lo declarado al lado.
     *
     * Es la comparacion que da sentido a un contrato: no basta con saber que
     * la funcion reserva 3 veces, hace falta saber que prometio reservar 0.
     */
    function celdaMedidoVsDeclarado(medido, declarado, unidad) {
        var td = document.createElement('td');
        td.className = 'num';
        if (medido === undefined || medido === null) { return td; }
        var texto = String(medido) + unidad;
        if (declarado === undefined || declarado === null) {
            if (medido === 0) { td.className += ' flojo'; }
            td.textContent = texto;
            return td;
        }
        td.appendChild(document.createTextNode(texto + '  '));
        var d = document.createElement('span');
        var cumple = medido <= declarado;
        d.className = cumple ? 'cuadra' : 'nocuadra';
        d.textContent = 'de ' + declarado + unidad;
        d.title = cumple ? 'cabe en lo declarado' : 'se pasa de lo declarado';
        td.appendChild(d);
        return td;
    }

    function marca(texto, clase) {
        var s = document.createElement('span');
        s.className = 'marca ' + clase;
        s.textContent = texto;
        s.style.marginRight = '4px';
        return s;
    }
}());
</script>
</body>
</html>`;
}
