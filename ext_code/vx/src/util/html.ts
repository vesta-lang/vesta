/**
 * @file html.ts
 * @brief Utilidades para construir el contenido de las vistas web.
 *
 * Las vistas de la extension muestran texto que viene del compilador (codigo
 * fuente, ensamblador, nombres de simbolos).  Ese texto se inserta en HTML, de
 * modo que hay que escaparlo siempre, y los guiones que lo acompanan se
 * autorizan con un valor unico por carga en lugar de abrir la politica de
 * seguridad entera.
 */

/** Longitud del valor unico que autoriza los guiones de una vista. */
const NONCE_LENGTH = 32;

/** Alfabeto del valor unico: solo caracteres validos en una cabecera. */
const NONCE_ALPHABET = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789';

/**
 * @brief Genera el valor unico que autoriza los guiones de una carga concreta.
 * @return Cadena aleatoria de longitud fija.
 */
export function createNonce(): string {
    let out = '';
    for (let i = 0; i < NONCE_LENGTH; i++) {
        out += NONCE_ALPHABET.charAt(Math.floor(Math.random() * NONCE_ALPHABET.length));
    }
    return out;
}

/**
 * @brief Escapa un texto para poder insertarlo dentro de un nodo HTML.
 * @param text Texto de origen, tal cual lo devuelve el compilador.
 * @return El mismo texto con los caracteres con significado ya neutralizados.
 */
export function escapeHtml(text: string): string {
    return text
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;')
        .replace(/'/g, '&#39;');
}

/**
 * @brief Autoriza los guiones de una pagina ya escrita por otro.
 *
 * Los diagramas en formato HTML llegan del compilador como una pagina completa
 * y autocontenida.  Para mostrarla sin relajar la politica de seguridad se le
 * anade el valor unico a cada etiqueta de guion, que es justo lo que la
 * politica exige.
 *
 * @param html  Pagina de origen.
 * @param nonce Valor unico de esta carga.
 * @return La misma pagina con los guiones autorizados.
 */
export function applyNonceToScripts(html: string, nonce: string): string {
    return html.replace(/<script(?![^>]*\bnonce=)/gi, `<script nonce="${nonce}"`);
}
