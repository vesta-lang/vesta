/**
 * @file machinePanel.ts
 * @brief Vista correlacionada del fuente, el IR y el codigo maquina.
 *
 * Tres columnas alineadas por linea fuente: a la izquierda el codigo Vesta de
 * la funcion, en el centro el IR ya optimizado y a la derecha el desensamblado
 * que produce el generador elegido.  Al pasar por encima de cualquier fila se
 * marcan las de las otras columnas que salieron de la misma linea, que es la
 * pregunta que uno se hace mirando codigo generado: de donde sale esto.
 *
 * La correlacion no se calcula aqui: el servidor ya devuelve cada instruccion
 * con su linea fuente y con la identidad exacta de la operacion del IR que la
 * genero.  Esta vista solo la presenta.
 */

import * as vscode from 'vscode';

import { VestaLanguageClient, describeError } from '../lsp/client';
import { AsmResponse, FunctionsResponse, VestaMethod } from '../lsp/protocol';
import { createNonce } from '../util/html';
import { aotTier, inspectTarget } from '../util/settings';

/** Generador de codigo del que se pide el desensamblado. */
export type MachineBackend = 'jit' | 'aot';

/** Peticion que el panel manda al editor. */
interface PanelRequest {
    type: 'refresh' | 'backend' | 'function' | 'reveal';
    backend?: MachineBackend;
    functionName?: string;
    line?: number;
}

/**
 * @class MachineViewPanel
 * @brief Panel unico con la vista correlacionada.
 */
export class MachineViewPanel {
    /** Panel vivo, si lo hay. */
    private static current: MachineViewPanel | undefined;

    /** Tipo con el que se registra el panel en el editor. */
    private static readonly viewType = 'vesta.machineView';

    /** Documento del que se esta mirando el codigo generado. */
    private documentUri: vscode.Uri;

    /** Generador de codigo seleccionado. */
    private backend: MachineBackend = 'jit';

    /** Funcion seleccionada; vacia significa la primera compilable. */
    private functionName = '';

    /** Funciones del modulo, para el selector de la barra superior. */
    private functions: string[] = [];

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
            if (MachineViewPanel.current === this) {
                MachineViewPanel.current = undefined;
            }
        });

        this.panel.webview.onDidReceiveMessage((message: PanelRequest) => {
            void this.handleRequest(message);
        });
    }

    /**
     * @brief Abre la vista para un documento, o la reapunta si ya estaba abierta.
     * @param client Cliente del servidor de lenguaje.
     * @param uri    Documento del que mirar el codigo generado.
     */
    public static async show(client: VestaLanguageClient, uri: vscode.Uri): Promise<void> {
        const column = vscode.window.activeTextEditor
            ? vscode.ViewColumn.Beside
            : vscode.ViewColumn.One;

        if (!MachineViewPanel.current) {
            const panel = vscode.window.createWebviewPanel(
                MachineViewPanel.viewType,
                'Vesta: fuente / IR / ensamblador',
                column,
                { enableScripts: true, retainContextWhenHidden: true, localResourceRoots: [] },
            );
            MachineViewPanel.current = new MachineViewPanel(panel, client, uri);
            MachineViewPanel.current.panel.webview.html = buildHtml(panel.webview);
        }

        const current = MachineViewPanel.current;
        current.documentUri = uri;
        current.functionName = '';
        current.panel.reveal(column, false);
        await current.refresh();
    }

    /** @brief Cierra el panel si esta abierto. */
    public static dispose(): void {
        MachineViewPanel.current?.panel.dispose();
        MachineViewPanel.current = undefined;
    }

    /**
     * @brief Atiende una peticion llegada desde el panel.
     * @param message Peticion del panel.
     */
    private async handleRequest(message: PanelRequest): Promise<void> {
        switch (message.type) {
            case 'backend':
                if (message.backend) {
                    this.backend = message.backend;
                }
                await this.refresh();
                break;
            case 'function':
                this.functionName = message.functionName ?? '';
                await this.refresh();
                break;
            case 'refresh':
                await this.refresh();
                break;
            case 'reveal':
                await this.revealSourceLine(message.line ?? 0);
                break;
        }
    }

    /**
     * @brief Lleva el cursor del editor a una linea del fuente.
     *
     * Cerrar el circulo importa: desde el ensamblador se llega al codigo que lo
     * genero con un solo clic.
     *
     * @param line Linea fuente, contada desde uno.
     */
    private async revealSourceLine(line: number): Promise<void> {
        if (line <= 0) {
            return;
        }
        const document = await vscode.workspace.openTextDocument(this.documentUri);
        const editor = await vscode.window.showTextDocument(document, {
            viewColumn: vscode.ViewColumn.One,
            preserveFocus: true,
        });
        const position = new vscode.Position(Math.max(0, line - 1), 0);
        editor.selection = new vscode.Selection(position, position);
        editor.revealRange(
            new vscode.Range(position, position),
            vscode.TextEditorRevealType.InCenterIfOutsideViewport,
        );
    }

    /** @brief Vuelve a pedir los datos al servidor y repinta el panel. */
    private async refresh(): Promise<void> {
        this.panel.webview.postMessage({ kind: 'loading' });

        const uri = this.documentUri.toString();
        try {
            await this.loadFunctions(uri);

            const target = inspectTarget();
            const method =
                this.backend === 'jit' ? VestaMethod.JitAsm : VestaMethod.AotAsm;
            const response = await this.client.request<AsmResponse>(method, {
                uri,
                function: this.functionName,
                os: target.os,
                arch: target.arch,
                tier: aotTier(),
            });

            this.panel.webview.postMessage({
                kind: 'data',
                backend: this.backend,
                functions: this.functions,
                selected: response.function ?? this.functionName,
                payload: response,
            });
        } catch (err) {
            this.panel.webview.postMessage({
                kind: 'error',
                message: describeError(err),
            });
        }
    }

    /**
     * @brief Refresca la lista de funciones del modulo.
     * @param uri Documento en curso.
     */
    private async loadFunctions(uri: string): Promise<void> {
        try {
            const response = await this.client.request<FunctionsResponse>(
                VestaMethod.Functions,
                { uri },
            );
            this.functions = (response.functions ?? []).map(fn => fn.name);
        } catch {
            // Sin lista de funciones la vista sigue sirviendo: el servidor elige
            // la primera compilable y el selector se queda vacio.
            this.functions = [];
        }
    }
}

/**
 * @brief Construye el documento del panel.
 *
 * El contenido es estatico: la vista se dibuja en el propio panel a partir de
 * los datos que llegan por mensaje.  Asi el panel conserva su desplazamiento y
 * su seleccion al cambiar de funcion, y no hay que rehacer el documento (ni la
 * politica de seguridad) en cada actualizacion.
 *
 * @param webview Vista a la que pertenece el documento.
 * @return El documento completo.
 */
function buildHtml(webview: vscode.Webview): string {
    const nonce = createNonce();
    const csp =
        "default-src 'none'; " +
        `style-src ${webview.cspSource} 'unsafe-inline'; ` +
        `script-src 'nonce-${nonce}';`;

    return `<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="UTF-8">
<meta http-equiv="Content-Security-Policy" content="${csp}">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Vesta</title>
<style>
    :root { color-scheme: light dark; }
    body {
        margin: 0;
        font-family: var(--vscode-editor-font-family, monospace);
        font-size: var(--vscode-editor-font-size, 13px);
        color: var(--vscode-editor-foreground);
        background: var(--vscode-editor-background);
        display: flex;
        flex-direction: column;
        height: 100vh;
        overflow: hidden;
    }
    header {
        display: flex;
        align-items: center;
        gap: 12px;
        padding: 6px 10px;
        border-bottom: 1px solid var(--vscode-panel-border, rgba(128,128,128,.35));
        background: var(--vscode-editorWidget-background, transparent);
        font-family: var(--vscode-font-family);
        font-size: 12px;
        flex: 0 0 auto;
        flex-wrap: wrap;
    }
    header label { opacity: .8; }
    select, button {
        font-family: inherit;
        font-size: inherit;
        color: var(--vscode-dropdown-foreground, inherit);
        background: var(--vscode-dropdown-background, transparent);
        border: 1px solid var(--vscode-dropdown-border, rgba(128,128,128,.4));
        border-radius: 2px;
        padding: 2px 6px;
    }
    button { cursor: pointer; }
    button:hover { background: var(--vscode-toolbar-hoverBackground, rgba(128,128,128,.2)); }
    #stats { margin-left: auto; opacity: .75; }
    #columns { display: flex; flex: 1 1 auto; min-height: 0; }
    .column { flex: 1 1 0; min-width: 0; display: flex; flex-direction: column; border-right: 1px solid var(--vscode-panel-border, rgba(128,128,128,.25)); }
    .column:last-child { border-right: none; }
    .column h2 {
        margin: 0;
        padding: 4px 10px;
        font-family: var(--vscode-font-family);
        font-size: 11px;
        font-weight: 600;
        letter-spacing: .04em;
        text-transform: uppercase;
        opacity: .7;
        border-bottom: 1px solid var(--vscode-panel-border, rgba(128,128,128,.25));
        flex: 0 0 auto;
    }
    .rows { flex: 1 1 auto; overflow: auto; padding: 4px 0 40vh 0; }
    .row { display: flex; white-space: pre; padding: 0 10px; line-height: 1.45; }
    .row:hover { background: var(--vscode-list-hoverBackground, rgba(128,128,128,.12)); }
    .row.linked { background: var(--vscode-editor-selectionHighlightBackground, rgba(90,150,255,.18)); }
    .row.exact { background: var(--vscode-editor-findMatchHighlightBackground, rgba(255,190,60,.35)); }
    .gutter { flex: 0 0 auto; width: 4.5em; text-align: right; padding-right: 10px; opacity: .45; user-select: none; }
    .text { flex: 1 1 auto; }
    .row.label .text { opacity: .75; font-style: italic; }
    .row.clickable { cursor: pointer; }
    .comment { opacity: .6; }
    #footer {
        flex: 0 0 auto;
        border-top: 1px solid var(--vscode-panel-border, rgba(128,128,128,.35));
        padding: 4px 10px;
        min-height: 1.4em;
        white-space: pre;
        overflow-x: auto;
        opacity: .85;
    }
    #message { padding: 16px; font-family: var(--vscode-font-family); }
    #message.error { color: var(--vscode-errorForeground); }
    .hidden { display: none; }
    details { padding: 4px 10px; font-family: var(--vscode-font-family); font-size: 12px; }
    summary { cursor: pointer; opacity: .8; }
    table { border-collapse: collapse; margin-top: 6px; }
    td { padding: 1px 10px 1px 0; white-space: pre; }
</style>
</head>
<body>
<header>
    <label for="backend">Generador</label>
    <select id="backend">
        <option value="jit">JIT</option>
        <option value="aot">AOT</option>
    </select>
    <label for="fn">Funcion</label>
    <select id="fn"></select>
    <button id="reload" title="Volver a compilar y repintar">Actualizar</button>
    <span id="stats"></span>
</header>
<div id="message" class="hidden"></div>
<div id="columns">
    <div class="column"><h2>Fuente</h2><div class="rows" id="src"></div></div>
    <div class="column"><h2>IR</h2><div class="rows" id="ir"></div></div>
    <div class="column"><h2>Ensamblador</h2><div class="rows" id="asm"></div></div>
</div>
<details id="detail" class="hidden">
    <summary>Marco de pila y argumentos</summary>
    <div id="detailBody"></div>
</details>
<div id="footer"></div>
<script nonce="${nonce}">
(function () {
    var vscodeApi = acquireVsCodeApi();
    var elBackend = document.getElementById('backend');
    var elFn = document.getElementById('fn');
    var elReload = document.getElementById('reload');
    var elStats = document.getElementById('stats');
    var elMessage = document.getElementById('message');
    var elColumns = document.getElementById('columns');
    var elSrc = document.getElementById('src');
    var elIr = document.getElementById('ir');
    var elAsm = document.getElementById('asm');
    var elFooter = document.getElementById('footer');
    var elDetail = document.getElementById('detail');
    var elDetailBody = document.getElementById('detailBody');

    // Mapa de identidad de operacion del IR a su texto, para poder decir cual
    // genero exactamente cada instruccion.
    var irById = {};

    elBackend.addEventListener('change', function () {
        vscodeApi.postMessage({ type: 'backend', backend: elBackend.value });
    });
    elFn.addEventListener('change', function () {
        vscodeApi.postMessage({ type: 'function', functionName: elFn.value });
    });
    elReload.addEventListener('click', function () {
        vscodeApi.postMessage({ type: 'refresh' });
    });

    window.addEventListener('message', function (event) {
        var data = event.data;
        if (data.kind === 'loading') {
            showMessage('Compilando...', false);
            return;
        }
        if (data.kind === 'error') {
            showMessage(data.message, true);
            return;
        }
        render(data);
    });

    function showMessage(text, isError) {
        elMessage.textContent = text;
        elMessage.className = isError ? 'error' : '';
        elColumns.classList.add('hidden');
        elDetail.classList.add('hidden');
    }

    function showColumns() {
        elMessage.className = 'hidden';
        elColumns.classList.remove('hidden');
    }

    function render(data) {
        var payload = data.payload || {};
        elBackend.value = data.backend;
        fillFunctions(data.functions || [], data.selected || '');

        if (payload.error) {
            showMessage(payload.error, true);
            return;
        }
        if (payload.unsupported || payload.incompatible) {
            showMessage(payload.reason || 'La funcion no se puede compilar en este modo.', false);
            return;
        }

        showColumns();
        irById = payload.ir_by_id || {};

        elStats.textContent = describeStats(payload);
        renderSource(payload.source || []);
        renderIr(payload.ir_listing || []);
        renderAsm(payload.asm_lines || [], payload.asm_labels || []);
        renderDetail(payload);
        elFooter.textContent = '';
    }

    function describeStats(payload) {
        var parts = [];
        if (payload.bytes !== undefined) { parts.push(payload.bytes + ' bytes'); }
        if (payload.instructions !== undefined) { parts.push(payload.instructions + ' instrucciones'); }
        if (payload.relocs && payload.relocs.length) { parts.push(payload.relocs.length + ' reubicaciones'); }
        return parts.join('  |  ');
    }

    function fillFunctions(names, selected) {
        if (names.length === 0) {
            elFn.innerHTML = '';
            var only = document.createElement('option');
            only.value = selected;
            only.textContent = selected || '(automatica)';
            elFn.appendChild(only);
            return;
        }
        var current = elFn.value;
        if (names.join('\\u0001') !== (elFn.dataset.names || '')) {
            elFn.innerHTML = '';
            for (var i = 0; i < names.length; i++) {
                var opt = document.createElement('option');
                opt.value = names[i];
                opt.textContent = names[i];
                elFn.appendChild(opt);
            }
            elFn.dataset.names = names.join('\\u0001');
        }
        elFn.value = selected || current || names[0];
    }

    function makeRow(gutterText, text, line, extraClass) {
        var row = document.createElement('div');
        row.className = 'row' + (extraClass ? ' ' + extraClass : '');
        if (line > 0) { row.dataset.line = String(line); }

        var gutter = document.createElement('span');
        gutter.className = 'gutter';
        gutter.textContent = gutterText;
        row.appendChild(gutter);

        var body = document.createElement('span');
        body.className = 'text';
        var comment = text.indexOf('  ; ');
        if (comment >= 0) {
            body.appendChild(document.createTextNode(text.slice(0, comment)));
            var note = document.createElement('span');
            note.className = 'comment';
            note.textContent = text.slice(comment);
            body.appendChild(note);
        } else {
            body.textContent = text;
        }
        row.appendChild(body);
        return row;
    }

    function renderSource(lines) {
        elSrc.innerHTML = '';
        for (var i = 0; i < lines.length; i++) {
            var row = makeRow(String(lines[i].line), lines[i].text, lines[i].line, 'clickable');
            attachHover(row, lines[i].line, -1);
            attachReveal(row, lines[i].line);
            elSrc.appendChild(row);
        }
    }

    function renderIr(rows) {
        elIr.innerHTML = '';
        for (var i = 0; i < rows.length; i++) {
            var isLabel = rows[i].kind === 'label';
            var gutter = rows[i].line > 0 ? String(rows[i].line) : '';
            var text = isLabel ? rows[i].text + ':' : rows[i].text;
            var row = makeRow(gutter, text, rows[i].line, isLabel ? 'label' : 'clickable');
            attachHover(row, rows[i].line, -1);
            if (!isLabel) { attachReveal(row, rows[i].line); }
            elIr.appendChild(row);
        }
    }

    function renderAsm(lines, labels) {
        elAsm.innerHTML = '';
        var byOffset = {};
        for (var l = 0; l < labels.length; l++) {
            byOffset[labels[l].offset] = labels[l].name;
        }
        for (var i = 0; i < lines.length; i++) {
            var entry = lines[i];
            if (byOffset[entry.addr] !== undefined) {
                var labelRow = makeRow('', byOffset[entry.addr] + ':', 0, 'label');
                elAsm.appendChild(labelRow);
            }
            var row = makeRow(entry.addr, entry.text, entry.line, 'clickable');
            var irId = entry.ir_id === undefined ? -1 : entry.ir_id;
            attachHover(row, entry.line, irId);
            attachReveal(row, entry.line);
            elAsm.appendChild(row);
        }
    }

    function renderDetail(payload) {
        var args = payload.args || [];
        var frame = payload.frame || [];
        var relocs = payload.relocs || [];
        if (args.length === 0 && frame.length === 0 && relocs.length === 0) {
            elDetail.classList.add('hidden');
            return;
        }
        elDetail.classList.remove('hidden');
        elDetailBody.innerHTML = '';
        if (args.length) {
            elDetailBody.appendChild(buildTable('Argumentos', args.map(function (a) {
                return [a.reg, a.name];
            })));
        }
        if (frame.length) {
            elDetailBody.appendChild(buildTable('Marco de pila', frame.map(function (s) {
                return [s.label, s.size + ' B', s.kind, s.name];
            })));
        }
        if (relocs.length) {
            elDetailBody.appendChild(buildTable('Reubicaciones', relocs.map(function (r) {
                return ['+' + r.offset, r.kind, r.symbol, String(r.addend)];
            })));
        }
    }

    function buildTable(title, rows) {
        var box = document.createElement('div');
        var caption = document.createElement('div');
        caption.textContent = title;
        caption.style.opacity = '.7';
        caption.style.marginTop = '8px';
        box.appendChild(caption);
        var table = document.createElement('table');
        for (var i = 0; i < rows.length; i++) {
            var tr = document.createElement('tr');
            for (var c = 0; c < rows[i].length; c++) {
                var td = document.createElement('td');
                td.textContent = rows[i][c];
                tr.appendChild(td);
            }
            table.appendChild(tr);
        }
        box.appendChild(table);
        return box;
    }

    function attachHover(row, line, irId) {
        row.addEventListener('mouseenter', function () {
            highlight(line);
            if (irId >= 0 && irById[String(irId)] !== undefined) {
                elFooter.textContent = 'Operacion del IR: ' + irById[String(irId)];
            } else if (line > 0) {
                elFooter.textContent = 'Linea ' + line;
            } else {
                elFooter.textContent = '';
            }
        });
    }

    function attachReveal(row, line) {
        if (line <= 0) { return; }
        row.addEventListener('click', function () {
            vscodeApi.postMessage({ type: 'reveal', line: line });
        });
    }

    function highlight(line) {
        var marked = document.querySelectorAll('.row.linked');
        for (var i = 0; i < marked.length; i++) {
            marked[i].classList.remove('linked');
        }
        if (line <= 0) { return; }
        var rows = document.querySelectorAll('.row[data-line="' + line + '"]');
        for (var j = 0; j < rows.length; j++) {
            rows[j].classList.add('linked');
        }
    }
}());
</script>
</body>
</html>`;
}
