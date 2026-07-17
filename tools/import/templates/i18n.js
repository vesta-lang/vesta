// Soporte multi-idioma.  Catalogo es/en + aplicacion sobre [data-i18n] y helper
// window.t(clave) para los textos generados por JS.  El selector recarga la
// pagina (guarda el idioma en localStorage); asi el visor/analizador se
// re-renderizan con el idioma nuevo sin logica de re-render.
(function () {
    const STR = {
        es: {
            'nav.instr': 'Instrucciones', 'nav.an': 'Analizador', 'nav.repo': 'Repositorio',
            'idx.title': 'Base de datos de instrucciones x86',
            'idx.help': 'Como leer esta tabla',
            'bar.search': 'Buscar mnemonico, iclass, opcode o encoding...',
            'bar.iclass': 'iclass', 'bar.uarch': 'microarq.', 'bar.all': 'todos',
            'th.id': 'ID', 'th.form': 'Forma', 'th.iclass': 'iclass', 'th.opcode': 'opcode',
            'th.encoding': 'encoding', 'th.operands': 'operandos', 'th.overlay': 'overlay',
            'th.reciptp': 'recip_tp', 'th.uops': 'uops', 'th.latmax': 'lat. max',
            'an.title': 'Analizador de ensamblador x86',
            'an.desc': 'Pega instrucciones (sintaxis Intel, una por linea). Para cada una se busca su forma en la base de datos y se reporta lo que hace y su coste en la microarquitectura elegida.',
            'an.help': 'Como funciona el analisis',
            'an.run': 'Analizar', 'an.opts': 'Optimizaciones:',
            'opt.zero': 'puesta a cero', 'opt.strength': 'reduccion de fuerza',
            'opt.selfcopy': 'copia a si mismo', 'opt.dce': 'codigo muerto', 'opt.reorder': 'reordenacion',
            'an.col.instr': 'instruccion', 'an.col.form': 'forma', 'an.col.ports': 'puertos',
            'an.col.lat': 'latencia', 'an.matched': 'emparejadas', 'an.nomatch': 'sin forma',
            'an.notfound': 'forma no encontrada', 'an.badops': '(operandos no encajan)',
            'an.badmn': '(mnemonico desconocido)', 'an.nodata': 'sin dato', 'an.empty': 'Sin instrucciones que analizar.',
            'an.block': 'Analisis del bloque', 'an.blockopt': 'Analisis del bloque optimizado',
            'an.uops': 'micro-operaciones (uops)',
            'an.fe': 'coste por <b>front-end</b> (decodificacion/emision, {w} µops/ciclo)',
            'an.tp': 'coste por <b>throughput</b> (puertos, &Sigma; recip_tp)',
            'an.lat': 'coste por <b>latencia</b> (camino critico de dependencias)',
            'an.est': 'estimacion del bloque = max(front-end, throughput, latencia)',
            'an.bneck': 'cuello de botella', 'an.cycles': 'ciclos',
            'an.optcode': 'Codigo optimizado', 'an.optapplied': 'Optimizaciones aplicadas',
            'an.noopt': 'Sin optimizaciones aplicables: el codigo ya es optimo para las reglas del analizador.',
            'an.cmp.orig': 'original', 'an.cmp.opt': 'optimizado', 'an.cmp.ba': 'Antes / despues (misma escala)',
            'an.before': 'ANTES', 'an.after': 'DESPUES', 'an.elimnote': '(tachadas = se eliminan)',
            'an.eliminated': '(eliminada)', 'an.cccap': 'camino critico (cadena de dependencias; pasa el cursor por una flecha)',
            'rule.zero': 'idioma de puesta a cero', 'rule.strength': 'reduccion de fuerza',
            'rule.selfcopy': 'copia a si mismo', 'rule.dce': 'eliminacion de codigo muerto',
            'rule.reorder': 'reordenacion (planificacion)',
        },
        en: {
            'nav.instr': 'Instructions', 'nav.an': 'Analyzer', 'nav.repo': 'Repository',
            'idx.title': 'x86 instruction database',
            'idx.help': 'How to read this table',
            'bar.search': 'Search mnemonic, iclass, opcode or encoding...',
            'bar.iclass': 'iclass', 'bar.uarch': 'microarch.', 'bar.all': 'all',
            'th.id': 'ID', 'th.form': 'Form', 'th.iclass': 'iclass', 'th.opcode': 'opcode',
            'th.encoding': 'encoding', 'th.operands': 'operands', 'th.overlay': 'overlay',
            'th.reciptp': 'recip_tp', 'th.uops': 'uops', 'th.latmax': 'max lat.',
            'an.title': 'x86 assembly analyzer',
            'an.desc': 'Paste instructions (Intel syntax, one per line). For each one its form is looked up in the database, reporting what it does and its cost on the selected microarchitecture.',
            'an.help': 'How the analysis works',
            'an.run': 'Analyze', 'an.opts': 'Optimizations:',
            'opt.zero': 'zeroing', 'opt.strength': 'strength reduction',
            'opt.selfcopy': 'self copy', 'opt.dce': 'dead code', 'opt.reorder': 'reordering',
            'an.col.instr': 'instruction', 'an.col.form': 'form', 'an.col.ports': 'ports',
            'an.col.lat': 'latency', 'an.matched': 'matched', 'an.nomatch': 'no form',
            'an.notfound': 'form not found', 'an.badops': '(operands do not fit)',
            'an.badmn': '(unknown mnemonic)', 'an.nodata': 'no data', 'an.empty': 'Nothing to analyze.',
            'an.block': 'Block analysis', 'an.blockopt': 'Optimized block analysis',
            'an.uops': 'micro-operations (uops)',
            'an.fe': '<b>front-end</b> cost (decode/issue, {w} µops/cycle)',
            'an.tp': '<b>throughput</b> cost (ports, &Sigma; recip_tp)',
            'an.lat': '<b>latency</b> cost (dependency critical path)',
            'an.est': 'block estimate = max(front-end, throughput, latency)',
            'an.bneck': 'bottleneck', 'an.cycles': 'cycles',
            'an.optcode': 'Optimized code', 'an.optapplied': 'Optimizations applied',
            'an.noopt': 'No applicable optimizations: the code is already optimal for the analyzer rules.',
            'an.cmp.orig': 'original', 'an.cmp.opt': 'optimized', 'an.cmp.ba': 'Before / after (same scale)',
            'an.before': 'BEFORE', 'an.after': 'AFTER', 'an.elimnote': '(struck through = removed)',
            'an.eliminated': '(removed)', 'an.cccap': 'critical path (dependency chain; hover an arrow)',
            'rule.zero': 'zeroing idiom', 'rule.strength': 'strength reduction',
            'rule.selfcopy': 'self copy', 'rule.dce': 'dead code elimination',
            'rule.reorder': 'reordering (scheduling)',
        },
    };
    const lang = localStorage.getItem('vesta-lang') || (navigator.language || 'es').slice(0, 2);
    const L = STR[lang] ? lang : 'es';
    const D = STR[L];
    window.LANG = L;
    window.t = (k, vars) => {
        let s = D[k] != null ? D[k] : k;
        if (vars) for (const v in vars) s = s.replace('{' + v + '}', vars[v]);
        return s;
    };
    // aplicar sobre el DOM estatico (scripts al final del body -> DOM listo).
    document.querySelectorAll('[data-i18n]').forEach(el => { el.innerHTML = window.t(el.getAttribute('data-i18n')); });
    document.querySelectorAll('[data-i18n-ph]').forEach(el => { el.placeholder = window.t(el.getAttribute('data-i18n-ph')); });
    document.documentElement.lang = L;
    // selector de idioma en la nav.
    const nav = document.querySelector('.nav');
    if (nav) {
        const sel = document.createElement('select');
        sel.className = 'langsel';
        for (const code of Object.keys(STR)) {
            const o = document.createElement('option');
            o.value = code; o.textContent = code.toUpperCase();
            if (code === L) o.selected = true;
            sel.appendChild(o);
        }
        sel.onchange = () => { localStorage.setItem('vesta-lang', sel.value); location.reload(); };
        nav.appendChild(sel);
    }
})();
