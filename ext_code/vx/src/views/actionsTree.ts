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
 * Los grupos, en orden.
 *
 * El primero es el que contesta a "que hace el compilador con esto", que es
 * para lo que existe la extension; el ultimo, el que casi nunca se toca.
 */
const GRUPOS: readonly Grupo[] = [
    {
        titulo: 'Codigo generado',
        abierto: true,
        acciones: [
            {
                titulo: 'Fuente / IR / ensamblador',
                comando: 'vesta.showMachineView',
                icono: 'split-horizontal',
                ayuda: 'Las tres vistas alineadas por linea: de donde sale cada instruccion',
            },
            {
                titulo: 'IR SSA',
                comando: 'vesta.showIr',
                icono: 'symbol-structure',
                ayuda: 'La representacion intermedia, antes o despues de optimizar',
            },
            {
                titulo: 'Que hizo el optimizador',
                comando: 'vesta.showIrDiff',
                icono: 'diff',
                ayuda: 'El mismo IR antes y despues, en un diff',
            },
            {
                titulo: 'Bytecode .vel',
                comando: 'vesta.showBytecode',
                icono: 'file-binary',
                ayuda: 'El ensamblador de la maquina virtual',
            },
            {
                titulo: 'Ensamblador del JIT',
                comando: 'vesta.showJitAsm',
                icono: 'zap',
                ayuda: 'El codigo maquina que genera el JIT',
            },
            {
                titulo: 'Ensamblador AOT',
                comando: 'vesta.showAotAsm',
                icono: 'server-process',
                ayuda: 'El codigo maquina del binario nativo',
            },
            {
                titulo: 'Bloque de ensamblador con su flujo',
                comando: 'vesta.showAsmBlock',
                icono: 'references',
                ayuda: 'El bloque asm bajo el cursor, con las flechas de sus saltos y lo que se sabe de cada instruccion',
            },
            {
                titulo: 'Diagrama',
                comando: 'vesta.showDiagram',
                icono: 'type-hierarchy',
                ayuda: 'El AST, el IR o el flujo, dibujados',
            },
        ],
    },
    {
        titulo: 'Lo que el compilador sabe',
        abierto: true,
        acciones: [
            {
                titulo: 'Todo lo que sabe del modulo',
                comando: 'vesta.showAsa',
                icono: 'lightbulb',
                ayuda: 'Los hechos que el analisis dedujo, tal y como los cuenta la linea de ordenes',
            },
            {
                titulo: 'Coste y contratos por funcion',
                comando: 'vesta.showComplexity',
                icono: 'graph',
                ayuda: 'Lo que cada funcion declara -- coste, reservas, pila, pureza -- frente a lo que el compilador mide',
            },
            {
                titulo: 'Compatibilidad AOT',
                comando: 'vesta.showAotCompat',
                icono: 'checklist',
                ayuda: 'Que impide compilar cada funcion a nativo, si algo lo impide',
            },
            {
                titulo: 'Los tres modos de ejecucion',
                comando: 'vesta.showModes',
                icono: 'list-tree',
                ayuda: 'Interprete, JIT y nativo: que puede cada uno con este modulo',
            },
            {
                titulo: 'Expansion de las macros',
                comando: 'vesta.showMacroExpand',
                icono: 'symbol-snippet',
                ayuda: 'El fuente despues de expandir lo que se genera al compilar',
            },
            {
                titulo: 'Valores comptime',
                comando: 'vesta.showComptimeValues',
                icono: 'symbol-constant',
                ayuda: 'Lo que se resolvio durante la compilacion, con su valor',
            },
        ],
    },
    {
        titulo: 'Ejecutar',
        abierto: true,
        acciones: [
            {
                titulo: 'Ejecutar lo seleccionado',
                comando: 'vesta.runSelection',
                icono: 'run-below',
                ayuda: 'Compila y ejecuta solo lo que hay seleccionado',
            },
            {
                titulo: 'Compilar y ejecutar el fichero',
                comando: 'vesta.run',
                icono: 'play',
                ayuda: 'El fichero entero, con el modo y el nivel elegidos',
            },
            {
                titulo: 'Compilar el fichero',
                comando: 'vesta.compile',
                icono: 'tools',
                ayuda: 'Compila sin ejecutar',
            },
            {
                titulo: 'Como se ejecuta',
                comando: 'vesta.selectRunOptions',
                icono: 'settings',
                ayuda: 'Modo de ejecucion, nivel de optimizacion y depuracion',
            },
        ],
    },
    {
        titulo: 'Para que maquina',
        abierto: true,
        acciones: [
            {
                titulo: 'Elegir objetivo',
                comando: 'vesta.selectTarget',
                icono: 'device-desktop',
                ayuda: 'Sistema, arquitectura, nivel de optimizacion y microarquitectura: deciden las vistas Y los errores',
            },
        ],
    },
    {
        titulo: 'Navegar',
        abierto: false,
        acciones: [
            {
                titulo: 'Abrir un modulo de la biblioteca',
                comando: 'vesta.openStdlib',
                icono: 'library',
                ayuda: 'Buscar y abrir cualquier modulo de la biblioteca estandar',
            },
            {
                titulo: 'Abrir el import bajo el cursor',
                comando: 'vesta.openImport',
                icono: 'go-to-file',
                ayuda: 'Salta al fichero del modulo que se esta importando',
            },
        ],
    },
    {
        titulo: 'Servidor',
        abierto: false,
        acciones: [
            {
                titulo: 'Rutas en uso',
                comando: 'vesta.showPaths',
                icono: 'folder-opened',
                ayuda: 'Que servidor y que biblioteca se estan usando, y de donde salieron',
            },
            {
                titulo: 'Registro del servidor',
                comando: 'vesta.showServerLog',
                icono: 'output',
                ayuda: 'Lo que el servidor de lenguaje va contando',
            },
            {
                titulo: 'Reiniciar el servidor',
                comando: 'vesta.restartServer',
                icono: 'debug-restart',
                ayuda: 'Vuelve a arrancarlo con la configuracion actual',
            },
        ],
    },
];

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
            return GRUPOS.map(grupo => ({ clase: 'grupo', grupo } as Nodo));
        }
        if (nodo.clase === 'grupo') {
            return nodo.grupo.acciones.map(
                accion => ({ clase: 'accion', accion } as Nodo),
            );
        }
        return [];
    }
}
