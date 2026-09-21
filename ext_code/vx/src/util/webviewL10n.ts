/**
 * @file webviewL10n.ts
 * @brief El puente del idioma hasta dentro de un panel.
 *
 * Un panel corre en un marco aislado: alli no existe `vscode.l10n`, ni nada de
 * la extension.  Su texto tiene que VIAJAR, y este fichero es por donde pasa.
 *
 * El reparto es el mismo en los cuatro paneles:
 *
 *   - el texto se traduce UNA vez, en el anfitrion, con `vscode.l10n.t`, que es
 *     lo que el guardian de `test/l10n.test.js` sabe leer;
 *   - lo que aparece en el marcado se interpola (`${T.search}`);
 *   - lo que necesita el guion del panel se incrusta con `embedStrings` y se
 *     lee del objeto `T`.
 *
 * De ahi que el objeto lleve NOMBRES y no las frases: una cadena dentro del
 * guion no puede ser la clave, porque entonces habria dos sitios desde los que
 * traducir y el guardian solo mira uno.
 */

/**
 * @brief Incrusta los textos del panel como un objeto de su guion.
 *
 * Se escapa el menor-que porque una frase que contuviera `</script>` cerraria
 * el guion desde dentro -- el texto es de la traduccion, no del usuario, pero
 * el fallo no se veria hasta que alguien tradujera algo asi y la pagina saliera
 * partida por la mitad sin ningun error.
 *
 * @param strings Pares nombre -> texto ya traducido.
 * @return La linea de guion que define `T` y el relleno de huecos `tf`.
 */
export function embedStrings(strings: Record<string, string>): string {
    const json = JSON.stringify(strings).replace(/</g, '\\u003c');
    return (
        `var T = ${json};\n` +
        'function tf(template) {\n' +
        '    var args = Array.prototype.slice.call(arguments, 1);\n' +
        '    return String(template).replace(/\\{(\\d+)\\}/g, function (whole, i) {\n' +
        '        return args[i] === undefined ? whole : args[i];\n' +
        '    });\n' +
        '}'
    );
}
