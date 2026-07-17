// Conocimiento microarquitectural compartido por las paginas:
//   window.VESTA_PORTDESC(nombrePuerto) -> que HACE ese puerto (tooltip).
//   window.VESTA_ISSUE_WIDTH[microarq]  -> ancho de emision (uops/ciclo).
(function () {
    // AMD nombra los puertos por FUNCION (traducible con exactitud).
    const AMD = {
        LD: 'carga (load)', ALU: 'ALU entera', AGU: 'generacion de direccion (AGU)',
        STA: 'direccion de almacenamiento (store-address)', STD: 'dato de almacenamiento (store-data)',
        JMP: 'salto / branch', BR: 'salto / branch', MUL: 'multiplicacion entera', DIV: 'division',
        SHIFT: 'desplazamientos', SLOW: 'ruta lenta (operaciones complejas / microcodigo)',
        INT_OTHER: 'otras operaciones enteras', UNKNOWN: 'puerto no identificado por la fuente',
    };
    // Intel numera los puertos; funcion tipica del esquema moderno (Skylake y
    // similares; en Golden Cove/Gracemont la reparticion varia un poco).
    const INTEL = {
        '0': 'ALU, vector/FP, desplazamientos',
        '1': 'ALU, LEA, multiplicacion entera, vector/FP (FMA)',
        '2': 'carga + calculo de direccion (AGU)',
        '3': 'carga + calculo de direccion (AGU)',
        '4': 'escritura del dato a memoria (store-data)',
        '5': 'ALU, vector (shuffle, permutaciones)',
        '6': 'ALU, desplazamientos, ramas',
        '7': 'calculo de la direccion de escritura (store-AGU)',
        '8': 'carga / calculo de direccion',
        '9': 'escritura a memoria (store)',
    };
    window.VESTA_PORTDESC = function (name) {
        const up = name.toUpperCase();
        if (AMD[up]) return name + ': ' + AMD[up];
        let m = name.match(/^FP([0-9]+)$/i);
        if (m) return 'unidad(es) FP/vectorial ' + m[1].split('').map(d => 'FP' + d).join(', ') +
            '; la µop va a una de ellas (suma/mul/shuffle/conversion FP y SIMD)';
        m = name.match(/^p([0-9]+)([a-z]*)$/i);
        if (m) {
            const clus = m[2] ? ' (cluster ' + m[2].toUpperCase() + ')' : '';
            return 'la µop se despacha a UNO de estos puertos' + clus + ':\n' +
                m[1].split('').map(d => '  p' + d + ': ' + (INTEL[d] || 'unidad de ejecucion')).join('\n');
        }
        return name + ': unidad de ejecucion';
    };

    // Ancho de emision / rename del front-end (µops por ciclo).  Junto con los
    // puertos, es lo que limita la ejecucion superescalar/paralela.  Valores
    // documentados por microarquitectura (default 4 si no esta).
    window.VESTA_ISSUE_WIDTH = {
        'intel-haswell': 4, 'intel-skylake': 4, 'intel-skylake-x': 4,
        'intel-icelake': 5, 'intel-rocketlake': 5,
        'intel-alderlake-p': 6, 'intel-emeraldrapids': 6, 'intel-arrowlake-p': 8,
        'intel-alderlake-e': 5,
        'amd-zen2': 5, 'amd-zen3': 6, 'amd-zen4': 6, 'amd-zen5': 8,
    };
})();
