/**
 * @file discovery.ts
 * @brief Localizacion de los ejecutables de VestaVM sin exigir configuracion.
 *
 * El servidor de lenguaje (`vesta_lsp`) y la maquina virtual (`vesta` una vez
 * instalada, `vm` dentro del arbol de compilacion) pueden estar en sitios muy
 * distintos: una instalacion del sistema, una carpeta del usuario o un
 * directorio de compilacion del repositorio.  Pedirle la ruta al usuario en
 * cada maquina seria trabajo suyo que la extension puede ahorrarse.
 *
 * El orden de busqueda es el mismo que usa la libreria cliente de Python
 * (`tools/vesta_lsp_client`), a proposito: si un sitio vale para una
 * herramienta del ecosistema, vale para todas, y quien mueva un binario no
 * tiene que acordarse de dos listas.
 */

import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';

/** Descripcion de que ejecutable se busca y donde. */
export interface BinaryQuery {
    /** Nombres candidatos, sin extension; en Windows se prueba tambien `.exe`. */
    names: string[];
    /** Variable de entorno que apunta directamente al binario. */
    envVar?: string;
    /** Subdirectorios a probar bajo cada raiz de instalacion. */
    subdirs?: string[];
    /** Ruta que el usuario ha configurado a mano; tiene prioridad sobre todo. */
    explicit?: string;
    /** Carpetas desde las que subir buscando directorios de compilacion. */
    searchFrom?: string[];
}

/** Resultado de una busqueda: la ruta y de donde ha salido. */
export interface BinaryLocation {
    /** Ruta absoluta al ejecutable. */
    path: string;
    /** Origen, para poder explicarlo en el registro y en los mensajes de error. */
    origin: 'configuracion' | 'entorno' | 'PATH' | 'instalacion' | 'compilacion';
}

/** @brief Indica si la ruta existe y es un fichero. */
function isFile(candidate: string): boolean {
    try {
        return fs.statSync(candidate).isFile();
    } catch {
        return false;
    }
}

/**
 * @brief Expande un nombre base a los nombres reales del sistema.
 * @param base Nombre sin extension.
 * @return El nombre tal cual y, en Windows, con cada extension ejecutable.
 */
function executableNames(base: string): string[] {
    if (process.platform !== 'win32') {
        return [base];
    }
    // Si ya trae extension, respetarla; si no, probar las del PATHEXT.
    if (path.extname(base) !== '') {
        return [base];
    }
    const exts = (process.env.PATHEXT ?? '.EXE;.CMD;.BAT;.COM')
        .split(';')
        .filter(e => e.length > 0);
    return [...exts.map(e => base + e.toLowerCase()), base];
}

/**
 * @brief Busca los nombres dados en el PATH del sistema.
 * @param names Nombres base a buscar.
 * @return La primera ruta que exista, o undefined.
 */
function searchInPath(names: string[]): string | undefined {
    const dirs = (process.env.PATH ?? '').split(path.delimiter).filter(d => d.length > 0);
    for (const dir of dirs) {
        for (const base of names) {
            for (const name of executableNames(base)) {
                const candidate = path.join(dir, name);
                if (isFile(candidate)) {
                    return candidate;
                }
            }
        }
    }
    return undefined;
}

/**
 * @brief Raices donde suele quedar instalado VestaVM, segun el sistema.
 *
 * El instalador coloca los binarios en `<raiz>/bin`; las variables de entorno
 * permiten mover la instalacion sin tocar la configuracion de la extension.
 *
 * @return Lista de raices candidatas, sin comprobar que existan.
 */
function installRoots(): string[] {
    const roots: string[] = [];
    for (const envName of ['VESTA_LSP_HOME', 'VESTA_HOME', 'VEX_HOME']) {
        const value = process.env[envName];
        if (value) {
            roots.push(value);
        }
    }
    if (process.platform === 'win32') {
        for (const envName of ['ProgramFiles', 'ProgramFiles(x86)', 'LOCALAPPDATA', 'APPDATA', 'ProgramData']) {
            const base = process.env[envName];
            if (base) {
                roots.push(path.join(base, 'VestaVM'));
            }
        }
    } else {
        const home = os.homedir();
        roots.push(
            '/usr/local/vesta',
            '/opt/vesta',
            '/usr/local',
            '/usr',
            path.join(home, '.local', 'share', 'vesta'),
            path.join(home, '.local'),
            path.join(home, 'vesta'),
        );
    }
    return roots;
}

/** Nombres de los directorios de compilacion que usa el repositorio. */
const BUILD_DIRS = [
    'cmake-build-release',
    'cmake-build-debug',
    'cmake-build-relwithdebinfo',
    'build',
    'cmake-build-windows',
    'out',
];

/**
 * @brief Directorios de compilacion visibles subiendo desde unas carpetas.
 *
 * Permite trabajar sobre un clon del repositorio sin instalar nada: los
 * binarios estan en `cmake-build-release/` a unos pocos niveles por encima
 * tanto de la carpeta de la extension como de la carpeta abierta en el editor.
 *
 * @param startDirs Carpetas desde las que empezar a subir.
 * @return Directorios candidatos, del mas cercano al mas lejano.
 */
function repoBuildDirs(startDirs: string[]): string[] {
    const out: string[] = [];
    const seen = new Set<string>();
    for (const start of startDirs) {
        let dir = start;
        // Ocho niveles cubren de sobra cualquier disposicion razonable del
        // repositorio y acotan el coste de la busqueda.
        for (let depth = 0; depth < 8; depth++) {
            for (const sub of BUILD_DIRS) {
                const candidate = path.join(dir, sub);
                if (!seen.has(candidate)) {
                    seen.add(candidate);
                    out.push(candidate);
                }
            }
            const parent = path.dirname(dir);
            if (parent === dir) {
                break;
            }
            dir = parent;
        }
    }
    return out;
}

/**
 * @brief Motor comun de busqueda de ejecutables.
 *
 * Orden: ruta configurada, variable de entorno, PATH, raices de instalacion y
 * directorios de compilacion.  El primero que exista gana.
 *
 * @param query Que se busca y desde donde.
 * @return La ubicacion encontrada, o undefined si no hay ninguna.
 */
export function findBinary(query: BinaryQuery): BinaryLocation | undefined {
    const explicit = query.explicit?.trim();
    if (explicit) {
        if (isFile(explicit)) {
            return { path: path.resolve(explicit), origin: 'configuracion' };
        }
        // Una ruta configurada que no existe puede ser un nombre suelto que si
        // este en el PATH; se intenta antes de darla por perdida.
        const viaPath = searchInPath([explicit]);
        if (viaPath) {
            return { path: viaPath, origin: 'configuracion' };
        }
    }

    if (query.envVar) {
        const fromEnv = process.env[query.envVar];
        if (fromEnv && isFile(fromEnv)) {
            return { path: path.resolve(fromEnv), origin: 'entorno' };
        }
    }

    const viaPath = searchInPath(query.names);
    if (viaPath) {
        return { path: viaPath, origin: 'PATH' };
    }

    const subdirs = query.subdirs ?? [path.join('lsp', 'bin'), 'bin', ''];
    for (const root of installRoots()) {
        for (const sub of subdirs) {
            for (const base of query.names) {
                for (const name of executableNames(base)) {
                    const candidate = path.join(root, sub, name);
                    if (isFile(candidate)) {
                        return { path: candidate, origin: 'instalacion' };
                    }
                }
            }
        }
    }

    for (const dir of repoBuildDirs(query.searchFrom ?? [])) {
        for (const base of query.names) {
            for (const name of executableNames(base)) {
                const candidate = path.join(dir, name);
                if (isFile(candidate)) {
                    return { path: candidate, origin: 'compilacion' };
                }
            }
        }
    }

    return undefined;
}

/**
 * @brief Localiza el servidor de lenguaje `vesta_lsp`.
 * @param explicit   Ruta del ajuste `vesta.server.path` (puede ir vacia).
 * @param searchFrom Carpetas desde las que buscar directorios de compilacion.
 * @return La ubicacion del servidor, o undefined.
 */
export function discoverLanguageServer(
    explicit: string | undefined,
    searchFrom: string[],
): BinaryLocation | undefined {
    return findBinary({
        names: ['vesta_lsp'],
        envVar: 'VESTA_LSP',
        subdirs: [path.join('lsp', 'bin'), 'bin', ''],
        explicit,
        searchFrom,
    });
}

/**
 * @brief Localiza la maquina virtual con la que se ejecutan los programas.
 *
 * El binario instalado se llama `vesta`; dentro del arbol de compilacion del
 * repositorio se llama `vm`.  Se prueban los dos, en ese orden.
 *
 * @param explicit   Ruta del ajuste `vesta.vmPath` (puede ir vacia).
 * @param searchFrom Carpetas desde las que buscar directorios de compilacion.
 * @return La ubicacion de la maquina virtual, o undefined.
 */
export function discoverVestaVm(
    explicit: string | undefined,
    searchFrom: string[],
): BinaryLocation | undefined {
    return findBinary({
        names: ['vesta', 'vm'],
        envVar: 'VESTA_VM',
        subdirs: ['bin', ''],
        explicit,
        searchFrom,
    });
}
