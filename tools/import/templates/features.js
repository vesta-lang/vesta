// Features por microarquitectura: vista de lista (chips legibles) o matriz
// comparativa.  Datos en window.VESTA_DB.features[isa] = {table:[nombre...],
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

    const sub = $('subline');
    if (sub) sub.innerHTML = T('feat.sub', {
        cpus: CPUS.length.toLocaleString(LOC), feats: TABLE.length,
        isa: (DB.labels && DB.labels[ISA]) || ISA
    });
    // nombre de modelo de scheduling legible (sin el sufijo interno).
    function sched(s) {
        s = (s || '').replace(/Model$/, '');
        return (!s || s === 'NoSched') ? '—' : s;
    }

    // color estable por feature (agrupa visualmente las relacionadas).
    function hue(s) { let h = 0; for (const c of s) h = (h * 31 + c.charCodeAt(0)) >>> 0; return h % 360; }
    function chip(name, on, hl) {
        const h = hue(name);
        const cls = 'fchip' + (on ? '' : ' off') + (hl ? ' hl' : '');
        const st = on ? 'style="--fh:' + h + '"' : '';
        return '<span class="' + cls + '" ' + st + '>' + esc(name) + '</span>';
    }

    const wrap = $('matrix-wrap'), q = $('q'), count = $('count'), modeSel = $('mode');
    const cpuSets = CPUS.map(c => new Set(c[2]));

    // devuelve {rows, cols, matchedFeat} segun la busqueda.
    function filterSets(query) {
        const featMatch = TABLE.map((f, i) => i).filter(i => TABLE[i].toLowerCase().includes(query));
        const cpuMatch = CPUS.map((c, i) => i).filter(i => CPUS[i][0].toLowerCase().includes(query));
        const matchedFeat = new Set(query ? featMatch : []);
        let rows, cols;
        if (!query) { rows = CPUS.map((c, i) => i); cols = TABLE.map((f, i) => i); }
        else if (featMatch.length && !cpuMatch.length) { rows = CPUS.map((c, i) => i); cols = featMatch; }
        else if (cpuMatch.length && !featMatch.length) { rows = cpuMatch; cols = TABLE.map((f, i) => i); }
        else if (featMatch.length && cpuMatch.length) { rows = cpuMatch; cols = featMatch; }
        else { rows = []; cols = []; }
        return { rows, cols, matchedFeat };
    }

    function renderList(query) {
        const { rows, matchedFeat } = filterSets(query);
        // al buscar una feature, muestra solo las CPU que la tienen.
        const only = query && matchedFeat.size ? rows.filter(ri =>
            [...matchedFeat].some(fi => cpuSets[ri].has(fi))) : rows;
        count.textContent = only.length + ' CPU';
        if (!only.length) { wrap.innerHTML = '<p class="dim" style="padding:1rem">' + T('feat.none') + '</p>'; return; }
        const out = [];
        for (const ri of only) {
            const c = CPUS[ri];
            const feats = c[2].slice().sort((a, b) => TABLE[a].localeCompare(TABLE[b]));
            const chips = feats.map(fi => chip(TABLE[fi], true, matchedFeat.has(fi))).join('');
            out.push('<div class="fcard"><div class="fcard-h"><span class="fcpu">' + esc(c[0]) +
                '</span><span class="fsched">' + esc(sched(c[1])) + '</span>' +
                '<span class="fcnt">' + c[2].length + '</span></div>' +
                '<div class="fchips">' + chips + '</div></div>');
        }
        wrap.innerHTML = out.join('');
    }

    function renderMatrix(query) {
        const { rows, cols, matchedFeat } = filterSets(query);
        count.textContent = rows.length + ' CPU x ' + cols.length + ' features';
        if (!rows.length || !cols.length) { wrap.innerHTML = '<p class="dim" style="padding:1rem">' + T('feat.none') + '</p>'; return; }
        const out = ['<table class="feat-matrix"><thead><tr>',
            '<th class="cpu-h">' + T('feat.cpu') + '</th><th class="sch-h">' + T('feat.sched') +
            '</th><th class="cnt-h">' + T('feat.count') + '</th>'];
        for (const ci of cols) out.push('<th class="fh"' + (matchedFeat.has(ci) ? ' data-hl="1"' : '') +
            '><div><span style="color:hsl(' + hue(TABLE[ci]) + ' 65% 45%)">' + esc(TABLE[ci]) + '</span></div></th>');
        out.push('</tr></thead><tbody>');
        for (const ri of rows) {
            const c = CPUS[ri], set = cpuSets[ri];
            out.push('<tr><td class="cpu-c">' + esc(c[0]) + '</td><td class="sch-c">' + esc(sched(c[1])) +
                '</td><td class="cnt-c">' + c[2].length + '</td>');
            for (const ci of cols) out.push(set.has(ci) ?
                '<td class="yes" style="background:hsl(' + hue(TABLE[ci]) + ' 60% 50% / .16)">&check;</td>' : '<td></td>');
            out.push('</tr>');
        }
        out.push('</tbody></table>');
        wrap.innerHTML = out.join('');
    }

    function render() {
        const query = q.value.trim().toLowerCase();
        if (modeSel && modeSel.value === 'matrix') { wrap.className = 'wrap'; renderMatrix(query); }
        else { wrap.className = ''; renderList(query); }
    }

    q.addEventListener('input', render);
    if (modeSel) modeSel.addEventListener('change', render);
    render();
})();
