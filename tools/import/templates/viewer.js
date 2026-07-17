// Pagina de instrucciones: tabla paginada con busqueda, filtro por iclass,
// selector de microarquitectura y filas expandibles.  Datos en window.VESTA_DB
// (assets/db.js): forms + arches[{name, family, classes, map}].
// forms[i] = [id,uid,iclass,ext,opcode,enc,rmask,wmask,memflags,overlay,
//             operandos,string,summary,category,checksum,url]
(function () {
    const DB = window.VESTA_DB;
    const F = DB.forms, AR = DB.arches, PAGE = 100;
    let arch = 0, page = 0, sortK = 0, sortDir = 1, filt = F;
    const $ = id => document.getElementById(id);
    const esc = s => (s + '').replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

    function cost(r, a) { const cid = AR[a].map[r[0]]; return cid < 0 ? null : AR[a].classes[cid]; }

    const KIND = { R: 'resultado', A: 'direccion', F: 'flags', M: 'memoria' };
    function chips(s, cls) {
        return s ? s.split(',').map(x => '<span class="chip' + (cls ? ' ' + cls : '') + '">' + esc(x) + '</span>').join('')
            : '<span class="dim">&mdash;</span>';
    }
    function opChips(s) {
        if (!s) return '<span class="dim">&mdash;</span>';
        return s.split(', ').map(o => {
            const m = o.match(/^op(\d+) (\w+?)(\d+) ([rwis]*)$/);
            if (!m) return '<span class="chip">' + esc(o) + '</span>';
            let mk = ''; const rw = m[4];
            if (rw.includes('r')) mk += '<span class="rd">lee</span> ';
            if (rw.includes('w')) mk += '<span class="wr">escr</span> ';
            if (rw.includes('i')) mk += '<span class="dim">impl</span> ';
            return '<span class="op"><b>' + m[2] + m[3] + '</b> ' + mk.trim() + '</span>';
        }).join(' ');
    }
    function opsFromMask(h) {
        const n = parseInt(h, 16) || 0, o = [];
        for (let i = 0; i < 8; i++) if (n & (1 << i)) o.push('op' + i);
        return o.length ? o.join(', ') : '&mdash;';
    }
    function maskBin(h) {
        const n = parseInt(h, 16) || 0;
        return n ? '0b' + n.toString(2).padStart(4, '0') : '0b0000';
    }
    function maxLat(s) {
        if (!s) return '';
        let mx = 0;
        for (const m of s.matchAll(/([0-9.]+)[RAFM]/g)) { const v = parseFloat(m[1]); if (v > mx) mx = v; }
        return mx ? mx.toFixed(2) : '';
    }
    function latHuman(s) {
        if (!s) return '<span class="dim">&mdash;</span>';
        return s.split(', ').map(e => {
            const m = e.match(/^op(\d+)->op(\d+) ([0-9.]+)([RAFM])(\(ub\))?$/);
            if (!m) return esc(e);
            return 'op' + m[1] + ' &rarr; op' + m[2] + ': <b>' + m[3] + '</b> ciclos ' +
                '<span class="dim">(' + KIND[m[4]] + (m[5] ? ', cota superior' : '') + ')</span>';
        }).join('<br>');
    }
    // Que es cada puerto.  AMD nombra por FUNCION (traducible con exactitud);
    // Intel agrupa puertos numerados "pNNNN" (una µop va a UNO del grupo).
    const AMD_PORT = {
        LD: 'carga (load)', ALU: 'ALU entera', AGU: 'generacion de direccion (AGU)',
        STA: 'direccion de almacenamiento (store address)', STD: 'dato de almacenamiento (store data)',
        JMP: 'salto / branch', BR: 'salto / branch', MUL: 'multiplicacion entera', DIV: 'division',
        SHIFT: 'desplazamientos', SLOW: 'ruta lenta (operaciones complejas / microcodigo)',
        INT_OTHER: 'otras operaciones enteras', UNKNOWN: 'puerto no identificado por la fuente',
        FP0: 'unidad FP / vectorial 0', FP1: 'unidad FP / vectorial 1',
        FP2: 'unidad FP / vectorial 2', FP3: 'unidad FP / vectorial 3',
    };
    function portDesc(name) {
        const up = name.toUpperCase();
        if (AMD_PORT[up]) return name + ': ' + AMD_PORT[up];
        const m = name.match(/^p([0-9]+)([a-z]*)$/i);
        if (m) {
            const ports = m[1].split('').map(d => 'p' + d).join(', ');
            const clus = m[2] ? ' (cluster ' + m[2].toUpperCase() + ')' : '';
            return 'la µop se despacha a UNO de los puertos ' + ports + clus +
                '. Mas puertos disponibles = mas paralelismo.';
        }
        return name + ': unidad de ejecucion';
    }
    // color estable por grupo de puertos (el cerebro identifica los dominantes).
    function portHue(name) { let h = 0; for (const c of name) h = (h * 31 + c.charCodeAt(0)) >>> 0; return h % 360; }
    function portsHuman(s) {
        if (!s) return '<span class="dim">&mdash;</span>';
        return s.split(' ').map(t => {
            const m = t.match(/^([0-9.]+)x(.+)$/);
            if (!m) return esc(t);
            const n = parseFloat(m[1]), hue = portHue(m[2]);
            return '<b>' + m[1] + '</b> µop' + (n !== 1 ? 's' : '') + ' &rarr; ' +
                '<span class="pport" data-tip="' + esc(portDesc(m[2])) + '" style="background:hsl(' + hue + ' 60% 50% / .18);color:hsl(' + hue + ' 65% 42%)">' +
                esc(m[2]) + '</span>';
        }).join('<br>');
    }

    const icSel = $('ic'), arSel = $('ar');
    icSel.innerHTML = '<option value="">todos</option>' +
        [...new Set(F.map(r => r[2]))].sort().map(c => '<option>' + esc(c) + '</option>').join('');
    AR.forEach((a, i) => {
        const o = document.createElement('option');
        o.value = i; o.textContent = a.name;
        if (a.name === 'intel-skylake') o.selected = true;
        arSel.appendChild(o);
    });
    arch = arSel.selectedIndex < 0 ? 0 : arSel.selectedIndex;

    function apply() {
        const q = $('q').value.trim().toLowerCase(), ic = $('ic').value;
        filt = F.filter(r => {
            if (ic && r[2] !== ic) return false;
            if (!q) return true;
            return (r[1] + ' ' + r[2] + ' ' + r[4] + ' ' + r[5]).toLowerCase().includes(q);
        });
        filt.sort((a, b) => {
            let x = a[sortK], y = b[sortK];
            if (sortK === 0) { x = +x; y = +y; }
            else { x = (x + '').toLowerCase(); y = (y + '').toLowerCase(); }
            return x < y ? -sortDir : x > y ? sortDir : 0;
        });
        page = 0; render();
    }

    function render() {
        const tb = $('tb'); tb.innerHTML = '';
        const tot = filt.length, pages = Math.max(1, Math.ceil(tot / PAGE));
        if (page >= pages) page = pages - 1;
        for (const r of filt.slice(page * PAGE, page * PAGE + PAGE)) {
            const t = cost(r, arch), dot = '<span class="dim">&middot;</span>';
            const tip = (r[11] || '') + (r[12] ? ' — ' + r[12] : '');
            const tr = document.createElement('tr'); tr.className = 'main';
            tr.innerHTML =
                '<td class="n">' + r[0] + '</td>' +
                '<td class="forma mono"' + (tip ? ' title="' + esc(tip) + '"' : '') + '>' + esc(r[1]) + '</td>' +
                '<td class="nowrap iclink" title="ver todas las formas de ' + esc(r[2]) + '">' + esc(r[2]) + '</td>' +
                '<td class="nowrap mono">' + esc(r[4]) + '<span class="dim"> ' + esc(r[3]) + '</span></td>' +
                '<td class="enc">' + chips(r[5]) + '</td>' +
                '<td class="ops">' + opChips(r[10]) + '</td>' +
                '<td class="nowrap">' + chips(r[9], 'ov') + '</td>' +
                '<td class="n mono">' + (t ? esc(t[0]) : dot) + '</td>' +
                '<td class="n">' + (t ? t[1] : dot) + '</td>' +
                '<td class="n mono">' + (t ? (maxLat(t[4]) || dot) : dot) + '</td>';
            tr.querySelector('.iclink').onclick = ev => { ev.stopPropagation(); icSel.value = r[2]; apply(); };
            tr.onclick = () => toggle(tr, r);
            tb.appendChild(tr);
        }
        $('count').textContent = tot.toLocaleString('es') + ' formas' +
            (tot !== F.length ? ' (de ' + F.length.toLocaleString('es') + ')' : '');
        $('pgi').textContent = 'pagina ' + (page + 1) + ' / ' + pages;
        $('first').disabled = $('prev').disabled = (page <= 0);
        $('last').disabled = $('next').disabled = (page >= pages - 1);
    }

    function structKey(r) {
        let s = '<details class="skey"><summary>Identidad estructural (form_key)</summary><div class="mono">';
        s += 'iclass    = ' + esc(r[2]) + '<br>extension = ' + esc(r[3]) +
            '<br>opcode    = ' + esc(r[4]) + '<br><br>encoding:<br>';
        s += r[5] ? r[5].split(',').map(x => '&nbsp;&nbsp;' + esc(x)).join('<br>') : '&nbsp;&nbsp;(ninguno)';
        s += '<br><br>operandos:<br>';
        s += r[10] ? r[10].split(', ').map(o => '&nbsp;&nbsp;' + esc(o)).join('<br>') : '&nbsp;&nbsp;(ninguno)';
        s += '</div><div class="dim" style="margin-top:6px">Los conjuntos de registros permitidos ' +
            'estan en <span class="mono">x86.vxisa</span> (se omiten aqui por tamano).</div></details>';
        return s;
    }

    function toggle(tr, r) {
        if (tr.nextSibling && tr.nextSibling.classList && tr.nextSibling.classList.contains('detail')) {
            tr.nextSibling.remove(); return;
        }
        const d = document.createElement('tr'); d.className = 'detail';
        let h = '<td colspan="10">';
        h += '<div class="idline mono">FormID <b>' + r[0] + '</b>' +
            ' &middot; checksum ' + esc(r[14]) +
            (r[15] ? ' &middot; <a target="_blank" rel="noopener" href="' + esc(r[15]) + '">ver en uops.info &nearr;</a>' : '') +
            '</div>';
        h += '<h4>que hace</h4><div>' +
            (r[12] ? '<b>' + esc(r[12]) + '</b>' : '<span class="dim">(sin descripcion)</span>') +
            (r[11] ? ' &middot; <span class="mono dim">' + esc(r[11]) + '</span>' : '') +
            (r[13] ? ' &middot; ' + chips(r[13]) : '') + '</div>';
        h += '<h4>operandos</h4><div class="oplist">' + opChips(r[10]) + '</div>';
        h += '<h4>efectos</h4><div>' +
            '<span class="rd">lee</span> operandos: ' + opsFromMask(r[6]) +
            ' <span class="dim mono">(' + esc(r[6]) + ' = ' + maskBin(r[6]) + ')</span><br>' +
            '<span class="wr">escribe</span> operandos: ' + opsFromMask(r[7]) +
            ' <span class="dim mono">(' + esc(r[7]) + ' = ' + maskBin(r[7]) + ')</span><br>' +
            ((r[8] & 4) ? '<span class="wr">escribe flags</span> ' : '') +
            ((r[8] & 8) ? '<span class="rd">lee flags</span> ' : '') +
            ((r[8] & 1) ? 'accede a memoria' : (!(r[8] & 12) ? '<span class="dim">no toca flags ni memoria</span>' : '')) +
            '</div>';
        h += structKey(r);
        h += '<h4>coste por microarquitectura</h4><table><thead><tr>' +
            '<th>microarq.</th>' +
            '<th title="throughput reciproco: ciclos por instruccion (menor = mas rapido)">recip_tp</th>' +
            '<th title="micro-operaciones que genera">uops</th>' +
            '<th title="microcoded = usa microcodigo; macro_fusible = fusionable con un salto">notas</th>' +
            '<th title="latencia del divisor (DIV/IDIV)">div_cycles</th>' +
            '<th title="latencia por cada camino operando-fuente a operando-destino">latencias</th>' +
            '<th title="reparto de micro-ops entre los puertos de ejecucion">puertos</th>' +
            '</tr></thead><tbody>';
        AR.forEach((a, i) => {
            const em = '<span class="dim">&mdash;</span>', t = cost(r, i);
            if (!t) { h += '<tr><td>' + esc(a.name) + '</td><td colspan="6" class="dim">sin dato en esta microarq.</td></tr>'; return; }
            const notes = [];
            if (t[2] & 1) notes.push('microcoded');
            if (t[2] & 2) notes.push('macro_fusible');
            h += '<tr><td>' + esc(a.name) + '</td><td class="mono">' + esc(t[0]) + '</td><td>' + t[1] +
                '</td><td>' + (notes.join(', ') || em) +
                '</td><td>' + (t[3] !== '-1.00' ? esc(t[3]) : em) +
                '</td><td>' + latHuman(t[4]) +
                '</td><td>' + portsHuman(t[5]) + '</td></tr>';
        });
        h += '</tbody></table></td>';
        d.innerHTML = h; tr.after(d);
    }

    document.querySelectorAll('th[data-k]').forEach(th => {
        th.onclick = () => {
            const k = +th.dataset.k;
            if (sortK === k) sortDir = -sortDir; else { sortK = k; sortDir = 1; }
            apply();
        };
    });
    $('q').addEventListener('input', () => { clearTimeout(window._t); window._t = setTimeout(apply, 140); });
    $('ic').onchange = apply;
    arSel.onchange = () => { arch = +arSel.value; render(); };
    $('first').onclick = () => { page = 0; render(); };
    $('prev').onclick = () => { page--; render(); };
    $('next').onclick = () => { page++; render(); };
    $('last').onclick = () => { page = 1e9; render(); };
    apply();
})();
