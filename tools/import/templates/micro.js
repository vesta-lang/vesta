// Conocimiento microarquitectural compartido por las paginas:
//   window.VESTA_PORTDESC(nombrePuerto) -> que HACE ese puerto (tooltip).
//   window.VESTA_ISSUE_WIDTH[microarq]  -> ancho de emision (uops/ciclo).
// Las descripciones se traducen via window.t (i18n.js); si no esta cargado,
// se usa la clave como texto (identidad).
(function () {
    const T = k => (window.t ? window.t(k) : k);
    // AMD nombra los puertos por FUNCION.  Clave i18n por nombre de puerto.
    const AMD = {
        LD: 'port.amd.ld', ALU: 'port.amd.alu', AGU: 'port.amd.agu',
        STA: 'port.amd.sta', STD: 'port.amd.std', JMP: 'port.amd.jmp', BR: 'port.amd.jmp',
        MUL: 'port.amd.mul', DIV: 'port.amd.div', SHIFT: 'port.amd.shift', SLOW: 'port.amd.slow',
        INT_OTHER: 'port.amd.int_other', UNKNOWN: 'port.amd.unknown',
    };
    // Intel numera los puertos; funcion tipica del esquema moderno (Skylake y
    // similares; en Golden Cove/Gracemont la reparticion varia un poco).
    const INTEL = {
        '0': 'port.intel.0', '1': 'port.intel.1', '2': 'port.intel.2', '3': 'port.intel.3',
        '4': 'port.intel.4', '5': 'port.intel.5', '6': 'port.intel.6', '7': 'port.intel.7',
        '8': 'port.intel.8', '9': 'port.intel.9',
    };
    window.VESTA_PORTDESC = function (name) {
        const up = name.toUpperCase();
        if (AMD[up]) return name + ': ' + T(AMD[up]);
        let m = name.match(/^FP([0-9]+)$/i);
        if (m) return T('port.fp.pre') + ' ' + m[1].split('').map(d => 'FP' + d).join(', ') +
            '; ' + T('port.fp.post');
        m = name.match(/^p([0-9]+)([a-z]*)$/i);
        if (m) {
            const clus = m[2] ? ' (' + T('port.cluster') + ' ' + m[2].toUpperCase() + ')' : '';
            return T('port.dispatch') + clus + ':\n' +
                m[1].split('').map(d => '  p' + d + ': ' + T(INTEL[d] || 'port.exec')).join('\n');
        }
        return name + ': ' + T('port.exec');
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
        // ARM (ancho de emision aproximado del core)
        'neoverse-n1': 4, 'neoverse-n2': 5, 'neoverse-v1': 8, 'neoverse-v2': 8,
        'neoverse-v3': 8, 'cortex-a76': 4, 'cortex-a76-a32': 4, 'cortex-x4': 10,
        'cortex-a53': 2, 'cortex-a57': 3, 'a64fx': 4, 'neoverse-n1-a32': 4,
        // RISC-V (IssueWidth del modelo de scheduling)
        'rocket': 1, 'sifive-7-series': 2, 'sifive-p450': 3, 'sifive-p670': 4,
        'syntacore-scr1': 1, 'syntacore-scr3-rv32': 1, 'syntacore-scr3-rv64': 1,
        'xiangshan-nanhu': 6,
    };
})();
