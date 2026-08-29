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
        md.appendMarkdown(`**En ${f.microarch}**\n\n`);
        md.appendMarkdown(`- latencia: ${c.latency}\n`);
        md.appendMarkdown(`- se puede repetir cada: ${c.reciprocalThroughput}\n`);
        md.appendMarkdown(`- uops: ${c.uops}\n`);
        if (c.divCycles !== undefined) {
            md.appendMarkdown(`- ciclos de division: ${c.divCycles}\n`);
        }
        if (c.microcoded) {
            md.appendMarkdown('- microcodificada\n');
        }
        if (c.macroFusible) {
            md.appendMarkdown('- se puede fusionar con la siguiente\n');
        }
        const puertos = (c.ports ?? [])
            .map(p => `${p.name ?? p.port}x${p.uops}`)
            .join(', ');
        if (puertos.length > 0) {
            md.appendMarkdown(`- puertos: ${puertos}\n`);
        }
        md.appendMarkdown('\n');
    } else if (f.microarch) {
        // Decir que esa maquina no la cronometra no es lo mismo que no saber
        // nada de la instruccion: lo demas sigue siendo cierto.
        md.appendMarkdown(`_${f.microarch} no cronometra esta forma._\n\n`);
    }

    const toca: string[] = [];
    if (f.reads?.length) {
        toca.push(`lee \`${f.reads.join(', ')}\``);
    }
    if (f.writes?.length) {
        toca.push(`escribe \`${f.writes.join(', ')}\``);
    }
    if (f.readsMemory) {
        toca.push('lee memoria');
    }
    if (f.writesMemory) {
        toca.push('escribe memoria');
    }
    if (f.flagsRead?.length) {
        toca.push(`lee banderas \`${f.flagsRead.join(', ')}\``);
    } else if (f.readsFlags) {
        toca.push('lee banderas');
    }
    if (f.flagsWritten?.length) {
        toca.push(`escribe banderas \`${f.flagsWritten.join(', ')}\``);
    } else if (f.writesFlags) {
        toca.push('escribe banderas');
    }
    if (f.readsState?.length) {
        toca.push(`lee \`${f.readsState.join(', ')}\``);
    }
    if (f.writesState?.length) {
        toca.push(`escribe \`${f.writesState.join(', ')}\``);
    }
    if (toca.length > 0) {
        md.appendMarkdown('**Toca:** ' + toca.join(', ') + '\n\n');
    }

    if (f.barrier) {
        md.appendMarkdown('**Barrera:** nada se puede mover al otro lado.\n\n');
    }
    if (f.isCall) {
        md.appendMarkdown('**Llamada:** se le supone todo efecto.\n\n');
    }

    // Que hizo el compilador con ella.  No todas las instrucciones acaban como
    // una instruccion: el subconjunto computacional se eleva a operaciones del
    // IR y a partir de ahi el optimizador las mueve como cualquier otro codigo.
    if (f.lifted === 'ir') {
        const ops = (f.irOps ?? []).slice(0, 6).join(', ');
        md.appendMarkdown(
            '**Elevada:** el compilador la convirtio en operaciones del IR' +
            (ops ? ` (\`${ops}\`)` : '') +
            ', asi que la optimiza como al resto del codigo.\n\n',
        );
    } else if (f.lifted === 'micro') {
        md.appendMarkdown(
            '_Se emite tal cual; el compilador la reordena sabiendo lo que ' +
            'toca._\n\n',
        );
    }
    if (f.resolvedBy === 'texto') {
        md.appendMarkdown(
            '_Resuelta contra la base por su texto: el compilador no dejo una ' +
            'instruccion para esta linea._\n\n',
        );
    }
    if (f.modeled === false) {
        md.appendMarkdown(
            '_Sus operandos no estan modelados: se trata de forma ' +
            'conservadora y no se reordena a su alrededor._\n',
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
