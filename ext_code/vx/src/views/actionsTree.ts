/**
 * @file actionsTree.ts
 * @brief Todo lo que la extension sabe hacer, a la vista y a un clic.
 *
 * Las acciones estaban solo en la paleta de comandos: para ver el IR habia que
 * saber que existe, acordarse del nombre y escribirlo.  Eso no es una
 * herramienta, es un examen.  Aqui viven agrupadas por lo que uno quiere hacer
 * -- mirar el codigo generado, ejecutar, navegar, ajustar -- en la barra
 * lateral, con el logo del lenguaje.
 *
 * Los grupos no son una taxonomia: son las tres o cuatro cosas por las que se
 * abre esto.  Lo que se usa a diario arriba; lo del servidor, al final.
 */

import * as vscode from 'vscode';

/** Una accion: lo que se ve y el comando que dispara. */
interface Accion {
    /** Texto de la fila. */
    readonly titulo: string;
    /** Comando que ejecuta. */
    readonly comando: string;
    /** Icono de la fila (nombre de codicon). */
    readonly icono: string;
    /** Explicacion al posar el cursor. */
    readonly ayuda: string;
}

/** Un grupo de acciones. */
interface Grupo {
    /** Titulo del grupo. */
    readonly titulo: string;
    /** Si nace abierto.  Lo de diario, si; lo demas, no. */
    readonly abierto: boolean;
    /** Sus acciones, en el orden en que se ofrecen. */
    readonly acciones: readonly Accion[];
}

/**
 * @brief Los grupos, en orden.
 *
 * El primero es el que contesta a "que hace el compilador con esto", que es
 * para lo que existe la extension; el ultimo, el que casi nunca se toca.
 *
 * Se construyen al pedir el arbol y no como una constante del modulo: el texto
 * sale del idioma del editor, y una constante lo fijaria en el que hubiera al
 * CARGAR la extension.
 *
 * @return Los grupos con su texto ya traducido.
 */
function groups(): readonly Grupo[] {
    return [
        {
            titulo: vscode.l10n.t('Generated code'),
            abierto: true,
            acciones: [
                {
                    titulo: vscode.l10n.t('Source / IR / assembly'),
                    comando: 'vesta.showMachineView',
                    icono: 'split-horizontal',
                    ayuda: vscode.l10n.t(
                        'The three views lined up by line: where each instruction comes from'),
                },
                {
                    titulo: vscode.l10n.t('SSA IR'),
                    comando: 'vesta.showIr',
                    icono: 'symbol-structure',
                    ayuda: vscode.l10n.t(
                        'The intermediate representation, before or after optimising'),
                },
                {
                    titulo: vscode.l10n.t('What the optimiser did'),
                    comando: 'vesta.showIrDiff',
                    icono: 'diff',
                    ayuda: vscode.l10n.t('The same IR before and after, as a diff'),
                },
                {
                    titulo: vscode.l10n.t('.vel bytecode'),
                    comando: 'vesta.showBytecode',
                    icono: 'file-binary',
                    ayuda: vscode.l10n.t("The virtual machine's assembly"),
                },
                {
                    titulo: vscode.l10n.t('JIT assembly'),
                    comando: 'vesta.showJitAsm',
                    icono: 'zap',
                    ayuda: vscode.l10n.t('The machine code the JIT generates'),
                },
                {
                    titulo: vscode.l10n.t('AOT assembly'),
                    comando: 'vesta.showAotAsm',
                    icono: 'server-process',
                    ayuda: vscode.l10n.t("The native binary's machine code"),
                },
                {
                    titulo: vscode.l10n.t('Assembly block with its flow'),
                    comando: 'vesta.showAsmBlock',
                    icono: 'references',
                    ayuda: vscode.l10n.t(
                        'The asm block under the cursor, with the arrows of its jumps and what is known about each instruction'),
                },
                {
                    titulo: vscode.l10n.t('Diagram'),
                    comando: 'vesta.showDiagram',
                    icono: 'type-hierarchy',
                    ayuda: vscode.l10n.t('The AST, the IR or the flow, drawn'),
                },
            ],
        },
        {
            titulo: vscode.l10n.t('What the compiler knows'),
            abierto: true,
            acciones: [
                {
                    titulo: vscode.l10n.t('Everything it knows about the module'),
                    comando: 'vesta.showAsa',
                    icono: 'lightbulb',
                    ayuda: vscode.l10n.t(
                        'The facts the analysis deduced, just as the command line reports them'),
                },
                {
                    titulo: vscode.l10n.t('Cost and contracts per function'),
                    comando: 'vesta.showComplexity',
                    icono: 'graph',
                    ayuda: vscode.l10n.t(
                        'What each function declares -- cost, allocations, stack, purity -- against what the compiler measures'),
                },
                {
                    titulo: vscode.l10n.t('AOT compatibility'),
                    comando: 'vesta.showAotCompat',
                    icono: 'checklist',
                    ayuda: vscode.l10n.t(
                        'What stops each function from compiling to native, if anything does'),
                },
                {
                    titulo: vscode.l10n.t('The three execution modes'),
                    comando: 'vesta.showModes',
                    icono: 'list-tree',
                    ayuda: vscode.l10n.t(
                        'Interpreter, JIT and native: what each one can do with this module'),
                },
                {
                    titulo: vscode.l10n.t('Macro expansion'),
                    comando: 'vesta.showMacroExpand',
                    icono: 'symbol-snippet',
                    ayuda: vscode.l10n.t(
                        'The source after expanding what is generated at compile time'),
                },
                {
                    titulo: vscode.l10n.t('Comptime values'),
                    comando: 'vesta.showComptimeValues',
                    icono: 'symbol-constant',
                    ayuda: vscode.l10n.t(
                        'What was resolved during compilation, with its value'),
                },
            ],
        },
        {
            titulo: vscode.l10n.t('Run'),
            abierto: true,
            acciones: [
                {
                    titulo: vscode.l10n.t('Run the selection'),
                    comando: 'vesta.runSelection',
                    icono: 'run-below',
                    ayuda: vscode.l10n.t('Compiles and runs only what is selected'),
                },
                {
                    titulo: vscode.l10n.t('Compile and run the file'),
                    comando: 'vesta.run',
                    icono: 'play',
                    ayuda: vscode.l10n.t(
                        'The whole file, with the chosen mode and level'),
                },
                {
                    titulo: vscode.l10n.t('Compile the file'),
                    comando: 'vesta.compile',
                    icono: 'tools',
                    ayuda: vscode.l10n.t('Compiles without running'),
                },
                {
                    titulo: vscode.l10n.t('How it runs'),
                    comando: 'vesta.selectRunOptions',
                    icono: 'settings',
                    ayuda: vscode.l10n.t(
                        'Execution mode, optimisation level and debugging'),
                },
            ],
        },
        {
            titulo: vscode.l10n.t('For which machine'),
            abierto: true,
            acciones: [
                {
                    titulo: vscode.l10n.t('Choose the target'),
                    comando: 'vesta.selectTarget',
                    icono: 'device-desktop',
                    ayuda: vscode.l10n.t(
                        'System, architecture, optimisation level and microarchitecture: they decide the views AND the errors'),
                },
            ],
        },
        {
            titulo: vscode.l10n.t('Navigate'),
            abierto: false,
            acciones: [
                {
                    titulo: vscode.l10n.t('Open a library module'),
                    comando: 'vesta.openStdlib',
                    icono: 'library',
                    ayuda: vscode.l10n.t(
                        'Search for and open any module of the standard library'),
                },
                {
                    titulo: vscode.l10n.t('Open the import under the cursor'),
                    comando: 'vesta.openImport',
                    icono: 'go-to-file',
                    ayuda: vscode.l10n.t(
                        'Jumps to the file of the module being imported'),
                },
            ],
        },
        {
            titulo: vscode.l10n.t('Server'),
            abierto: false,
            acciones: [
                {
                    titulo: vscode.l10n.t('Paths in use'),
                    comando: 'vesta.showPaths',
                    icono: 'folder-opened',
                    ayuda: vscode.l10n.t(
                        'Which server and which library are in use, and where they came from'),
                },
                {
                    titulo: vscode.l10n.t('Server log'),
                    comando: 'vesta.showServerLog',
                    icono: 'output',
                    ayuda: vscode.l10n.t('What the language server keeps reporting'),
                },
                {
                    titulo: vscode.l10n.t('Restart the server'),
                    comando: 'vesta.restartServer',
                    icono: 'debug-restart',
                    ayuda: vscode.l10n.t(
                        'Starts it again with the current configuration'),
                },
                {
                    titulo: vscode.l10n.t('Stop the server'),
                    comando: 'vesta.stopServer',
                    icono: 'debug-stop',
                    ayuda: vscode.l10n.t(
                        'Leaves it stopped and frees its binary, so it can be rebuilt'),
                },
            ],
        },
    ];
}

/** Nodo del arbol: un grupo o una accion. */
type Nodo = { clase: 'grupo'; grupo: Grupo } | { clase: 'accion'; accion: Accion };

/**
 * @class VestaActionsProvider
 * @brief Da al editor el arbol de acciones de la barra lateral.
 */
export class VestaActionsProvider implements vscode.TreeDataProvider<Nodo> {
    /**
     * @brief Convierte un nodo en su fila.
     * @param nodo Nodo a mostrar.
     * @return La fila ya montada.
     */
    public getTreeItem(nodo: Nodo): vscode.TreeItem {
        if (nodo.clase === 'grupo') {
            const item = new vscode.TreeItem(
                nodo.grupo.titulo,
                nodo.grupo.abierto
                    ? vscode.TreeItemCollapsibleState.Expanded
                    : vscode.TreeItemCollapsibleState.Collapsed,
            );
            item.contextValue = 'vestaGrupo';
            return item;
        }
        const item = new vscode.TreeItem(
            nodo.accion.titulo,
            vscode.TreeItemCollapsibleState.None,
        );
        item.iconPath = new vscode.ThemeIcon(nodo.accion.icono);
        item.tooltip = nodo.accion.ayuda;
        item.command = {
            command: nodo.accion.comando,
            title: nodo.accion.titulo,
        };
        item.contextValue = 'vestaAccion';
        return item;
    }

    /**
     * @brief Los hijos de un nodo, o los grupos si no se pide ninguno.
     * @param nodo Nodo padre, o undefined para la raiz.
     * @return Los hijos.
     */
    public getChildren(nodo?: Nodo): Nodo[] {
        if (!nodo) {
            return groups().map(grupo => ({ clase: 'grupo', grupo } as Nodo));
        }
        if (nodo.clase === 'grupo') {
            return nodo.grupo.acciones.map(
                accion => ({ clase: 'accion', accion } as Nodo),
            );
        }
        return [];
    }
}
