// Visor de la base de datos de instrucciones (datos embebidos en #data).
// Renderiza una tabla paginada con busqueda, filtro por iclass, selector de
// microarquitectura y filas expandibles con el detalle completo.
const D = JSON.parse(document.getElementById('data').textContent);
const R = D.rows, A = D.arches, PAGE = 100;
let arch = 0, page = 0, sortK = 0, sortDir = 1, filt = R;
const $ = id => document.getElementById(id);
const arSel = $('ar');

A.forEach((n, i) => {
    const o = document.createElement('option');
    o.value = i; o.textContent = n;
    if (n === 'intel-skylake') o.selected = true;
    arSel.appendChild(o);
});
arch = arSel.selectedIndex < 0 ? 0 : arSel.selectedIndex;

const esc = s => (s + '').replace(/[&<>]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c]));

// "isa_set=I386,eosz=1" -> chips; vacio -> raya.
function chips(s, cls) {
    return s
        ? s.split(',').map(x => '<span class="chip' + (cls ? ' ' + cls : '') + '">' + esc(x) + '</span>').join('')
        : '<span class="dim">&mdash;</span>';
}

// "op0 reg64 rw, op1 mem64 r" -> chips por operando con lee/escribe coloreado.
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

// bitmask hex -> "op0, op1".
function opsFromMask(h) {
    const n = parseInt(h, 16) || 0, o = [];
    for (let i = 0; i < 8; i++) if (n & (1 << i)) o.push('op' + i);
    return o.length ? o.join(', ') : '&mdash;';
}

// mayor numero de ciclos que aparece en la cadena de latencias.
function maxLat(s) {
    if (!s) return '';
    let mx = 0;
    for (const m of s.matchAll(/([0-9.]+)[RAFM]/g)) {
        const v = parseFloat(m[1]); if (v > mx) mx = v;
    }
    return mx ? mx.toFixed(2) : '';
}

function apply() {
    const q = $('q').value.trim().toLowerCase(), ic = $('ic').value;
    filt = R.filter(r => {
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
        const t = r[11][arch], dot = '<span class="dim">&middot;</span>';
        const tr = document.createElement('tr'); tr.className = 'main';
        tr.innerHTML =
            '<td class="n">' + r[0] + '</td>' +
            '<td class="forma mono">' + esc(r[1]) + '</td>' +
            '<td class="nowrap">' + esc(r[2]) + '</td>' +
            '<td class="nowrap mono">' + esc(r[4]) + '<span class="dim"> ' + esc(r[3]) + '</span></td>' +
            '<td class="enc">' + chips(r[5]) + '</td>' +
            '<td class="ops">' + opChips(r[10]) + '</td>' +
            '<td class="nowrap">' + chips(r[9], 'ov') + '</td>' +
            '<td class="n mono">' + (t ? esc(t[0]) : dot) + '</td>' +
            '<td class="n">' + (t ? t[1] : dot) + '</td>' +
            '<td class="n mono">' + (t ? (maxLat(t[4]) || dot) : dot) + '</td>';
        tr.onclick = () => toggle(tr, r);
        tb.appendChild(tr);
    }
    $('count').textContent = tot.toLocaleString('es') + ' formas' +
        (tot !== R.length ? ' (de ' + R.length.toLocaleString('es') + ')' : '');
    $('pgi').textContent = 'pagina ' + (page + 1) + ' / ' + pages;
    $('first').disabled = $('prev').disabled = (page <= 0);
    $('last').disabled = $('next').disabled = (page >= pages - 1);
}

function toggle(tr, r) {
    if (tr.nextSibling && tr.nextSibling.classList && tr.nextSibling.classList.contains('detail')) {
        tr.nextSibling.remove(); return;
    }
    const d = document.createElement('tr'); d.className = 'detail';
    let h = '<td colspan="10">';
    h += '<h4>identidad</h4><div class="mono dim">iclass=' + esc(r[2]) + ' ext=' + esc(r[3]) +
        ' opcode=' + esc(r[4]) + ' &middot; encoding: ' + (r[5] ? esc(r[5]) : '(ninguno)') + '</div>';
    h += '<h4>operandos</h4><div class="oplist">' + opChips(r[10]) + '</div>';
    h += '<h4>efectos</h4><div><span class="rd">lee</span>: ' + opsFromMask(r[6]) +
        ' &middot; <span class="wr">escribe</span>: ' + opsFromMask(r[7]) + ' &middot; ' +
        ((r[8] & 4) ? '<span class="wr">escribe flags</span> ' : '') +
        ((r[8] & 8) ? '<span class="rd">lee flags</span> ' : '') +
        ((r[8] & 1) ? 'accede a memoria' : '') + '</div>';
    h += '<h4>coste por microarquitectura</h4><table><thead><tr><th>microarq.</th>' +
        '<th>recip_tp</th><th>uops</th><th>notas</th><th>div_cycles</th>' +
        '<th>latencias (op&rarr;op ciclos tipo)</th><th>puertos</th></tr></thead><tbody>';
    r[11].forEach((t, i) => {
        const em = '<span class="dim">&mdash;</span>';
        if (!t) { h += '<tr><td>' + esc(A[i]) + '</td><td colspan="6" class="dim">(sin dato)</td></tr>'; return; }
        const notes = [];
        if (t[2] & 1) notes.push('microcoded');
        if (t[2] & 2) notes.push('macro_fusible');
        h += '<tr><td>' + esc(A[i]) + '</td><td class="mono">' + esc(t[0]) + '</td><td>' + t[1] +
            '</td><td>' + (notes.join(', ') || em) +
            '</td><td>' + (t[3] !== '-1.00' ? esc(t[3]) : em) +
            '</td><td class="mono">' + (t[4] ? esc(t[4]) : em) +
            '</td><td class="mono">' + (t[5] ? esc(t[5]) : em) + '</td></tr>';
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
