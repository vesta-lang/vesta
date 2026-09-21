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
    ComptimeValuesResponse,
    DiagramKind,
    FunctionEntry,
    FunctionsResponse,
    IrDiffResponse,
    IrPhase,
    MacroExpandResponse,
    ModesResponse,
    TextResponse,
    VestaMethod,
} from '../lsp/protocol';
import { AsaPanel } from '../views/asaPanel';
import { AsmBlockPanel } from '../views/asmBlockPanel';
import { ReportPanel } from '../views/reportPanel';
import { DiagramPanel } from '../views/diagramPanel';
import { MachineViewPanel } from '../views/machinePanel';
import { VestaTextViewProvider } from '../views/textViews';
import {
    activeVestaDocument,
    aotTier,
    diagramCost,
    diagramFormat,
    inspectTarget,
    applyTarget,
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
                    void vscode.window.showErrorMessage(
                        vscode.l10n.t('Vesta: {0}', describeError(err)));
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
    register('vesta.showAsmBlock', () => showAsmBlock(deps));
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
        { location: vscode.ProgressLocation.Window, title: vscode.l10n.t('Vesta: {0}', title) },
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
    let entradas: FunctionEntry[] = [];
    try {
        const response = await client.request<FunctionsResponse>(VestaMethod.Functions, { uri });
        entradas = response.functions ?? [];
    } catch {
        // Sin lista de funciones se sigue adelante: el servidor elige por su
        // cuenta la primera compilable.
        return '';
    }

    if (entradas.length === 0) {
        return '';
    }

    interface Item extends vscode.QuickPickItem {
        value: string;
    }
    const items: Item[] = [];
    if (allowModule) {
        items.push({
            label: vscode.l10n.t('The whole module'),
            detail: vscode.l10n.t('Without singling out any function'),
            value: '',
        });
    }
    for (const fn of entradas) {
        // Se ensena el nombre ESCRITO y se manda el interno: uno es para leer,
        // el otro es el que el servidor entiende.
        items.push({ label: fn.display || fn.name, value: fn.name });
    }

    const choice = await vscode.window.showQuickPick(items, {
        placeHolder: vscode.l10n.t('Choose the function'),
    });
    return choice?.value;
}

/** @brief Nombre corto del fichero activo, para titular las vistas. */
function baseName(document: vscode.TextDocument): string {
    return path.basename(document.uri.fsPath);
}

/**
 * @brief Como se nombra en los informes lo que no es una funcion concreta.
 * @return El texto del modulo entero, en el idioma del editor.
 */
function wholeModuleLabel(): string {
    return vscode.l10n.t('module');
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
                label: vscode.l10n.t('After optimising'),
                detail: vscode.l10n.t('The IR that reaches the code generator'),
                phase: 'post',
            },
            {
                label: vscode.l10n.t('Before optimising'),
                detail: vscode.l10n.t('The IR just as lowering leaves it'),
                phase: 'pre',
            },
        ],
        { placeHolder: vscode.l10n.t('Which phase of the IR') },
    );
    if (!choice) {
        return;
    }

    // Con que se compila forma parte de la pregunta: el mismo fuente da otro
    // IR a otro nivel de optimizacion o para otra maquina.
    const params: Record<string, unknown> = {
        uri: document.uri.toString(),
        phase: choice.phase,
    };
    applyTarget(params, inspectTarget());
    const response = await request<TextResponse>(
        deps.client,
        vscode.l10n.t('generating the IR'),
        VestaMethod.Ir,
        params,
    );
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

    const response = await request<IrDiffResponse>(
        deps.client,
        vscode.l10n.t('comparing the IR'),
        VestaMethod.IrDiff,
        { uri, function: functionName },
    );
    if (showErrorIfAny(response)) {
        return;
    }

    /* El servidor devuelve FILAS, no un texto: cada una dice si la linea sigue
     * igual, si desaparecio, si es nueva o si cambio, con las dos versiones
     * alineadas.  Se pedia `text`, que no existe en esa respuesta, asi que la
     * vista salia vacia sin un solo error -- ni del servidor, que contesto
     * bien, ni de aqui, que leyo un campo ausente y siguio --.
     *
     * Se compone en unificado (+/-) porque es lo que el editor sabe colorear
     * como un diff. */
    const rows = response.rows ?? [];
    const label = functionName || wholeModuleLabel();
    if (rows.length === 0) {
        void vscode.window.showInformationMessage(
            vscode.l10n.t('Vesta: there is no IR to compare in {0}.', label),
        );
        return;
    }

    const lineas: string[] = [];
    let cambios = 0;
    for (const row of rows) {
        switch (row.k) {
            case 'del':
                lineas.push('-' + row.l);
                cambios++;
                break;
            case 'add':
                lineas.push('+' + row.r);
                cambios++;
                break;
            case 'chg':
                lineas.push('-' + row.l);
                lineas.push('+' + row.r);
                cambios++;
                break;
            default:
                lineas.push(' ' + row.l);
                break;
        }
    }

    // Decirlo arriba: sin esto, un diff sin cambios se lee como un fallo.
    const cabecera = cambios === 0
        ? '# ' + vscode.l10n.t('The optimiser did not touch {0}.', label) + '\n'
        : '# ' + vscode.l10n.t('The optimiser changed {0} of the {1} lines of {2}.',
                               cambios, rows.length, label) + '\n' +
          '# ' + vscode.l10n.t('-- before optimising    ++ after') + '\n';

    await deps.views.show(
        'ir-diff',
        `${baseName(document)} (${label}).diff`,
        cabecera + lineas.join('\n') + '\n',
        'diff',
    );
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

    const params: Record<string, unknown> = { uri, function: functionName };
    applyTarget(params, inspectTarget());
    const response = await request<TextResponse>(
        deps.client,
        vscode.l10n.t('generating the bytecode'),
        VestaMethod.Bytecode,
        params,
    );
    if (!showErrorIfAny(response)) {
        const label = functionName || wholeModuleLabel();
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
        vscode.l10n.t('compiling ({0})', backend.toUpperCase()),
        method,
        asmParams(uri, functionName, target),
    );

    if (showErrorIfAny(response)) {
        return;
    }
    if (response.unsupported || response.incompatible) {
        /* El motivo lo da el servidor, que lo saca del catalogo del compilador;
         * el de aqui solo cubre el caso en que no diga ninguno. */
        void vscode.window.showWarningMessage(
            vscode.l10n.t(
                'Vesta: {0}',
                response.reason
                    ?? vscode.l10n.t('the function cannot be compiled in this mode.')),
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
 * @brief Parametros de una peticion de desensamblado.
 * @param uri          Documento.
 * @param functionName Funcion pedida (vacio = la que elija el servidor).
 * @param target       Con que se compila.
 * @return Los parametros listos para la peticion.
 */
function asmParams(
    uri: string,
    functionName: string,
    target: ReturnType<typeof inspectTarget>,
): Record<string, unknown> {
    const params: Record<string, unknown> = {
        uri,
        function: functionName,
        tier: aotTier(),
    };
    applyTarget(params, target);
    return params;
}

/**
 * @brief Compone la cabecera informativa del desensamblado.
 * @param response Respuesta del servidor.
 * @param backend  Generador de codigo usado.
 * @return Texto de cabecera, terminado en linea en blanco.
 */
function buildAsmHeader(response: AsmResponse, backend: string): string {
    /* Las filas se juntan con su texto ya traducido y se alinean DESPUES: un
     * relleno escrito a mano cuadra en un idioma y se tuerce en el siguiente,
     * y la cabecera de un desensamblado se lee en columna. */
    const rows: Array<[string, string]> = [];
    rows.push([vscode.l10n.t('function'), response.function ?? '']);
    rows.push([vscode.l10n.t('backend'), backend.toUpperCase()]);
    if (response.bytes !== undefined) {
        rows.push([vscode.l10n.t('size'), vscode.l10n.t('{0} bytes', response.bytes)]);
    }
    if (response.instructions !== undefined) {
        rows.push([vscode.l10n.t('instructions'), String(response.instructions)]);
    }
    for (const arg of response.args ?? []) {
        rows.push([vscode.l10n.t('argument'), `${arg.reg} = ${arg.name}`]);
    }
    for (const reloc of response.relocs ?? []) {
        rows.push([vscode.l10n.t('relocation'),
                   `+${reloc.offset} ${reloc.kind} ${reloc.symbol}`]);
    }

    const width = rows.reduce((max, [label]) => Math.max(max, label.length), 0);
    const lines = rows.map(([label, value]) => `; ${label.padEnd(width)} : ${value}`);
    lines.push('');
    return lines.join('\n');
}

/**
 * @brief Abre el informe por funcion: lo que declara frente a lo que hace.
 *
 * Era una tabla de texto en un documento aparte -- un volcado, que no se puede
 * filtrar, ni ordenar, ni pulsar --.  Ahora es un panel donde cada fila es una
 * funcion: lo que cuesta, lo que reserva, cuanta pila usa, que hace, que
 * declaro y si lo cumple, y si el modo nativo puede con ella.  Lo que no cuadra
 * se ve sin tener que compararlo de cabeza, que es justo para lo que existe un
 * contrato.
 *
 * @param deps Contexto de los comandos.
 */
async function showComplexity(deps: InspectContext): Promise<void> {
    const document = activeVestaDocument();
    if (!document) {
        return;
    }
    await ReportPanel.show(deps.client, document.uri);
}

/** @brief Abre el informe de los tres modos de ejecucion. */
async function showModes(deps: InspectContext): Promise<void> {
    const document = activeVestaDocument();
    if (!document) {
        return;
    }

    const response = await request<ModesResponse>(
        deps.client,
        vscode.l10n.t('analysing the modes'),
        VestaMethod.Modes,
        { uri: document.uri.toString(), tier: aotTier() },
    );
    if (showErrorIfAny(response)) {
        return;
    }

    const yes = vscode.l10n.t('yes');
    const no = vscode.l10n.t('no');
    const parts: string[] = [
        titleFor(document, vscode.l10n.t('The three execution modes')),
    ];
    for (const mode of response.modes ?? []) {
        // Se recogen las filas del modo y se alinean juntas, por lo mismo que
        // la cabecera del desensamblado: el ancho lo fija el idioma.
        const rows: Array<[string, string]> = [];
        if (mode.ok !== undefined) {
            rows.push([vscode.l10n.t('compiles'), mode.ok ? yes : no]);
        }
        if (mode.errors !== undefined) {
            rows.push([vscode.l10n.t('errors'), String(mode.errors)]);
        }
        if (mode.warnings !== undefined) {
            rows.push([vscode.l10n.t('warnings'), String(mode.warnings)]);
        }
        if (mode.tier) {
            rows.push([vscode.l10n.t('tier'), mode.tier]);
        }
        if (mode.compatible !== undefined) {
            rows.push([vscode.l10n.t('compatible'), mode.compatible ? yes : no]);
        }
        if (mode.compilable_functions?.length) {
            rows.push([vscode.l10n.t('compiled'), mode.compilable_functions.join(', ')]);
        }
        if (mode.fallback_functions?.length) {
            rows.push([vscode.l10n.t('to the interpreter'),
                       mode.fallback_functions.join(', ')]);
        }
        if (mode.note) {
            rows.push([vscode.l10n.t('note'), mode.note]);
        }

        parts.push(`[${mode.mode}]`);
        const width = rows.reduce((max, [label]) => Math.max(max, label.length), 0);
        for (const [label, value] of rows) {
            parts.push(`  ${label.padEnd(width)} : ${value}`);
        }
        for (const issue of mode.issues ?? []) {
            parts.push('  - ' + vscode.l10n.t(
                '{0} (line {1}): {2}',
                issue.fn_display || issue.fn_name, issue.source_line, issue.reason));
        }
        parts.push('');
    }

    await deps.views.show('modes', `${baseName(document)} (modes).txt`, parts.join('\n'));
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
        vscode.l10n.t('checking compatibility'),
        VestaMethod.AotCompat,
        { uri: document.uri.toString(), tier },
    );
    if (showErrorIfAny(response)) {
        return;
    }

    const parts: string[] = [
        titleFor(document, vscode.l10n.t('AOT compatibility (tier {0})', tier)),
    ];
    parts.push(response.compatible
        ? vscode.l10n.t('Compatible: yes')
        : vscode.l10n.t('Compatible: no'));
    parts.push('');

    const issues = response.issues ?? [];
    if (issues.length > 0) {
        parts.push(vscode.l10n.t('What stands in the way:'));
        parts.push(
            renderTable(
                [vscode.l10n.t('function'), vscode.l10n.t('line'),
                 vscode.l10n.t('operation'), vscode.l10n.t('reason')],
                issues.map(i => [i.fn_display || i.fn_name,
                                 String(i.source_line), i.op, i.reason]),
            ),
        );
    }

    const okFunctions = response.ok_functions ?? [];
    if (okFunctions.length > 0) {
        parts.push(vscode.l10n.t('Functions with no problems ({0}):', okFunctions.length));
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
        vscode.l10n.t('expanding the macros'),
        VestaMethod.MacroExpand,
        { uri: document.uri.toString() },
    );
    if (showErrorIfAny(response)) {
        return;
    }

    const expansions = response.expansions ?? [];
    const skipped = response.skipped ?? [];
    if (expansions.length === 0 && skipped.length === 0) {
        void vscode.window.showInformationMessage(
            vscode.l10n.t('Vesta: the module does not use macros.'));
        return;
    }

    const parts: string[] = [
        titleFor(document, vscode.l10n.t('Code generated by the macros')),
    ];
    for (const expansion of expansions) {
        parts.push(`// ${expansion.macro_name}(${expansion.args.join(', ')})`);
        parts.push('// ' + vscode.l10n.t('at {0}', expansion.call_site_loc));
        parts.push(expansion.generated_code);
        parts.push('');
    }
    if (skipped.length > 0) {
        parts.push('// ' + vscode.l10n.t('Macros that could not be expanded:'));
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
        vscode.l10n.t('evaluating the compile-time code'),
        VestaMethod.ComptimeValues,
        { uri: document.uri.toString() },
    );
    if (showErrorIfAny(response)) {
        return;
    }

    const values = response.values ?? [];
    if (values.length === 0) {
        void vscode.window.showInformationMessage(
            vscode.l10n.t('Vesta: the module has no values resolved at compile time.'),
        );
        return;
    }

    const text =
        titleFor(document, vscode.l10n.t('Values resolved at compile time')) +
        renderTable(
            [vscode.l10n.t('name'), vscode.l10n.t('scope'),
             vscode.l10n.t('type'), vscode.l10n.t('value')],
            values.map(v => [v.name, v.scope, v.type_kind, v.value_str]),
        );

    await deps.views.show('comptime', `${baseName(document)} (comptime).txt`, text);
}

/**
 * @brief Abre todo lo que el compilador sabe del modulo.
 *
 * El analisis produce miles de hechos -- a donde apunta cada valor, entre que
 * limites se mueve, a que esta alineado, si escapa, que efectos tiene --, y
 * eso como volcado de texto es como no tenerlo: nadie lee miles de lineas
 * buscando una cosa.  Se abre en un panel donde se filtra por funcion, por
 * analisis y por certeza, se busca por texto, y cada fila lleva al codigo del
 * que habla.
 *
 * Quien quiera el volcado literal -- el mismo que da la linea de ordenes --
 * lo tiene ahi, con `vm --analyze`.
 *
 * @param deps Contexto de los comandos.
 */
async function showAsa(deps: InspectContext): Promise<void> {
    const document = activeVestaDocument();
    if (!document) {
        return;
    }
    await AsaPanel.show(deps.client, document.uri);
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
            {
                label: vscode.l10n.t('Optimised IR'),
                detail: vscode.l10n.t('Block graph after optimising'),
                diagram: 'ir-post',
            },
            {
                label: vscode.l10n.t('Unoptimised IR'),
                detail: vscode.l10n.t('Block graph straight out of lowering'),
                diagram: 'ir-pre',
            },
            {
                label: vscode.l10n.t('Syntax tree'),
                detail: vscode.l10n.t('Structure of the source'),
                diagram: 'ast',
            },
            {
                label: vscode.l10n.t('Bytecode'),
                detail: vscode.l10n.t('Flow of the virtual machine code'),
                diagram: 'vel',
            },
            {
                label: vscode.l10n.t('Machine code'),
                detail: vscode.l10n.t("Graph of a function's native code"),
                diagram: 'asm',
            },
        ],
        { placeHolder: vscode.l10n.t('Which diagram') },
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
        vscode.l10n.t('drawing the diagram'),
        VestaMethod.Diagram,
        diagramParams(uri, choice.diagram, format, functionName, target),
    );
    if (showErrorIfAny(response)) {
        return;
    }

    const text = response.text ?? '';
    if (format === 'html') {
        DiagramPanel.show(vscode.l10n.t('Vesta: {0}', choice.label), text);
        return;
    }
    const extension = format === 'mermaid' ? 'mmd' : 'dot';
    await deps.views.show(
        `diagram-${choice.diagram}`,
        `${baseName(document)} (${choice.diagram}).${extension}`,
        text,
    );
}

/**
 * @brief Parametros de una peticion de diagrama.
 * @param uri          Documento.
 * @param kind         Que se diagrama.
 * @param format       En que formato.
 * @param functionName Funcion, solo para el diagrama del codigo maquina.
 * @param target       Con que se compila.
 * @return Los parametros listos para la peticion.
 */
function diagramParams(
    uri: string,
    kind: DiagramKind,
    format: string,
    functionName: string,
    target: ReturnType<typeof inspectTarget>,
): Record<string, unknown> {
    const params: Record<string, unknown> = {
        uri,
        kind,
        format,
        cost: diagramCost(),
        function: functionName,
    };
    applyTarget(params, target);
    return params;
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
        void vscode.window.showErrorMessage(vscode.l10n.t('Vesta: {0}', response.error));
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
 *
 * Las celdas llegan como lo que el servidor puso en su JSON, que no siempre es
 * texto: un numero, un booleano o un ausente.  Se convierten aqui.  Antes se
 * exigia texto y bastaba con que un campo llegara como numero para que la
 * vista entera reventara con "padEnd is not a function" -- y el ancho de la
 * columna se calculaba mal MUCHO antes de eso, sin avisar.
 *
 * @param headers Titulos de las columnas.
 * @param rows    Filas; cada una con tantas celdas como titulos.
 * @return Texto de la tabla, terminado en salto de linea.
 */
function renderTable(headers: string[], rows: unknown[][]): string {
    /** Lo que sea, como texto.  Ausente y nulo son la celda vacia. */
    const texto = (celda: unknown): string =>
        celda === undefined || celda === null ? '' : String(celda);

    const widths = headers.map((header, index) => {
        let width = header.length;
        for (const row of rows) {
            width = Math.max(width, texto(row[index]).length);
        }
        return width;
    });

    const pad = (cells: unknown[]): string =>
        cells
            .map((cell, index) => texto(cell).padEnd(widths[index]))
            .join('  ')
            .trimEnd();

    const lines = [pad(headers), widths.map(w => '-'.repeat(w)).join('  ')];
    for (const row of rows) {
        lines.push(pad(row));
    }
    return lines.join('\n') + '\n';
}

/**
 * @brief Abre el bloque de ensamblador donde esta el cursor, con su flujo.
 *
 * Se pide por la LINEA del cursor y el servidor localiza el bloque: quien lo
 * sabe es quien lo analiza, y hacerlo aqui seria una segunda idea de donde
 * empieza y acaba un bloque.
 *
 * @param deps Contexto de los comandos.
 */
async function showAsmBlock(deps: InspectContext): Promise<void> {
    const document = activeVestaDocument();
    if (!document) {
        return;
    }
    // La linea del cursor si lo hay; si no, la primera del fichero, y que el
    // servidor conteste que ahi no hay ningun bloque.
    const editor = vscode.window.visibleTextEditors.find(
        e => e.document.uri.toString() === document.uri.toString(),
    );
    const linea = (editor ? editor.selection.active.line : 0) + 1;
    await AsmBlockPanel.show(deps.client, document.uri, linea);
}
