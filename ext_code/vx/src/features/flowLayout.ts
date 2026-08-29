/**
 * @file flowLayout.ts
 * @brief El dibujo de las flechas de flujo, sin depender del editor.
 *
 * Aqui esta el calculo puro: agrupar los saltos, repartirlos en carriles y
 * decidir que caracter va en cada columna de cada linea.  Pintarlo es cosa de
 * `flowArrows.ts`; separarlo permite PROBARLO -- se le dan saltos y se
 * comprueba el dibujo que sale -- en lugar de mirar el fuente con expresiones
 * regulares a ver si menciona las palabras correctas, que es lo que se hacia
 * antes y dejaba pasar los fallos que costaron un intento cada uno.
 *
 * LO QUE SE DIBUJA ES EL DESTINO, NO EL SALTO.  Un bloque escrito a mano manda
 * a la misma etiqueta desde varios sitios -- tres `jmp .less` es lo normal --,
 * y con una linea por salto salian tres verticales paralelas, casi pegadas,
 * que acababan en el mismo sitio sin que nada lo dijera: seis carriles para
 * dos destinos.  Se agrupan por destino: UNA vertical por etiqueta, cada
 * origen se engancha a ella con un tramo horizontal, y la punta se pinta una
 * sola vez donde se llega.  Es como lo dibuja un desensamblador, y ademas deja
 * el dibujo mas estrecho, que es anchura que el codigo no pierde.
 */

/** Uno de los sitios desde los que se salta a un destino. */
export interface Fuente {
    /** Linea del salto (contando desde cero). */
    linea: number;
    /** Que es: una rama condicional, un salto, una llamada. */
    clase: string;
}

/** Todo lo que llega a una misma linea, dibujado como un solo trazo. */
export interface Grupo {
    /** Linea a la que se llega (contando desde cero). */
    hasta: number;
    /** De donde se llega, ordenadas de arriba abajo. */
    fuentes: Fuente[];
    /** Primera linea que ocupa el trazo. */
    arriba: number;
    /** Ultima linea que ocupa el trazo. */
    abajo: number;
    /** Carril en el que se dibuja, para que dos que se solapan no se pisen. */
    carril: number;
}

/** Lo que se pinta en una columna de una linea. */
export interface Celda {
    /** El caracter. */
    trazo: string;
    /** Color del grupo al que pertenece. */
    color: string;
}

/**
 * Los trazos, escapados para que el fichero siga siendo ASCII puro.
 *
 * El hueco es un espacio y NO se omite: es lo que mantiene todas las lineas del
 * bloque a la misma anchura.  Dibujando solo donde hay algo, una linea con
 * flecha queda corrida respecto de la de al lado, y un bloque de ensamblador --
 * alineado a mano en columnas -- se descuadra entero.
 */
export const T = {
    /** Vertical: el trazo pasa de largo por esta linea. */
    vertical: '\u2502',
    /** Esquina de arriba: el trazo empieza aqui y baja. */
    esquinaAbajo: '\u256d',
    /** Esquina de abajo: el trazo acaba aqui y viene de arriba. */
    esquinaArriba: '\u2570',
    /** Empalme: el trazo sigue de largo Y ademas sale hacia el codigo. */
    empalme: '\u251c',
    /** El tramo que corre hacia el codigo. */
    horizontal: '\u2500',
    /** Ese tramo cruzando el carril de otro trazo. */
    cruce: '\u253c',
    /** A donde SE LLEGA. */
    punta: '\u25b6',
    /** Por donde el flujo se VA del bloque: la punta mira hacia fuera. */
    salida: '\u25c0',
    hueco: ' ',
} as const;

/**
 * Los colores de los carriles, ciclicos.
 *
 * Dos trazos que se solapan van en carriles distintos y por tanto en colores
 * distintos: es lo que permite seguir cada uno con la vista cuando hay varios
 * anidados, que es justo el caso en el que las flechas hacen falta.
 */
export const COLORES = [
    '#d29922', // ambar
    '#3fb950', // verde
    '#58a6ff', // azul
    '#db6d28', // naranja
    '#a371f7', // violeta
    '#f85149', // rojo
];

/**
 * El color de una salida del bloque.
 *
 * Gris y fuera de la lista de carriles a proposito: una salida no es un carril
 * -- no tiene recorrido, no se solapa con nadie --, y darle uno de los colores
 * de los saltos la haria pasar por lo que no es.
 */
export const COLOR_SALIDA = '#8b949e';

/**
 * Cuantos carriles se dibujan como mucho.
 *
 * Mas que esto no se distinguen ni con colores, y cada uno cuesta una columna
 * delante del codigo.  Los que no caben se apilan en el ultimo.
 */
export const MAX_CARRILES = 6;

/** Columnas del dibujo: los carriles mas la que empalma con el codigo. */
export const MAX_COLUMNAS = MAX_CARRILES + 1;

/**
 * @brief Agrupa los saltos por destino y les reparte carril.
 *
 * Dos trazos que se solapan no pueden compartir columna: sus lineas se
 * confundirian en una sola.  Se busca para cada uno el primer carril libre en
 * todo su recorrido, que es lo mismo que hace un desensamblador.
 *
 * Los cortos primero: asi quedan en el carril 0, que es el pegado al codigo, y
 * los largos los rodean por fuera.  Un trazo corto es casi siempre el que se
 * esta siguiendo, y tenerlo cerca del codigo es lo que hace que se lea.
 *
 * @param saltos Saltos del bloque, en lineas contando desde uno.
 * @return Un grupo por destino, en lineas contando desde cero.
 */
export function repartirCarriles(
    saltos: { fromLine: number; toLine: number; flow: string }[],
): Grupo[] {
    /* 1. Juntar por destino.  Un `Map` conserva el orden de insercion, que es
     *    el del fichero: el dibujo sale igual entre repintados. */
    const porDestino = new Map<number, Fuente[]>();
    for (const salto of saltos) {
        if (salto.fromLine <= 0 || salto.toLine <= 0) {
            continue;
        }
        if (salto.fromLine === salto.toLine) {
            continue; // un salto a su propia linea no tiene trazo que dibujar
        }
        const destino = salto.toLine - 1;
        const fuentes = porDestino.get(destino) ?? [];
        const origen = salto.fromLine - 1;
        /* La misma linea no se apunta dos veces.  Una instruccion aparece una
         * sola vez, asi que si llegan dos entradas iguales es que el mismo
         * salto se conto dos veces; sin esto el emergente diria "se llega desde
         * 2 sitios" nombrando la misma linea las dos veces. */
        if (!fuentes.some(f => f.linea === origen)) {
            fuentes.push({ linea: origen, clase: salto.flow });
        }
        porDestino.set(destino, fuentes);
    }

    // 2. Cada grupo ocupa desde su linea mas alta hasta la mas baja.
    const grupos: Grupo[] = [];
    for (const [hasta, fuentes] of porDestino) {
        fuentes.sort((a, b) => a.linea - b.linea);
        const lineas = [hasta, ...fuentes.map(f => f.linea)];
        grupos.push({
            hasta,
            fuentes,
            arriba: Math.min(...lineas),
            abajo: Math.max(...lineas),
            carril: 0,
        });
    }

    // 3. Repartir carriles, los cortos primero.
    const ocupacion: [number, number][][] = [];
    const porLongitud = [...grupos].sort(
        (a, b) => a.abajo - a.arriba - (b.abajo - b.arriba),
    );
    for (const grupo of porLongitud) {
        let carril = 0;
        while (carril < MAX_CARRILES - 1) {
            const tramos = ocupacion[carril] ?? [];
            if (tramos.every(t => grupo.abajo < t[0] || grupo.arriba > t[1])) {
                break;
            }
            carril++;
        }
        if (!ocupacion[carril]) {
            ocupacion[carril] = [];
        }
        ocupacion[carril].push([grupo.arriba, grupo.abajo]);
        grupo.carril = carril;
    }
    return grupos;
}

/**
 * @brief Cuantos carriles ocupa de verdad un reparto.
 * @param grupos Grupos ya repartidos.
 * @return El numero de carriles a dibujar, o cero si no hay ninguno.
 */
export function carrilesUsados(grupos: Grupo[], haySalidas = false): number {
    const usados =
        grupos.length === 0
            ? 0
            : Math.min(Math.max(...grupos.map(g => g.carril)) + 1,
                       MAX_CARRILES);
    /* Una salida se dibuja desde la columna 0, la de mas afuera, asi que se
     * reserva una columna para ella EN VEZ de meterla en el carril 0: ahi
     * taparia la vertical de un salto que pasa por esa linea y partiria la
     * flecha en dos.  Sin ningun salto, esa unica columna es todo el dibujo. */
    return haySalidas ? Math.min(usados + 1, MAX_CARRILES) : usados;
}

/**
 * @brief Indica si una linea es uno de los extremos de un grupo.
 * @param grupo Grupo a mirar.
 * @param linea Linea (contando desde cero).
 * @return Cierto si de ahi sale un salto o ahi llega el trazo.
 */
export function esExtremo(grupo: Grupo, linea: number): boolean {
    return grupo.hasta === linea || grupo.fuentes.some(f => f.linea === linea);
}

/**
 * @brief Dibuja una linea entera: los carriles y el tramo hacia el codigo.
 *
 * Las columnas van de izquierda a derecha y los carriles al reves: el carril 0
 * -- el del trazo mas corto -- queda pegado al codigo, y los largos por fuera.
 * La ultima columna es la que empalma con el codigo.
 *
 * @param grupos   Grupos del bloque, ya con carril.
 * @param linea    Linea (contando desde cero).
 * @param carriles Cuantos carriles se dibujan.
 * @param salidas  Lineas por las que el flujo se va del bloque.
 * @return Una celda por columna; SIEMPRE las mismas, con hueco donde no hay
 *         trazo, para que todas las lineas del bloque midan igual.
 */
export function dibujarLinea(
    grupos: Grupo[],
    linea: number,
    carriles: number,
    salidas?: ReadonlySet<number>,
): Celda[] {
    const columnas = carriles + 1;
    const celdas: Celda[] = [];
    for (let i = 0; i < columnas; i++) {
        celdas.push({ trazo: T.hueco, color: COLORES[0] });
    }
    /** La columna en la que se pinta un carril. */
    const colDe = (carril: number): number => carriles - 1 - carril;
    const visibles = grupos.filter(g => g.carril < carriles);

    // 1. Los verticales: todo grupo que pasa por esta linea ocupa su carril.
    for (const grupo of visibles) {
        if (linea < grupo.arriba || linea > grupo.abajo) {
            continue;
        }
        const col = colDe(grupo.carril);
        const color = COLORES[grupo.carril % COLORES.length];
        let trazo: string;
        if (linea === grupo.arriba) {
            trazo = T.esquinaAbajo;
        } else if (linea === grupo.abajo) {
            trazo = T.esquinaArriba;
        } else {
            /* En medio del recorrido.  Si ademas es un extremo -- uno de los
             * varios origenes, o el destino con origenes por encima y por
             * debajo -- el trazo sigue de largo Y sale hacia el codigo: eso es
             * un empalme, no una vertical.  Sin esto, un salto intermedio se
             * quedaba con su tramo horizontal saliendo de la nada. */
            trazo = esExtremo(grupo, linea) ? T.empalme : T.vertical;
        }
        celdas[col] = { trazo, color };
    }

    /* 2. Los tramos horizontales de los EXTREMOS.
     *
     * Es lo que dice de donde a donde va cada trazo: sin esto se ve una raya
     * vertical que no toca ninguna instruccion.  Corre desde su carril hasta el
     * codigo, y donde cruza el carril de otro trazo se marca el cruce en vez de
     * borrarlo -- borrarlo partiria la otra flecha en dos --.
     *
     * De fuera hacia dentro: el tramo mas corto es el que manda sobre la
     * columna pegada al codigo, que es la que se mira. */
    const extremos = visibles
        .filter(g => esExtremo(g, linea))
        .sort((a, b) => b.carril - a.carril);
    for (const grupo of extremos) {
        const color = COLORES[grupo.carril % COLORES.length];
        for (let col = colDe(grupo.carril) + 1; col < columnas; col++) {
            const ocupada = celdas[col].trazo !== T.hueco;
            celdas[col] = { trazo: ocupada ? T.cruce : T.horizontal, color };
        }
    }

    /* 3. Las salidas del bloque.
     *
     * El flujo se va a una funcion del modulo: no hay linea a la que apuntar,
     * asi que el trazo cruza hacia FUERA y la punta mira al reves.  Cruza los
     * carriles que haya en medio igual que cualquier otro tramo, sin borrarlos.
     */
    if (salidas?.has(linea)) {
        for (let col = 0; col < columnas; col++) {
            const ocupada = celdas[col].trazo !== T.hueco;
            celdas[col] = {
                trazo: col === 0 ? T.salida : ocupada ? T.cruce : T.horizontal,
                color: COLOR_SALIDA,
            };
        }
    }

    /* 4. La punta, la ultima.
     *
     * Se decide aparte porque una misma linea puede ser el destino de un trazo
     * y el origen de otro: si mandara el orden de dibujo, el tramo del origen
     * taparia la punta y la linea a la que se LLEGA no se distinguiria.  Se
     * queda con ella el carril mas pegado al codigo.  Manda tambien sobre una
     * salida: a una linea con etiqueta se puede llegar Y de ella irse, y lo que
     * no se puede perder es que ahi LLEGA algo. */
    const destino = extremos.filter(g => g.hasta === linea).pop();
    if (destino) {
        celdas[columnas - 1] = {
            trazo: T.punta,
            color: COLORES[destino.carril % COLORES.length],
        };
    }
    return celdas;
}
