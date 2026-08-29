/**
 * @file asaPanel.ts
 * @brief Lo que el compilador sabe del modulo, para leerlo de verdad.
 *
 * El analisis produce miles de hechos: de cada valor dice a donde apunta, entre
 * que limites se mueve, a que esta alineado, si escapa, que efectos tiene.  Eso
 * ya se podia ver, pero como un volcado de texto de miles de lineas -- que es
 * como no poder verlo: nadie lee eso buscando una cosa concreta.
 *
 * Aqui se presenta como lo que es: una tabla de hechos que se filtra por
 * funcion, por analisis, por lo seguro que sea cada uno y por texto, y en la
 * que cada fila lleva al codigo del que habla.
 *
 * Se aporta ademas lo que el volcado no ensena: POR QUE se callo.  El analisis
 * cuenta cuantas entidades miro y por que motivo no dijo nada de cada una, y
 * eso es la mitad util de un analisis -- lo que no sabe, y por que --.
 */

import * as vscode from 'vscode';

import { VestaLanguageClient, describeError } from '../lsp/client';
import { AsaFactsResponse, VestaMethod } from '../lsp/protocol';
import { createNonce } from '../util/html';

/** Peticion que la pagina manda al editor. */
interface PanelRequest {
    /**
     * 'refresh' vuelve a preguntar, 'reveal' lleva el cursor a una linea de
     * este fichero y 'goto' busca una funcion por su nombre y la abre, este
     * donde este.
     */
    type: 'refresh' | 'reveal' | 'goto';
    line?: number;
    name?: string;
}

/**
 * @class AsaPanel
 * @brief Panel unico con lo que el compilador sabe del modulo.
 */
export class AsaPanel {
    /** Panel vivo, si lo hay. */
    private static current: AsaPanel | undefined;

    /** Tipo con el que se registra el panel en el editor. */
    private static readonly viewType = 'vesta.asa';

    /** Documento del que se esta mirando lo que se sabe. */
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
            if (AsaPanel.current === this) {
                AsaPanel.current = undefined;
            }
        });

        this.panel.webview.onDidReceiveMessage((message: PanelRequest) => {
            void this.handleRequest(message);
        });
    }

    /**
     * @brief Abre la vista para un documento, o la reapunta si ya estaba.
     * @param client Cliente del servidor de lenguaje.
     * @param uri    Documento del que mirar lo que se sabe.
     */
    public static async show(
        client: VestaLanguageClient,
        uri: vscode.Uri,
    ): Promise<void> {
        const column = vscode.window.activeTextEditor
            ? vscode.ViewColumn.Beside
            : vscode.ViewColumn.One;

        if (!AsaPanel.current) {
            const panel = vscode.window.createWebviewPanel(
                AsaPanel.viewType,
                'Vesta: lo que el compilador sabe',
                column,
                { enableScripts: true, retainContextWhenHidden: true, localResourceRoots: [] },
            );
            AsaPanel.current = new AsaPanel(panel, client, uri);
            AsaPanel.current.panel.webview.html = buildHtml();
        }

        const current = AsaPanel.current;
        current.documentUri = uri;
        current.panel.reveal(column, false);
        await current.refresh();
    }

    /** @brief Cierra el panel si esta abierto. */
    public static dispose(): void {
        AsaPanel.current?.panel.dispose();
        AsaPanel.current = undefined;
    }

    /**
     * @brief Atiende una peticion llegada desde la pagina.
     * @param message Peticion.
     */
    private async handleRequest(message: PanelRequest): Promise<void> {
        if (message.type === 'refresh') {
            await this.refresh();
            return;
        }
        if (message.type === 'reveal') {
            await this.revealSourceLine(message.line ?? 0);
            return;
        }
        if (message.type === 'goto') {
            await abrirFuncion(message.name ?? '');
        }
    }

    /**
     * @brief Lleva el cursor del editor a una linea del fuente.
     * @param line Linea, contada desde uno.
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

    /** @brief Vuelve a pedir los datos y repinta. */
    private async refresh(): Promise<void> {
        this.panel.webview.postMessage({ kind: 'loading' });
        try {
            const response = await this.client.request<AsaFactsResponse>(
                VestaMethod.AsaFacts,
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
 * @brief Abre la funcion que se llama @p nombre, este en el fichero que este.
 *
 * Aqui solo se tiene el NOMBRE: no hay una posicion que dar, asi que "ir a la
 * definicion" no sirve.  El servidor busca por nombre -- entiende tanto el
 * escrito como el interno -- y devuelve donde esta.
 *
 * @param nombre Nombre de la funcion, como se ensena o como lo llama el
 *               compilador.
 */
async function abrirFuncion(nombre: string): Promise<void> {
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
        preserveFocus: false,
    });
    editor.selection = new vscode.Selection(
        primero.location.range.start,
        primero.location.range.start,
    );
    editor.revealRange(
        primero.location.range,
        vscode.TextEditorRevealType.InCenterIfOutsideViewport,
    );
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
        display: flex;
        align-items: center;
        gap: 8px;
        padding: 6px 10px;
        border-bottom: 1px solid var(--vscode-panel-border, rgba(128,128,128,.35));
        background: var(--vscode-editorWidget-background, transparent);
        flex: 0 0 auto;
        flex-wrap: wrap;
    }
    input[type="search"], select, button {
        font-family: inherit;
        font-size: inherit;
        color: var(--vscode-input-foreground, inherit);
        background: var(--vscode-input-background, transparent);
        border: 1px solid var(--vscode-input-border, rgba(128,128,128,.4));
        border-radius: 2px;
        padding: 2px 6px;
    }
    input[type="search"] { min-width: 16em; }
    button { cursor: pointer; background: var(--vscode-button-secondaryBackground, transparent); }
    button:hover { background: var(--vscode-toolbar-hoverBackground, rgba(128,128,128,.2)); }
    #cuenta { margin-left: auto; opacity: .75; }
    #cuerpo { display: flex; flex: 1 1 auto; min-height: 0; }
    /* Izquierda: por donde se navega.  Derecha: lo que se lee. */
    #lado {
        flex: 0 0 17em;
        overflow: auto;
        padding: 6px 0;
    }
    /* Los paneles se arrastran: cuanto sitio necesita cada uno depende de lo
     * que se este mirando -- nombres largos a la izquierda, derivaciones largas
     * a la derecha -- y eso no se puede decidir de antemano. */
    .tirador {
        flex: 0 0 4px;
        cursor: col-resize;
        background: var(--vscode-panel-border, rgba(128,128,128,.25));
    }
    .tirador:hover, .tirador.arrastrando {
        background: var(--vscode-focusBorder, #3b82f6);
    }
    .tirador.horizontal { cursor: row-resize; flex: 0 0 4px; width: auto; }
    #lado h3 {
        margin: 10px 10px 4px;
        font-size: 10px;
        text-transform: uppercase;
        letter-spacing: .05em;
        opacity: .6;
        font-weight: 600;
    }
    .item {
        display: flex;
        gap: 6px;
        padding: 2px 10px;
        cursor: pointer;
        white-space: nowrap;
        overflow: hidden;
        text-overflow: ellipsis;
    }
    .item:hover { background: var(--vscode-list-hoverBackground, rgba(128,128,128,.12)); }
    .item.sel { background: var(--vscode-list-activeSelectionBackground, rgba(90,150,255,.25)); }
    .item .n { margin-left: auto; opacity: .55; }
    #tabla { flex: 1 1 auto; overflow: auto; }
    table { border-collapse: collapse; width: 100%; }
    th {
        position: sticky;
        top: 0;
        text-align: left;
        padding: 4px 10px;
        font-size: 10px;
        text-transform: uppercase;
        letter-spacing: .05em;
        opacity: .6;
        background: var(--vscode-editor-background);
        border-bottom: 1px solid var(--vscode-panel-border, rgba(128,128,128,.3));
    }
    td { padding: 2px 10px; vertical-align: top; }
    tr.fila { cursor: pointer; }
    tr.fila:hover { background: var(--vscode-list-hoverBackground, rgba(128,128,128,.12)); }
    .linea { text-align: right; opacity: .5; font-family: var(--vscode-editor-font-family, monospace); }
    .sujeto { font-family: var(--vscode-editor-font-family, monospace); opacity: .85; }
    .dice { font-family: var(--vscode-editor-font-family, monospace); }
    /* Lo seguro que es cada cosa, que es lo primero que hay que saber de un
     * hecho: no vale lo mismo algo demostrado que algo supuesto. */
    .cert { font-size: 10px; padding: 0 5px; border-radius: 8px; white-space: nowrap; }
    .cert.demostrada { background: var(--vscode-charts-green, #2ea043); color: #08170c; }
    .cert.inferida  { background: var(--vscode-charts-blue, #3b82f6); color: #06122b; }
    .cert.desconocida { background: var(--vscode-charts-orange, #d29922); color: #241a02; }
    .origen { opacity: .7; font-size: 11px; }
    .detalle { opacity: .6; }
    .ambito { color: var(--vscode-charts-orange, #d29922); }
    .quien { opacity: .45; }
    /* La cabecera de todo lo que se sabe de una misma cosa. */
    tr.grupo td {
        padding-top: 10px;
        border-bottom: 1px solid var(--vscode-panel-border, rgba(128,128,128,.25));
    }
    .grupoTitulo { font-family: var(--vscode-editor-font-family, monospace); color: var(--vscode-charts-blue, #569cd6); }
    .grupoFn { opacity: .55; }
    .grupoN { float: right; opacity: .45; font-size: 11px; }
    /* Un hecho del que se sigue otro: se puede ir a el. */
    .enlace { cursor: pointer; text-decoration: underline; opacity: .85; }
    .enlace:hover { color: var(--vscode-textLink-activeForeground, #4daafc); }
    tr.resaltado { background: var(--vscode-editor-findMatchHighlightBackground, rgba(255,190,60,.35)); }
    /* Que mira cada analisis, bajo su nombre en la lista de la izquierda. */
    .proposito {
        padding: 0 10px 4px 10px;
        opacity: .55;
        font-size: 11px;
        white-space: normal;
    }
    #mensaje { padding: 16px; }
    #mensaje.error { color: var(--vscode-errorForeground); }
    .oculto { display: none; }
    #callado { padding: 8px 10px; border-top: 1px solid var(--vscode-panel-border, rgba(128,128,128,.35)); max-height: 30vh; overflow: auto; flex: 0 0 auto; }
    #callado summary { cursor: pointer; opacity: .8; }
    #callado table { margin-top: 6px; width: auto; }
    #callado td { padding: 1px 14px 1px 0; }
</style>
</head>
<body>
<header>
    <input type="search" id="buscar" placeholder="Buscar en lo que se sabe...">
    <select id="certeza">
        <option value="">Cualquier certeza</option>
        <option value="demostrada">Solo lo demostrado</option>
        <option value="inferida">Solo lo inferido</option>
        <option value="desconocida">Solo lo que no se sabe</option>
    </select>
    <label for="ver">Ver</label>
    <select id="ver">
        <option value="codigo">El codigo</option>
        <option value="ir">El IR</option>
    </select>
    <label for="agrupar">Agrupar por</label>
    <select id="agrupar">
        <option value="dicho">Lo que se sabe</option>
        <option value="sujeto">De que habla</option>
    </select>
    <button id="recargar" title="Volver a analizar y repintar">Actualizar</button>
    <span id="cuenta"></span>
</header>
<div id="mensaje" class="oculto"></div>
<div id="cuerpo">
    <div id="lado"></div>
    <div class="tirador" id="tiradorLado"></div>
    <div id="tabla"></div>
</div>
<div class="tirador horizontal" id="tiradorCallado"></div>
<details id="callado" class="oculto">
    <summary id="calladoTitulo">Por que no se sabe mas</summary>
    <div id="calladoCuerpo"></div>
</details>
<script nonce="${nonce}">
(function () {
    var api = acquireVsCodeApi();
    var elBuscar = document.getElementById('buscar');
    var elCerteza = document.getElementById('certeza');
    var elAgrupar = document.getElementById('agrupar');
    var elVer = document.getElementById('ver');
    var elRecargar = document.getElementById('recargar');
    var elCuenta = document.getElementById('cuenta');
    var elMensaje = document.getElementById('mensaje');
    var elCuerpo = document.getElementById('cuerpo');
    var elLado = document.getElementById('lado');
    var elTabla = document.getElementById('tabla');
    var elCallado = document.getElementById('callado');
    var elCalladoTitulo = document.getElementById('calladoTitulo');
    var elCalladoCuerpo = document.getElementById('calladoCuerpo');
    var elTiradorLado = document.getElementById('tiradorLado');
    var elTiradorCallado = document.getElementById('tiradorCallado');

    var hechos = [];
    var dominios = [];
    var funcionSel = '';
    var dominioSel = '';

    elRecargar.addEventListener('click', function () {
        api.postMessage({ type: 'refresh' });
    });
    elBuscar.addEventListener('input', pintarTabla);
    elCerteza.addEventListener('change', pintarTabla);
    elAgrupar.addEventListener('change', pintarTabla);
    elVer.addEventListener('change', pintarTabla);

    /**
     * Hace que un tirador reparta el sitio entre dos paneles.
     *
     * Cuanto necesita cada uno depende de lo que se este mirando -- nombres
     * largos a la izquierda, derivaciones largas a la derecha --, y eso no se
     * puede decidir de antemano por nadie que no sea quien esta mirando.
     */
    function hacerTirador(tirador, panel, vertical) {
        tirador.addEventListener('mousedown', function (ev) {
            ev.preventDefault();
            tirador.classList.add('arrastrando');
            var inicio = vertical ? ev.clientY : ev.clientX;
            var tamInicial = vertical ? panel.offsetHeight : panel.offsetWidth;
            function mover(e) {
                var delta = vertical ? (inicio - e.clientY) : (e.clientX - inicio);
                var nuevo = Math.max(80, tamInicial + delta);
                if (vertical) {
                    panel.style.maxHeight = 'none';
                    panel.style.height = nuevo + 'px';
                } else {
                    panel.style.flex = '0 0 ' + nuevo + 'px';
                }
            }
            function soltar() {
                tirador.classList.remove('arrastrando');
                document.removeEventListener('mousemove', mover);
                document.removeEventListener('mouseup', soltar);
            }
            document.addEventListener('mousemove', mover);
            document.addEventListener('mouseup', soltar);
        });
    }
    hacerTirador(elTiradorLado, elLado, false);
    hacerTirador(elTiradorCallado, elCallado, true);

    window.addEventListener('message', function (evento) {
        var d = evento.data;
        if (d.kind === 'loading') { mensaje('Analizando...', false); return; }
        if (d.kind === 'error') { mensaje(d.message, true); return; }
        var p = d.payload || {};
        if (p.error) { mensaje(p.error, true); return; }
        hechos = p.facts || [];
        dominios = p.domains || [];
        // Su indice ES su identidad: es como se referencian entre si cuando uno
        // se sigue de otro.
        for (var i = 0; i < hechos.length; i++) { hechos[i].idx = i; }
        if (hechos.length === 0 && dominios.length === 0) {
            mensaje('El analisis no dijo nada de este modulo.', false);
            return;
        }
        elMensaje.classList.add('oculto');
        elCuerpo.classList.remove('oculto');
        pintarLado();
        pintarTabla();
        pintarCallado();
    });

    function mensaje(texto, esError) {
        elMensaje.textContent = texto;
        elMensaje.className = esError ? 'error' : '';
        elCuerpo.classList.add('oculto');
        elCallado.classList.add('oculto');
    }

    /** Cuenta cuantos hechos hay por cada valor de un campo. */
    function contarPor(campo) {
        var cuenta = {};
        for (var i = 0; i < hechos.length; i++) {
            var k = hechos[i][campo] || '(sin nombre)';
            cuenta[k] = (cuenta[k] || 0) + 1;
        }
        var lista = [];
        for (var k2 in cuenta) {
            if (Object.prototype.hasOwnProperty.call(cuenta, k2)) {
                lista.push({ nombre: k2, n: cuenta[k2] });
            }
        }
        lista.sort(function (a, b) { return b.n - a.n; });
        return lista;
    }

    function pintarLado() {
        elLado.innerHTML = '';
        agregarSeccion('Funcion', contarPor('functionDisplay'), funcionSel, false,
                       function (v) { funcionSel = v; pintarLado(); pintarTabla(); });
        agregarSeccion('Que se mira', contarPor('domain'), dominioSel, true,
                       function (v) { dominioSel = v; pintarLado(); pintarTabla(); });
    }

    /** Que mira un analisis, en una frase; vacio si no se sabe. */
    function propositoDe(dominio) {
        for (var i = 0; i < dominios.length; i++) {
            if (dominios[i].domain === dominio) { return dominios[i].purpose || ''; }
        }
        return '';
    }

    function agregarSeccion(titulo, lista, seleccion, conProposito, alElegir) {
        var h = document.createElement('h3');
        h.textContent = titulo;
        elLado.appendChild(h);

        elLado.appendChild(hacerItem('Todo', hechos.length, seleccion === '',
                                     function () { alElegir(''); }));
        for (var i = 0; i < lista.length; i++) {
            (function (entrada) {
                var prop = conProposito ? propositoDe(entrada.nombre) : '';
                var item = hacerItem(entrada.nombre, entrada.n,
                                     seleccion === entrada.nombre,
                                     function () { alElegir(entrada.nombre); });
                /* Doble clic sobre una funcion: se abre.  Un clic filtra, que
                 * es lo que se hace casi siempre desde aqui. */
                if (titulo === 'Funcion' && entrada.nombre !== '(sin nombre)') {
                    item.title = 'Doble clic para abrir ' + entrada.nombre;
                    item.addEventListener('dblclick', function () {
                        api.postMessage({ type: 'goto', nombre: entrada.nombre,
                                          name: entrada.nombre });
                    });
                }
                if (prop) { item.title = prop; }
                elLado.appendChild(item);
                /* El nombre de un analisis -- "asa.rangos" -- no dice que mira.
                 * Puesto debajo, se entiende sin tener que probar. */
                if (prop) {
                    var d = document.createElement('div');
                    d.className = 'proposito';
                    d.textContent = prop;
                    elLado.appendChild(d);
                }
            })(lista[i]);
        }
    }

    function hacerItem(texto, n, seleccionado, alPulsar) {
        var d = document.createElement('div');
        d.className = 'item' + (seleccionado ? ' sel' : '');
        d.title = texto;
        var t = document.createElement('span');
        t.textContent = texto;
        d.appendChild(t);
        var c = document.createElement('span');
        c.className = 'n';
        c.textContent = String(n);
        d.appendChild(c);
        d.addEventListener('click', alPulsar);
        return d;
    }

    function filtrados() {
        var texto = elBuscar.value.toLowerCase();
        var cert = elCerteza.value;
        var salida = [];
        for (var i = 0; i < hechos.length; i++) {
            var f = hechos[i];
            if (funcionSel &&
                (f.functionDisplay || f.function || '(sin nombre)') !== funcionSel) { continue; }
            if (dominioSel && (f.domain || '(sin nombre)') !== dominioSel) { continue; }
            if (cert && f.certainty !== cert) { continue; }
            if (texto) {
                var todo = (f.label + ' ' + f.subject + ' ' + f.detail + ' ' +
                            f.code + ' ' + (f.functionDisplay || f.function) + ' ' +
                            (f.subjectText || '')).toLowerCase();
                if (todo.indexOf(texto) < 0) { continue; }
            }
            salida.push(f);
        }
        return salida;
    }

    /**
     * De QUE habla un hecho.
     *
     * Por defecto, el CoDIGO: una operacion del IR identifica sin lugar a
     * dudas y no dice nada a quien no lo tiene delante, que es casi siempre. La
     * linea del fuente es de lo que uno habla cuando programa.  El IR sigue
     * estando, a un clic, porque para leer codigo generado es justo lo que
     * hace falta.
     */
    function deQue(f) {
        if (elVer.value === 'codigo' && f.sourceText) { return f.sourceText; }
        if (f.subjectText) { return f.subjectText; }
        if (f.subject === 'funcion') {
            return 'la funcion ' + (f.functionDisplay || f.function || '');
        }
        if (f.subject === 'modulo') { return 'el modulo entero'; }
        if (f.subject === 'bloque') { return 'el bloque #' + f.subjectId; }
        if (f.subject === 'valor') { return 'el valor %' + f.subjectId; }
        return f.subject || '';
    }

    /**
     * Los hechos AGRUPADOS por aquello de lo que hablan.
     *
     * Sueltos son una lista de afirmaciones sin relacion aparente: ocho filas
     * que dicen "valor" y un rango cada una.  Juntos por sujeto se leen como lo
     * que son -- todo lo que se sabe de ESTE valor --, que es la pregunta que
     * uno trae.
     */
    function agrupar(lista) {
        var orden = [];
        var porClave = {};
        for (var i = 0; i < lista.length; i++) {
            var f = lista[i];
            var clave = (f.function || '') + '#' + f.subject + '#' + f.subjectId;
            if (!porClave[clave]) {
                porClave[clave] = { titulo: deQue(f),
                                    ir: f.subjectText || '',
                                    fn: f.functionDisplay || f.function || '',
                                    linea: f.line, hechos: [] };
                orden.push(clave);
            }
            porClave[clave].hechos.push(f);
        }
        var salida = [];
        for (var k = 0; k < orden.length; k++) { salida.push(porClave[orden[k]]); }
        return salida;
    }

    /**
     * Los hechos agrupados por LO QUE DICEN.
     *
     * Es la forma que quita la repeticion aparente.  El analisis no repite
     * nada -- comprobado: cero hechos identicos sobre el mismo sujeto --, lo
     * que pasa es que la MISMA frase vale para muchisimos valores: "puede
     * referirse a cualquier memoria" sale 120 veces porque hay 120 valores de
     * los que no se sabe a donde apuntan.  Ciento veinte filas iguales parecen
     * un fallo; una fila que dice "120 valores" es un dato.
     */
    function agruparPorDicho(lista) {
        var orden = [];
        var porClave = {};
        for (var i = 0; i < lista.length; i++) {
            var f = lista[i];
            var clave = (f.label || f.code) + '#' + f.certainty + '#' + f.domain;
            if (!porClave[clave]) {
                porClave[clave] = { dicho: f.label || f.code, certeza: f.certainty,
                                    dominio: f.domain, hechos: [] };
                orden.push(clave);
            }
            porClave[clave].hechos.push(f);
        }
        var salida = [];
        for (var k = 0; k < orden.length; k++) { salida.push(porClave[orden[k]]); }
        // Lo que mas se repite arriba: es lo que domina el modulo.
        salida.sort(function (a, b) { return b.hechos.length - a.hechos.length; });
        return salida;
    }

    function pintarTabla() {
        var lista = filtrados();
        elTabla.innerHTML = '';
        if (elAgrupar.value === 'sujeto') {
            pintarPorSujeto(lista);
        } else {
            pintarPorDicho(lista);
        }
    }

    /** Una fila por COSA de la que se habla, con todo lo que se sabe de ella. */
    function pintarPorSujeto(lista) {
        var grupos = agrupar(lista);
        elCuenta.textContent = lista.length + ' de ' + hechos.length +
            ' hechos, sobre ' + grupos.length + ' cosas';

        var tabla = nuevaTabla(['Linea', 'Que se sabe', 'Certeza', 'Como se sabe']);
        for (var g = 0; g < grupos.length; g++) {
            tabla.appendChild(hacerCabeceraGrupo(grupos[g]));
            for (var h = 0; h < grupos[g].hechos.length; h++) {
                tabla.appendChild(hacerFila(grupos[g].hechos[h]));
            }
        }
        elTabla.appendChild(tabla);
    }

    /** Una fila por COSA QUE SE SABE, con las cosas de las que se sabe. */
    function pintarPorDicho(lista) {
        var grupos = agruparPorDicho(lista);
        elCuenta.textContent = lista.length + ' de ' + hechos.length +
            ' hechos, ' + grupos.length + ' cosas distintas';

        var tabla = nuevaTabla(['Cuantos', 'Que se sabe', 'Certeza', 'De que']);
        for (var g = 0; g < grupos.length; g++) {
            tabla.appendChild(hacerFilaDicho(grupos[g]));
        }
        elTabla.appendChild(tabla);
    }

    function nuevaTabla(titulos) {
        var tabla = document.createElement('table');
        var cab = document.createElement('tr');
        for (var t = 0; t < titulos.length; t++) {
            var th = document.createElement('th');
            th.textContent = titulos[t];
            cab.appendChild(th);
        }
        tabla.appendChild(cab);
        return tabla;
    }

    /** Una cosa sabida, y de cuantas se sabe. */
    function hacerFilaDicho(grupo) {
        var tr = document.createElement('tr');
        tr.className = 'fila';

        var n = grupo.hechos.length;
        tr.appendChild(celda(String(n), 'linea'));
        tr.appendChild(celda(grupo.dicho, 'dice'));

        var tdC = document.createElement('td');
        var chip = document.createElement('span');
        chip.className = 'cert ' + (grupo.certeza || 'desconocida');
        chip.textContent = grupo.certeza || '?';
        tdC.appendChild(chip);
        tr.appendChild(tdC);

        /* De QUE se sabe.  Se nombran; si son muchas, las primeras y cuantas
         * quedan -- pero se pueden abrir todas, que para eso esta el otro
         * agrupado --. */
        var tdD = document.createElement('td');
        tdD.className = 'origen';
        var cuantas = Math.min(n, 8);
        for (var i = 0; i < cuantas; i++) {
            (function (f) {
                var s = document.createElement('span');
                s.className = 'enlace';
                s.textContent = deQue(f);
                s.title = 'linea ' + f.line + ' en ' + (f.functionDisplay || f.function || '');
                s.addEventListener('click', function (ev) {
                    ev.stopPropagation();
                    if (f.line > 0) { api.postMessage({ type: 'reveal', line: f.line }); }
                });
                tdD.appendChild(s);
                tdD.appendChild(document.createTextNode('  '));
            })(grupo.hechos[i]);
        }
        if (n > cuantas) {
            var mas = document.createElement('span');
            mas.className = 'enlace';
            mas.textContent = 'y ' + (n - cuantas) + ' mas';
            mas.title = 'Ver una fila por cada una';
            mas.addEventListener('click', function (ev) {
                ev.stopPropagation();
                elBuscar.value = grupo.dicho;
                elAgrupar.value = 'sujeto';
                pintarTabla();
            });
            tdD.appendChild(mas);
        }
        tr.appendChild(tdD);
        return tr;
    }

    /** La fila que encabeza todo lo que se sabe de una misma cosa. */
    function hacerCabeceraGrupo(grupo) {
        var tr = document.createElement('tr');
        tr.className = 'grupo';
        var td = document.createElement('td');
        td.colSpan = 4;
        var t = document.createElement('span');
        t.className = 'grupoTitulo';
        t.textContent = grupo.titulo;
        td.appendChild(t);
        /* Mostrando el codigo, dos valores de la misma linea tienen el mismo
         * titulo.  La operacion del IR, en pequeno, es lo que los separa --
         * sin obligar a leerla para entender de que se habla --. */
        if (grupo.ir && grupo.ir !== grupo.titulo) {
            var ir = document.createElement('span');
            ir.className = 'grupoFn';
            ir.textContent = '  ' + grupo.ir;
            td.appendChild(ir);
        }
        if (grupo.fn) {
            // El nombre lleva a la funcion: es el sitio natural desde donde
            // querer ir, y hasta ahora era solo texto.
            var f = document.createElement('span');
            f.className = 'grupoFn enlace';
            f.textContent = '  en ' + grupo.fn;
            f.title = 'Abrir ' + grupo.fn;
            f.addEventListener('click', function (ev) {
                ev.stopPropagation();
                api.postMessage({ type: 'goto', name: grupo.fn });
            });
            td.appendChild(f);
        }
        var n = document.createElement('span');
        n.className = 'grupoN';
        n.textContent = grupo.hechos.length === 1
            ? '1 cosa sabida' : grupo.hechos.length + ' cosas sabidas';
        td.appendChild(n);
        tr.appendChild(td);
        if (grupo.linea > 0) {
            tr.style.cursor = 'pointer';
            tr.title = 'Ir a la linea ' + grupo.linea;
            tr.addEventListener('click', function () {
                api.postMessage({ type: 'reveal', line: grupo.linea });
            });
        }
        return tr;
    }

    function hacerFila(f) {
        var tr = document.createElement('tr');
        tr.className = 'fila';
        tr.id = 'hecho' + f.idx;
        if (f.line > 0) {
            tr.addEventListener('click', function () {
                api.postMessage({ type: 'reveal', line: f.line });
            });
            tr.title = 'Ir a la linea ' + f.line;
        }

        tr.appendChild(celda(f.line > 0 ? String(f.line) : '', 'linea'));

        // Que se sabe, y el detalle solo si anade algo: repetirlo al lado era
        // media tabla diciendo dos veces lo mismo.
        var tdQ = document.createElement('td');
        tdQ.className = 'dice';
        var texto = f.label || f.code || '';
        tdQ.appendChild(document.createTextNode(texto));
        if (f.detail && texto.indexOf(f.detail) < 0) {
            var det = document.createElement('span');
            det.className = 'detalle';
            det.textContent = '  ' + f.detail;
            tdQ.appendChild(det);
        }
        // Donde vale, si no vale en todas partes.  Callarlo seria afirmar en
        // general algo que solo se comprobo para una maquina.
        var ambito = [];
        if (f.isa) { ambito.push(f.isa); }
        if (f.os) { ambito.push(f.os); }
        if (f.backend) { ambito.push(f.backend); }
        if (ambito.length) {
            var am = document.createElement('span');
            am.className = 'ambito';
            am.textContent = '  solo en ' + ambito.join('/');
            tdQ.appendChild(am);
        }
        tr.appendChild(tdQ);

        var tdC = document.createElement('td');
        var chip = document.createElement('span');
        chip.className = 'cert ' + (f.certainty || 'desconocida');
        chip.textContent = f.certainty || '?';
        tdC.appendChild(chip);
        tr.appendChild(tdC);

        // COMO se sabe: la regla, de que otros hechos se sigue, y quien lo
        // dijo.  Es lo que convierte una afirmacion en algo comprobable.
        var tdK = document.createElement('td');
        tdK.className = 'origen';
        var partes = [];
        if (f.rule) { partes.push('por ' + f.rule); }
        else if (f.source) { partes.push(f.source); }
        tdK.appendChild(document.createTextNode(partes.join(' ')));

        var de = f.from || [];
        if (de.length) {
            tdK.appendChild(document.createTextNode('  se sigue de '));
            for (var d = 0; d < de.length; d++) {
                (function (idFuente) {
                    var enlace = document.createElement('span');
                    enlace.className = 'enlace';
                    enlace.textContent = '#' + idFuente;
                    enlace.title = descripcionCorta(idFuente);
                    enlace.addEventListener('click', function (ev) {
                        ev.stopPropagation();
                        irAlHecho(idFuente);
                    });
                    tdK.appendChild(enlace);
                    if (d + 1 < de.length) {
                        tdK.appendChild(document.createTextNode(', '));
                    }
                })(de[d]);
            }
        }
        if (f.producer) {
            var quien = document.createElement('span');
            quien.className = 'quien';
            quien.textContent = '  ' + f.producer;
            tdK.appendChild(quien);
        }
        tr.appendChild(tdK);
        return tr;
    }

    /** Una linea con lo que dice el hecho numero @p id, para el emergente. */
    function descripcionCorta(id) {
        var f = hechos[id];
        if (!f) { return 'hecho #' + id; }
        return deQue(f) + ': ' + (f.label || f.code || '');
    }

    /**
     * Lleva al hecho del que se sigue este.
     *
     * Puede estar filtrado fuera -- es de otra funcion, o de un analisis que no
     * se esta mirando --, asi que se quitan los filtros antes de buscarlo: mas
     * vale ensenarlo que decir que no esta.
     */
    function irAlHecho(id) {
        var destino = document.getElementById('hecho' + id);
        if (!destino) {
            funcionSel = '';
            dominioSel = '';
            elBuscar.value = '';
            elCerteza.value = '';
            pintarLado();
            pintarTabla();
            destino = document.getElementById('hecho' + id);
        }
        if (destino) {
            destino.scrollIntoView({ block: 'center' });
            destino.classList.add('resaltado');
            setTimeout(function () { destino.classList.remove('resaltado'); }, 1600);
        }
    }

    function celda(texto, clase) {
        var td = document.createElement('td');
        td.className = clase;
        td.textContent = texto;
        return td;
    }

    /** Lo que el analisis miro y de lo que NO supo decir nada, y por que. */
    function pintarCallado() {
        var conMotivos = [];
        for (var i = 0; i < dominios.length; i++) {
            if (dominios[i].silent > 0 || (dominios[i].unknown || []).length) {
                conMotivos.push(dominios[i]);
            }
        }
        if (conMotivos.length === 0) { elCallado.classList.add('oculto'); return; }
        elCallado.classList.remove('oculto');

        var mudos = 0;
        for (var m = 0; m < conMotivos.length; m++) { mudos += conMotivos[m].silent || 0; }
        elCalladoTitulo.textContent = 'Por que no se sabe mas   ' + mudos +
            ' sin respuesta en ' + conMotivos.length + ' analisis';

        elCalladoCuerpo.innerHTML = '';
        for (var d = 0; d < conMotivos.length; d++) {
            var dom = conMotivos[d];
            var titulo = document.createElement('div');
            titulo.style.opacity = '.75';
            titulo.style.marginTop = '8px';
            titulo.textContent = dom.domain + ': miro ' + dom.looked +
                ', dijo algo de ' + dom.facts + ', se callo en ' + dom.silent;
            elCalladoCuerpo.appendChild(titulo);

            var tabla = document.createElement('table');
            var motivos = dom.unknown || [];
            for (var u = 0; u < motivos.length; u++) {
                var tr = document.createElement('tr');
                tr.appendChild(celda(String(motivos[u].times), 'linea'));
                tr.appendChild(celda(motivos[u].code, 'dice'));
                tabla.appendChild(tr);
            }
            elCalladoCuerpo.appendChild(tabla);
        }
    }
}());
</script>
</body>
</html>`;
}
