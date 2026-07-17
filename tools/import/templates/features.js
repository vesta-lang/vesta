// Features por microarquitectura: matriz CPU x feature (extraidas de LLVM).
// Datos en window.VESTA_DB.features[isa] = {table:[nombre...],
//   cpus:[[cpu, sched, [featIdx...]]...]}.  Multi-ISA por ?isa=.
(function () {
    const DB = window.VESTA_DB;
    const ORDER = DB.order || ['x86'];
    const params = new URLSearchParams(location.search);
    let ISA = params.get('isa');
    if (!ISA || !DB.features || !DB.features[ISA]) {
        ISA = ORDER.find(k => DB.features && DB.features[k]) || ORDER[0];
    }
    const FE = (DB.features && DB.features[ISA]) || { table: [], cpus: [] };
    const TABLE = FE.table, CPUS = FE.cpus;
    const T = window.t || (k => k);
    const LOC = window.LANG || 'es';
    const $ = id => document.getElementById(id);
    const esc = s => (s + '').replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

    // selector de ISA (solo las que tienen features).
    const isaSel = $('isa');
    if (isaSel) {
        isaSel.innerHTML = ORDER.filter(k => DB.features && DB.features[k]).map(k =>
            '<option value="' + k + '"' + (k === ISA ? ' selected' : '') + '>' +
            esc((DB.labels && DB.labels[k]) || k) + '</option>').join('');
        isaSel.onchange = () => { location.search = '?isa=' + isaSel.value; };
    }

    const meta = (DB.isas && DB.isas[ISA] && DB.isas[ISA].meta) || {};
    const sub = $('subline');
    if (sub) sub.innerHTML = T('feat.sub', {
        cpus: CPUS.length.toLocaleString(LOC), feats: TABLE.length,
        isa: (DB.labels && DB.labels[ISA]) || ISA, date: meta.date || 'llvm-19'
    });

    // color estable por feature (agrupa visualmente las relacionadas).
    function hue(s) { let h = 0; for (const c of s) h = (h * 31 + c.charCodeAt(0)) >>> 0; return h % 360; }

    const wrap = $('matrix-wrap'), q = $('q'), count = $('count');
    // por CPU, un Set de indices para O(1) lookup en la celda.
    const cpuSets = CPUS.map(c => new Set(c[2]));

    function render() {
        const query = q.value.trim().toLowerCase();
        const featMatch = TABLE.map((f, i) => [f, i]).filter(([f]) => f.toLowerCase().includes(query));
        const cpuMatch = CPUS.map((c, i) => i).filter(i => CPUS[i][0].toLowerCase().includes(query));
        // filas y columnas visibles segun a que casa la busqueda.
        let cols, rows;
        if (!query) { cols = TABLE.map((f, i) => i); rows = CPUS.map((c, i) => i); }
        else if (featMatch.length && !cpuMatch.length) { cols = featMatch.map(x => x[1]); rows = CPUS.map((c, i) => i); }
        else if (cpuMatch.length && !featMatch.length) { cols = TABLE.map((f, i) => i); rows = cpuMatch; }
        else if (featMatch.length && cpuMatch.length) { cols = featMatch.map(x => x[1]); rows = cpuMatch; }
        else { cols = []; rows = []; }

        count.textContent = rows.length + ' CPU x ' + cols.length + ' features';
        if (!rows.length || !cols.length) {
            wrap.innerHTML = '<p class="dim" style="padding:1rem">' + T('feat.none') + '</p>';
            return;
        }
        const out = ['<table class="feat-matrix"><thead><tr>',
            '<th class="cpu-h">' + T('feat.cpu') + '</th>',
            '<th class="sch-h">' + T('feat.sched') + '</th>',
            '<th class="cnt-h">' + T('feat.count') + '</th>'];
        for (const ci of cols) {
            const h = hue(TABLE[ci]);
            out.push('<th class="fh"><span style="color:hsl(' + h + ' 60% 42%)">' + esc(TABLE[ci]) + '</span></th>');
        }
        out.push('</tr></thead><tbody>');
        for (const ri of rows) {
            const c = CPUS[ri], set = cpuSets[ri];
            out.push('<tr><td class="cpu-c">' + esc(c[0]) + '</td>' +
                '<td class="sch-c">' + esc(c[1]) + '</td>' +
                '<td class="cnt-c">' + c[2].length + '</td>');
            for (const ci of cols) {
                if (set.has(ci)) {
                    const h = hue(TABLE[ci]);
                    out.push('<td class="yes" style="background:hsl(' + h + ' 60% 50% / .16)">&check;</td>');
                } else out.push('<td></td>');
            }
            out.push('</tr>');
        }
        out.push('</tbody></table>');
        wrap.innerHTML = out.join('');
    }

    q.addEventListener('input', render);
    render();
})();
