/**
 * @file inspect.ts
 * @brief Comandos que abren las vistas del compilador.
 *
 * Cada comando pide una vista al servidor y la muestra: las que son texto
 * (IR, bytecode, ensamblador, informes) van a un documento virtual, para poder
 * buscarlas y compararlas como cualquier fichero; los diagramas y la vista
 * correlacionada van a su propio panel.
 *
 * Los informes se formatean aqui, no en el servidor: el servidor devuelve
 * datos, y como se ven es cosa de quien los ensena.
 */

import * as path from 'path';
import * as vscode from 'vscode';

import { VestaLanguageClient, describeError } from '../lsp/client';
import {
    AotCompatResponse,
    AsmResponse,
    ComplexityResponse,
    ComptimeValuesResponse,
    DiagramKind,
    FunctionsResponse,
    IrPhase,
    MacroExpandResponse,
    ModesResponse,
    TextResponse,
    VestaMethod,
} from '../lsp/protocol';
import { DiagramPanel } from '../views/diagramPanel';
import { MachineViewPanel } from '../views/machinePanel';
import { VestaTextViewProvider } from '../views/textViews';
import {
    activeVestaDocument,
    aotTier,
    diagramCost,
    diagramFormat,
    inspectTarget,
} from '../util/settings';

/** Contexto compartido por todos los comandos de inspeccion. */
export interface InspectContext {
    client: VestaLanguageClient;
    views: VestaTextViewProvider;
}

/**
 * @brief Registra todos los comandos de inspeccion.
 * @param context Contexto de la extension, donde se guardan las suscripciones.
 * @param deps    Cliente del servidor y proveedor de vistas de texto.
 */
export function registerInspectCommands(
    context: vscode.ExtensionContext,
    deps: InspectContext,
): void {
    const register = (id: string, handler: () => Promise<void>): void => {
        context.subscriptions.push(
            vscode.commands.registerCommand(id, async () => {
                try {
                    await handler();
                } catch (err) {
                    void vscode.window.showErrorMessage(`Vesta: ${describeError(err)}`);
                }
            }),
        );
    };

    register('vesta.showIr', () => showIr(deps));
    register('vesta.showIrDiff', () => showIrDiff(deps));
    register('vesta.showBytecode', () => showBytecode(deps));
    register('vesta.showJitAsm', () => showAsm(deps, 'jit'));
    register('vesta.showAotAsm', () => showAsm(deps, 'aot'));
    register('vesta.showComplexity', () => showComplexity(deps));
    register('vesta.showModes', () => showModes(deps));
    register('vesta.showAotCompat', () => showAotCompat(deps));
    register('vesta.showMacroExpand', () => showMacroExpand(deps));
    register('vesta.showComptimeValues', () => showComptimeValues(deps));
    register('vesta.showAsa', () => showAsa(deps));
    register('vesta.showDiagram', () => showDiagram(deps));
    register('vesta.showMachineView', () => showMachineView(deps));
}

/**
 * @brief Pide una vista al servidor mostrando un indicador de progreso.
 * @tparam T     Forma de la respuesta.
 * @param client Cliente del servidor.
 * @param title  Texto del indicador.
 * @param method Metodo a invocar.
 * @param params Parametros de la peticion.
 * @return La respuesta del servidor.
 */
async function request<T>(
    client: VestaLanguageClient,
    title: string,
    method: string,
    params: Record<string, unknown>,
): Promise<T> {
    return vscode.window.withProgress(
        { location: vscode.ProgressLocation.Window, title: `Vesta: ${title}` },
        () => client.request<T>(method, params),
    );
}

/**
 * @brief Deja elegir una funcion del modulo.
 * @param client      Cliente del servidor.
 * @param uri         Documento en curso.
 * @param allowModule Si es cierto, ofrece tambien "el modulo entero".
 * @return El nombre elegido (vacio para el modulo entero), o undefined si se
 *         cancelo la eleccion.
 */
async function pickFunction(
    client: VestaLanguageClient,
    uri: string,
    allowModule: boolean,
): Promise<string | undefined> {
    let names: string[] = [];
    try {
        const response = await client.request<FunctionsResponse>(VestaMethod.Functions, { uri });
        names = (response.functions ?? []).map(fn => fn.name);
    } catch {
        // Sin lista de funciones se sigue adelante: el servidor elige por su
        // cuenta la primera compilable.
        return '';
    }

    if (names.length === 0) {
        return '';
    }

    interface Item extends vscode.QuickPickItem {
        value: string;
    }
    const items: Item[] = [];
    if (allowModule) {
        items.push({
            label: 'El modulo entero',
            detail: 'Sin aislar ninguna funcion',
            value: '',
        });
    }
    for (const name of names) {
        items.push({ label: name, value: name });
    }

    const choice = await vscode.window.showQuickPick(items, {
        placeHolder: 'Elige la funcion',
    });
    return choice?.value;
}

/** @brief Nombre corto del fichero activo, para titular las vistas. */
function baseName(document: vscode.TextDocument): string {
    return path.basename(document.uri.fsPath);
}

/** @brief Abre la vista del IR, preguntando la fase. */
async function showIr(deps: InspectContext): Promise<void> {
    const document = activeVestaDocument();
    if (!document) {
        return;
    }

    interface Item extends vscode.QuickPickItem {
        phase: IrPhase;
    }
    const choice = await vscode.window.showQuickPick<Item>(
        [
            {
                label: 'Despues de optimizar',
                detail: 'El IR que llega al generador de codigo',
                phase: 'post',
            },
            {
                label: 'Antes de optimizar',
                detail: 'El IR tal y como sale del bajado',
                phase: 'pre',
            },
        ],
        { placeHolder: 'Que fase del IR' },
    );
    if (!choice) {
        return;
    }

    const target = inspectTarget();
    const response = await request<TextResponse>(deps.client, 'generando el IR', VestaMethod.Ir, {
        uri: document.uri.toString(),
        phase: choice.phase,
        os: target.os,
        arch: target.arch,
    });
    if (!showErrorIfAny(response)) {
        await deps.views.show(
            `ir-${choice.phase}`,
            `${baseName(document)} (IR ${choice.phase}).ir`,
            response.text ?? '',
        );
    }
}

/** @brief Abre el diff del IR de una funcion, antes y despues de optimizar. */
async function showIrDiff(deps: InspectContext): Promise<void> {
    const document = activeVestaDocument();
    if (!document) {
        return;
    }
    const uri = document.uri.toString();
    const functionName = await pickFunction(deps.client, uri, true);
    if (functionName === undefined) {
        return;
    }

    const response = await request<TextResponse>(
        deps.client,
        'comparando el IR',
        VestaMethod.IrDiff,
        { uri, function: functionName },
    );
    if (!showErrorIfAny(response)) {
        const label = functionName || 'modulo';
        await deps.views.show(
            'ir-diff',
            `${baseName(document)} (${label}).diff`,
            response.text ?? '',
            'diff',
        );
    }
}

/** @brief Abre el bytecode del modulo o de una funcion. */
async function showBytecode(deps: InspectContext): Promise<void> {
    const document = activeVestaDocument();
    if (!document) {
        return;
    }
    const uri = document.uri.toString();
    const functionName = await pickFunction(deps.client, uri, true);
    if (functionName === undefined) {
        return;
    }

    const target = inspectTarget();
    const response = await request<TextResponse>(
        deps.client,
        'generando el bytecode',
        VestaMethod.Bytecode,
        { uri, function: functionName, os: target.os, arch: target.arch },
    );
    if (!showErrorIfAny(response)) {
        const label = functionName || 'modulo';
        await deps.views.show(
            'bytecode',
            `${baseName(document)} (${label}).vel`,
            response.text ?? '',
            // El ensamblador de la maquina virtual lo aporta otra extension del
            // repositorio; si no esta instalada el texto se ve sin colores.
            'vestaasm',
        );
    }
}

/**
 * @brief Abre el desensamblado de una funcion.
 * @param deps    Contexto de los comandos.
 * @param backend Generador de codigo del que pedirlo.
 */
async function showAsm(deps: InspectContext, backend: 'jit' | 'aot'): Promise<void> {
    const document = activeVestaDocument();
    if (!document) {
        return;
    }
    const uri = document.uri.toString();
    const functionName = await pickFunction(deps.client, uri, false);
    if (functionName === undefined) {
        return;
    }

    const target = inspectTarget();
    const method = backend === 'jit' ? VestaMethod.JitAsm : VestaMethod.AotAsm;
    const response = await request<AsmResponse>(
        deps.client,
        `compilando (${backend.toUpperCase()})`,
        method,
        {
            uri,
            function: functionName,
            os: target.os,
            arch: target.arch,
            tier: aotTier(),
        },
    );

    if (showErrorIfAny(response)) {
        return;
    }
    if (response.unsupported || response.incompatible) {
        void vscode.window.showWarningMessage(
            `Vesta: ${response.reason ?? 'la funcion no se puede compilar en este modo.'}`,
        );
        return;
    }

    const header = buildAsmHeader(response, backend);
    await deps.views.show(
        `asm-${backend}`,
        `${baseName(document)} (${response.function ?? functionName} ${backend.toUpperCase()}).asm`,
        header + (response.text ?? ''),
    );
}

/**
 * @brief Compone la cabecera informativa del desensamblado.
 * @param response Respuesta del servidor.
 * @param backend  Generador de codigo usado.
 * @return Texto de cabecera, terminado en linea en blanco.
 */
function buildAsmHeader(response: AsmResponse, backend: string): string {
    const lines: string[] = [];
    lines.push(`; funcion    : ${response.function ?? ''}`);
    lines.push(`; generador  : ${backend.toUpperCase()}`);
    if (response.bytes !== undefined) {
        lines.push(`; tamano     : ${response.bytes} bytes`);
    }
    if (response.instructions !== undefined) {
        lines.push(`; instruccs. : ${response.instructions}`);
    }
    for (const arg of response.args ?? []) {
        lines.push(`; argumento  : ${arg.reg} = ${arg.name}`);
    }
    for (const reloc of response.relocs ?? []) {
        lines.push(`; reubicacion: +${reloc.offset} ${reloc.kind} ${reloc.symbol}`);
    }
    lines.push('');
    return lines.join('\n');
}

/** @brief Abre el informe de coste por funcion. */
async function showComplexity(deps: InspectContext): Promise<void> {
    const document = activeVestaDocument();
    if (!document) {
        return;
    }

    const response = await request<ComplexityResponse>(
        deps.client,
        'calculando el coste',
        VestaMethod.Complexity,
        { uri: document.uri.toString() },
    );
    if (showErrorIfAny(response)) {
        return;
    }

    const entries = response.functions ?? [];
    const rows = entries.map(fn => [
        fn.name,
        fn.total,
        fn.partial,
        fn.total_confidence,
        String(fn.max_loop_depth),
        fn.recursive ? 'si' : 'no',
        fn.declared ?? '',
        fn.contract_mismatch ? 'NO CUADRA' : '',
    ]);

    const text =
        titleFor(document, 'Coste por funcion') +
        renderTable(
            ['funcion', 'total', 'propio', 'confianza', 'bucles', 'recursiva', 'declarado', 'contrato'],
            rows,
        ) +
        '\n' +
        'El coste "propio" es el del cuerpo sin contar las llamadas; el "total"\n' +
        'incluye lo que cuestan.  La columna "contrato" avisa cuando lo declarado\n' +
        'con @complexity no cuadra con lo que el compilador infiere.\n';

    await deps.views.show('complexity', `${baseName(document)} (coste).txt`, text);
}

/** @brief Abre el informe de los tres modos de ejecucion. */
async function showModes(deps: InspectContext): Promise<void> {
    const document = activeVestaDocument();
    if (!document) {
        return;
    }

    const response = await request<ModesResponse>(
        deps.client,
        'analizando los modos',
        VestaMethod.Modes,
        { uri: document.uri.toString(), tier: aotTier() },
    );
    if (showErrorIfAny(response)) {
        return;
    }

    const parts: string[] = [titleFor(document, 'Los tres modos de ejecucion')];
    for (const mode of response.modes ?? []) {
        parts.push(`[${mode.mode}]`);
        if (mode.ok !== undefined) {
            parts.push(`  compila       : ${mode.ok ? 'si' : 'no'}`);
        }
        if (mode.errors !== undefined) {
            parts.push(`  errores       : ${mode.errors}`);
        }
        if (mode.warnings !== undefined) {
            parts.push(`  avisos        : ${mode.warnings}`);
        }
        if (mode.tier) {
            parts.push(`  nivel         : ${mode.tier}`);
        }
        if (mode.compatible !== undefined) {
            parts.push(`  compatible    : ${mode.compatible ? 'si' : 'no'}`);
        }
        if (mode.compilable_functions?.length) {
            parts.push(`  compiladas    : ${mode.compilable_functions.join(', ')}`);
        }
        if (mode.fallback_functions?.length) {
            parts.push(`  al interprete : ${mode.fallback_functions.join(', ')}`);
        }
        for (const issue of mode.issues ?? []) {
            parts.push(`  - ${issue.fn_name} (linea ${issue.source_line}): ${issue.reason}`);
        }
        if (mode.note) {
            parts.push(`  nota          : ${mode.note}`);
        }
        parts.push('');
    }

    await deps.views.show('modes', `${baseName(document)} (modos).txt`, parts.join('\n'));
}

/** @brief Abre el informe de compatibilidad con la compilacion anticipada. */
async function showAotCompat(deps: InspectContext): Promise<void> {
    const document = activeVestaDocument();
    if (!document) {
        return;
    }

    const tier = aotTier();
    const response = await request<AotCompatResponse>(
        deps.client,
        'comprobando la compatibilidad',
        VestaMethod.AotCompat,
        { uri: document.uri.toString(), tier },
    );
    if (showErrorIfAny(response)) {
        return;
    }

    const parts: string[] = [titleFor(document, `Compatibilidad AOT (nivel ${tier})`)];
    parts.push(`Compatible: ${response.compatible ? 'si' : 'no'}`);
    parts.push('');

    const issues = response.issues ?? [];
    if (issues.length > 0) {
        parts.push('Lo que lo impide:');
        parts.push(
            renderTable(
                ['funcion', 'linea', 'operacion', 'motivo'],
                issues.map(i => [i.fn_name, String(i.source_line), i.op, i.reason]),
            ),
        );
    }

    const okFunctions = response.ok_functions ?? [];
    if (okFunctions.length > 0) {
        parts.push(`Funciones sin problemas (${okFunctions.length}):`);
        parts.push('  ' + okFunctions.join('\n  '));
    }

    await deps.views.show('aot-compat', `${baseName(document)} (AOT).txt`, parts.join('\n'));
}

/** @brief Abre el codigo que generan las macros del modulo. */
async function showMacroExpand(deps: InspectContext): Promise<void> {
    const document = activeVestaDocument();
    if (!document) {
        return;
    }

    const response = await request<MacroExpandResponse>(
        deps.client,
        'expandiendo las macros',
        VestaMethod.MacroExpand,
        { uri: document.uri.toString() },
    );
    if (showErrorIfAny(response)) {
        return;
    }

    const expansions = response.expansions ?? [];
    const skipped = response.skipped ?? [];
    if (expansions.length === 0 && skipped.length === 0) {
        void vscode.window.showInformationMessage('Vesta: el modulo no usa macros.');
        return;
    }

    const parts: string[] = [titleFor(document, 'Codigo generado por las macros')];
    for (const expansion of expansions) {
        parts.push(`// ${expansion.macro_name}(${expansion.args.join(', ')})`);
        parts.push(`// en ${expansion.call_site_loc}`);
        parts.push(expansion.generated_code);
        parts.push('');
    }
    if (skipped.length > 0) {
        parts.push('// Macros que no se pudieron expandir:');
        for (const skip of skipped) {
            parts.push(`//   ${skip.name}: ${skip.reason}`);
        }
    }

    await deps.views.show(
        'macro-expand',
        `${baseName(document)} (macros).vx`,
        parts.join('\n'),
        'vesta',
    );
}

/** @brief Abre los valores que el compilador resolvio al compilar. */
async function showComptimeValues(deps: InspectContext): Promise<void> {
    const document = activeVestaDocument();
    if (!document) {
        return;
    }

    const response = await request<ComptimeValuesResponse>(
        deps.client,
        'evaluando el codigo de compilacion',
        VestaMethod.ComptimeValues,
        { uri: document.uri.toString() },
    );
    if (showErrorIfAny(response)) {
        return;
    }

    const values = response.values ?? [];
    if (values.length === 0) {
        void vscode.window.showInformationMessage(
            'Vesta: el modulo no tiene valores resueltos en tiempo de compilacion.',
        );
        return;
    }

    const text =
        titleFor(document, 'Valores resueltos al compilar') +
        renderTable(
            ['nombre', 'ambito', 'tipo', 'valor'],
            values.map(v => [v.name, v.scope, v.type_kind, v.value_str]),
        );

    await deps.views.show('comptime', `${baseName(document)} (comptime).txt`, text);
}

/**
 * @brief Abre todo lo que el compilador sabe del modulo.
 *
 * Es el volcado completo, el mismo que da la linea de ordenes: los hechos, de
 * donde sale cada uno y con que certeza, y lo que se miro sin sacar nada.  Se
 * abre entero a proposito -- pedirlo por partes obligaria a saber que se busca
 * antes de mirarlo --; lo que se lee al vuelo mientras se escribe son las
 * anotaciones al final de cada linea, que es otra cosa.
 *
 * @param deps Contexto de los comandos.
 */
async function showAsa(deps: InspectContext): Promise<void> {
    const document = activeVestaDocument();
    if (!document) {
        return;
    }

    const response = await request<TextResponse>(
        deps.client,
        'reuniendo lo que sabe el compilador',
        VestaMethod.Asa,
        { uri: document.uri.toString() },
    );
    if (!showErrorIfAny(response)) {
        await deps.views.show(
            'asa',
            `${baseName(document)} (lo que se sabe).txt`,
            response.text ?? '',
        );
    }
}

/** @brief Abre un diagrama del modulo. */
async function showDiagram(deps: InspectContext): Promise<void> {
    const document = activeVestaDocument();
    if (!document) {
        return;
    }
    const uri = document.uri.toString();

    // El campo no se puede llamar `kind`: el editor reserva ese nombre en los
    // elementos del selector para distinguir separadores de opciones.
    interface Item extends vscode.QuickPickItem {
        diagram: DiagramKind;
    }
    const choice = await vscode.window.showQuickPick<Item>(
        [
            { label: 'IR optimizado', detail: 'Grafo de bloques tras optimizar', diagram: 'ir-post' },
            { label: 'IR sin optimizar', detail: 'Grafo de bloques recien bajado', diagram: 'ir-pre' },
            { label: 'Arbol sintactico', detail: 'Estructura del fuente', diagram: 'ast' },
            { label: 'Bytecode', detail: 'Flujo del codigo de la maquina virtual', diagram: 'vel' },
            { label: 'Codigo maquina', detail: 'Grafo del codigo nativo de una funcion', diagram: 'asm' },
        ],
        { placeHolder: 'Que diagrama' },
    );
    if (!choice) {
        return;
    }

    let functionName = '';
    if (choice.diagram === 'asm') {
        const picked = await pickFunction(deps.client, uri, false);
        if (picked === undefined) {
            return;
        }
        functionName = picked;
    }

    const format = diagramFormat();
    const target = inspectTarget();
    const response = await request<TextResponse>(
        deps.client,
        'dibujando el diagrama',
        VestaMethod.Diagram,
        {
            uri,
            kind: choice.diagram,
            format,
            cost: diagramCost(),
            function: functionName,
            os: target.os,
            arch: target.arch,
        },
    );
    if (showErrorIfAny(response)) {
        return;
    }

    const text = response.text ?? '';
    if (format === 'html') {
        DiagramPanel.show(`Vesta: ${choice.label}`, text);
        return;
    }
    const extension = format === 'mermaid' ? 'mmd' : 'dot';
    await deps.views.show(
        `diagram-${choice.diagram}`,
        `${baseName(document)} (${choice.diagram}).${extension}`,
        text,
    );
}

/** @brief Abre la vista correlacionada del fuente, el IR y el ensamblador. */
async function showMachineView(deps: InspectContext): Promise<void> {
    const document = activeVestaDocument();
    if (!document) {
        return;
    }
    await MachineViewPanel.show(deps.client, document.uri);
}

/**
 * @brief Avisa si la respuesta trae un fallo del servidor.
 * @param response Respuesta a comprobar.
 * @return true si habia fallo (y ya se ha avisado).
 */
function showErrorIfAny(response: { error?: string }): boolean {
    if (response.error) {
        void vscode.window.showErrorMessage(`Vesta: ${response.error}`);
        return true;
    }
    return false;
}

/**
 * @brief Cabecera comun de los informes en texto.
 * @param document Documento al que se refiere el informe.
 * @param title    Titulo del informe.
 * @return Cabecera terminada en linea en blanco.
 */
function titleFor(document: vscode.TextDocument, title: string): string {
    const name = baseName(document);
    const rule = '='.repeat(Math.max(title.length, name.length) + 4);
    return `${rule}\n${title}\n${name}\n${rule}\n\n`;
}

/**
 * @brief Formatea una tabla con las columnas alineadas.
 * @param headers Titulos de las columnas.
 * @param rows    Filas; cada una con tantas celdas como titulos.
 * @return Texto de la tabla, terminado en salto de linea.
 */
function renderTable(headers: string[], rows: string[][]): string {
    const widths = headers.map((header, index) => {
        let width = header.length;
        for (const row of rows) {
            width = Math.max(width, (row[index] ?? '').length);
        }
        return width;
    });

    const pad = (cells: string[]): string =>
        cells
            .map((cell, index) => (cell ?? '').padEnd(widths[index]))
            .join('  ')
            .trimEnd();

    const lines = [pad(headers), widths.map(w => '-'.repeat(w)).join('  ')];
    for (const row of rows) {
        lines.push(pad(row));
    }
    return lines.join('\n') + '\n';
}
