/**
 * @file instructionHover.ts
 * @brief Lo que cuesta una instruccion, al posar el cursor sobre ella.
 *
 * Dentro de un bloque `asm` el editor no tenia nada que decir: se veia el
 * mnemonico y poco mas.  Pero el compilador lleva una base con las
 * instrucciones cronometradas por microarquitectura -- es la misma que consulta
 * su planificador para decidir si puede mover una instruccion respecto de otra
 * --, y esa informacion sirve igual a quien escribe el ensamblador a mano.
 *
 * Se ensena lo que de verdad decide: cuanto tarda, cada cuanto se puede repetir,
 * por que puertos pasa, y que registros, banderas y estado del procesador toca.
 * Y SIEMPRE con la microarquitectura delante: una latencia sin decir de que
 * maquina no significa nada.
 */

import * as vscode from 'vscode';

import { VestaLanguageClient } from '../lsp/client';
import { InstructionResponse, VestaMethod } from '../lsp/protocol';
import { inspectTarget } from '../util/settings';

/** Abre un bloque de ensamblador, en cualquiera de sus formas. */
const ABRE_ASM = /\basm\b[^{;]*\{|\bbytes\s+[A-Za-z_][A-Za-z0-9_]*\s*\{/;

/**
 * @class InstructionHoverProvider
 * @brief Responde con la ficha de la instruccion bajo el cursor.
 */
export class InstructionHoverProvider implements vscode.HoverProvider {
    /**
     * @brief Construye el proveedor sobre un cliente ya creado.
     * @param client Cliente del servidor de lenguaje.
     */
    constructor(private readonly client: VestaLanguageClient) {}

    /**
     * @brief Devuelve la ficha si el cursor esta sobre una instruccion.
     * @param document Documento en curso.
     * @param position Donde esta el cursor.
     * @return La ficha, o undefined si ahi no hay ensamblador.
     */
    public async provideHover(
        document: vscode.TextDocument,
        position: vscode.Position,
    ): Promise<vscode.Hover | undefined> {
        if (!this.client.isRunning || !dentroDeAsm(document, position.line)) {
            return undefined;
        }
        const linea = document.lineAt(position.line).text;
        const limpia = sinComentario(linea).trim();
        if (limpia.length === 0 || limpia.endsWith(':')) {
            return undefined; // una etiqueta no es una instruccion.
        }

        // Se pide por LINEA, no por texto: asi el servidor responde por lo que
        // el compilador entendio de esa instruccion, no por lo que otro
        // emparejador crea que pone.
        const target = inspectTarget();
        let ficha: InstructionResponse;
        try {
            ficha = await this.client.request<InstructionResponse>(
                VestaMethod.Instruction,
                {
                    uri: document.uri.toString(),
                    line: position.line + 1,
                    cpu: target.cpu ?? '',
                    // A que base preguntar cuando el bloque no es del anfitrion:
                    // un `asm` de arm no se resuelve contra las de x86.
                    arch: target.arch ?? '',
                },
            );
        } catch {
            return undefined;
        }
        if (!ficha.found) {
            return undefined;
        }
        return new vscode.Hover(componer(limpia, ficha));
    }
}

/**
 * @brief Compone la ficha en markdown.
 * @param linea Linea de ensamblador.
 * @param f     Lo que devolvio el compilador.
 * @return El texto a mostrar.
 */
function componer(linea: string, f: InstructionResponse): vscode.MarkdownString {
    const md = new vscode.MarkdownString();
    md.appendCodeblock(linea, 'nasm');

    const cabecera: string[] = [];
    if (f.iclass) {
        cabecera.push(f.iclass);
    }
    if (f.extension) {
        cabecera.push('`' + f.extension + '`');
    }
    if (cabecera.length > 0) {
        md.appendMarkdown(cabecera.join('  ') + '\n\n');
    }

    // Que la base no la conozca es lo primero que hay que decir: lo de abajo
    // sigue siendo cierto, pero se queda corto a proposito.
    if (f.known === false && f.unknownReason) {
        md.appendMarkdown('**' + f.unknownReason + '**\n\n');
    }

    const c = f.cost;
    if (c?.timed) {
        md.appendMarkdown('**' + vscode.l10n.t('On {0}', f.microarch ?? '') + '**\n\n');
        md.appendMarkdown('- ' + vscode.l10n.t('latency: {0}', c.latency ?? '') + '\n');
        md.appendMarkdown(
            '- ' + vscode.l10n.t('can repeat every: {0}',
                                 c.reciprocalThroughput ?? '') + '\n');
        md.appendMarkdown('- ' + vscode.l10n.t('uops: {0}', c.uops ?? '') + '\n');
        if (c.divCycles !== undefined) {
            md.appendMarkdown(
                '- ' + vscode.l10n.t('division cycles: {0}', c.divCycles) + '\n');
        }
        if (c.microcoded) {
            md.appendMarkdown('- ' + vscode.l10n.t('microcoded') + '\n');
        }
        if (c.macroFusible) {
            md.appendMarkdown('- ' + vscode.l10n.t('can fuse with the next one') + '\n');
        }
        const puertos = (c.ports ?? [])
            .map(p => `${p.name ?? p.port}x${p.uops}`)
            .join(', ');
        if (puertos.length > 0) {
            md.appendMarkdown('- ' + vscode.l10n.t('ports: {0}', puertos) + '\n');
        }
        md.appendMarkdown('\n');
    } else if (f.microarch) {
        // Decir que esa maquina no la cronometra no es lo mismo que no saber
        // nada de la instruccion: lo demas sigue siendo cierto.
        md.appendMarkdown(
            '_' + vscode.l10n.t('{0} has no timings for this form.', f.microarch) +
            '_\n\n');
    }

    const toca: string[] = [];
    if (f.reads?.length) {
        toca.push(vscode.l10n.t('reads `{0}`', f.reads.join(', ')));
    }
    if (f.writes?.length) {
        toca.push(vscode.l10n.t('writes `{0}`', f.writes.join(', ')));
    }
    if (f.readsMemory) {
        toca.push(vscode.l10n.t('reads memory'));
    }
    if (f.writesMemory) {
        toca.push(vscode.l10n.t('writes memory'));
    }
    if (f.flagsRead?.length) {
        toca.push(vscode.l10n.t('reads flags `{0}`', f.flagsRead.join(', ')));
    } else if (f.readsFlags) {
        toca.push(vscode.l10n.t('reads flags'));
    }
    if (f.flagsWritten?.length) {
        toca.push(vscode.l10n.t('writes flags `{0}`', f.flagsWritten.join(', ')));
    } else if (f.writesFlags) {
        toca.push(vscode.l10n.t('writes flags'));
    }
    if (f.readsState?.length) {
        toca.push(vscode.l10n.t('reads `{0}`', f.readsState.join(', ')));
    }
    if (f.writesState?.length) {
        toca.push(vscode.l10n.t('writes `{0}`', f.writesState.join(', ')));
    }
    if (toca.length > 0) {
        md.appendMarkdown(
            '**' + vscode.l10n.t('Touches:') + '** ' + toca.join(', ') + '\n\n');
    }

    if (f.barrier) {
        md.appendMarkdown(
            '**' + vscode.l10n.t('Barrier:') + '** ' +
            vscode.l10n.t('nothing can move across it.') + '\n\n');
    }
    if (f.isCall) {
        md.appendMarkdown(
            '**' + vscode.l10n.t('Call:') + '** ' +
            vscode.l10n.t('every effect is assumed.') + '\n\n');
    }

    // Que hizo el compilador con ella.  No todas las instrucciones acaban como
    // una instruccion: el subconjunto computacional se eleva a operaciones del
    // IR y a partir de ahi el optimizador las mueve como cualquier otro codigo.
    if (f.lifted === 'ir') {
        /* Con las operaciones y sin ellas son dos frases ENTERAS: un inciso
         * entre parentesis no cae en el mismo sitio en todos los idiomas. */
        const ops = (f.irOps ?? []).slice(0, 6).join(', ');
        md.appendMarkdown(
            '**' + vscode.l10n.t('Lifted:') + '** ' +
            (ops
                ? vscode.l10n.t(
                    'the compiler turned it into IR operations (`{0}`), so it optimises it like the rest of the code.',
                    ops)
                : vscode.l10n.t(
                    'the compiler turned it into IR operations, so it optimises it like the rest of the code.')) +
            '\n\n',
        );
    } else if (f.lifted === 'micro') {
        md.appendMarkdown(
            '_' + vscode.l10n.t(
                'Emitted as is; the compiler reorders it knowing what it touches.') +
            '_\n\n',
        );
    }
    if (f.resolvedBy === 'text') {
        md.appendMarkdown(
            '_' + vscode.l10n.t(
                'Resolved against the database by its text: the compiler left no instruction for this line.') +
            '_\n\n',
        );
    }
    if (f.modeled === false) {
        md.appendMarkdown(
            '_' + vscode.l10n.t(
                'Its operands are not modelled: it is treated conservatively and nothing is reordered around it.') +
            '_\n',
        );
    }
    return md;
}

/**
 * @brief Indica si una linea cae dentro de un bloque de ensamblador.
 *
 * Se cuenta hacia atras: si desde la ultima apertura de un bloque `asm` hasta
 * aqui las llaves no se han cerrado, estamos dentro.  Es una comprobacion
 * barata, y equivocarse solo significa no ensenar la ficha o pedirla para una
 * linea que la base no reconoce, que tampoco ensena nada.
 *
 * @param document Documento.
 * @param linea    Linea a comprobar.
 * @return true si esa linea es ensamblador.
 */
function dentroDeAsm(document: vscode.TextDocument, linea: number): boolean {
    let profundidad = 0;
    for (let i = linea; i >= 0; i--) {
        const texto = sinComentario(document.lineAt(i).text);
        // Se recorre la linea al reves para que el balance cuadre subiendo.
        for (let c = texto.length - 1; c >= 0; c--) {
            if (texto[c] === '}') {
                profundidad++;
            } else if (texto[c] === '{') {
                if (profundidad === 0) {
                    // Esta llave es la que nos contiene: mirar quien la abre.
                    return ABRE_ASM.test(texto);
                }
                profundidad--;
            }
        }
    }
    return false;
}

/**
 * @brief Quita el comentario de una linea, en las dos formas que se usan.
 * @param texto Linea completa.
 * @return La linea sin su comentario.
 */
function sinComentario(texto: string): string {
    const puntoYComa = texto.indexOf(';');
    const barras = texto.indexOf('//');
    let corte = -1;
    if (puntoYComa >= 0) {
        corte = puntoYComa;
    }
    if (barras >= 0 && (corte < 0 || barras < corte)) {
        corte = barras;
    }
    return corte >= 0 ? texto.slice(0, corte) : texto;
}
