// Analizador de asm: empareja cada linea con su forma en window.VESTA_DB y hace
// un analisis del bloque -- uops, coste por throughput y camino critico de
// dependencias (registros, flags, memoria) -- para la microarquitectura elegida.
(function () {
    const DB = window.VESTA_DB, F = DB.forms, AR = DB.arches;
    const $ = id => document.getElementById(id);
    const esc = s => (s + '').replace(/[&<>]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c]));

    const byClass = new Map();
    for (const r of F) { const k = r[2].toUpperCase(); (byClass.get(k) || byClass.set(k, []).get(k)).push(r); }

    const PTRW = { byte: 8, word: 16, dword: 32, qword: 64, xmmword: 128, ymmword: 256, zmmword: 512 };
    const GP = {
        rax: 'A', eax: 'A', ax: 'A', al: 'A', ah: 'A', rbx: 'B', ebx: 'B', bx: 'B', bl: 'B', bh: 'B',
        rcx: 'C', ecx: 'C', cx: 'C', cl: 'C', ch: 'C', rdx: 'D', edx: 'D', dx: 'D', dl: 'D', dh: 'D',
        rsi: 'SI', esi: 'SI', si: 'SI', sil: 'SI', rdi: 'DI', edi: 'DI', di: 'DI', dil: 'DI',
        rbp: 'BP', ebp: 'BP', bp: 'BP', bpl: 'BP', rsp: 'SP', esp: 'SP', sp: 'SP', spl: 'SP'
    };
    // registro fisico canonico (para rastrear dependencias entre anchos/alias).
    function canon(reg) {
        reg = reg.toLowerCase();
        if (GP[reg]) return GP[reg];
        let m = reg.match(/^r(\d+)[dwb]?$/); if (m) return 'R' + m[1];
        m = reg.match(/^[xyz]mm(\d+)$/); if (m) return 'V' + m[1];
        m = reg.match(/^k([0-7])$/); if (m) return 'K' + m[1];
        return reg;
    }
    function regWidth(r) {
        r = r.toLowerCase();
        if (/^r(ax|bx|cx|dx|si|di|bp|sp|8|9|1[0-5])$/.test(r)) return 64;
        if (/^(e(ax|bx|cx|dx|si|di|bp|sp)|r(8|9|1[0-5])d)$/.test(r)) return 32;
        if (/^((ax|bx|cx|dx|si|di|bp|sp)|r(8|9|1[0-5])w)$/.test(r)) return 16;
        if (/^((a|b|c|d)(l|h)|(si|di|bp|sp)l|r(8|9|1[0-5])b)$/.test(r)) return 8;
        if (/^xmm\d+$/.test(r)) return 128;
        if (/^ymm\d+$/.test(r)) return 256;
        if (/^zmm\d+$/.test(r)) return 512;
        if (/^k[0-7]$/.test(r)) return 64;
        return 0;
    }
    function classify(tok) {
        tok = tok.trim();
        if (/\[/.test(tok)) {
            const p = tok.match(/^(byte|word|dword|qword|xmmword|ymmword|zmmword)\s+ptr\b/i);
            const regs = [];
            const inner = (tok.match(/\[([^\]]*)\]/) || [, ''])[1];
            for (const m of inner.matchAll(/[a-z][a-z0-9]*/gi)) if (regWidth(m[0])) regs.push(m[0]);
            return { kind: 'mem', width: p ? PTRW[p[1].toLowerCase()] : 0, addr: regs, raw: tok };
        }
        if (/^[-+]?(0x[0-9a-f]+|0b[01]+|\d+)$/i.test(tok)) return { kind: 'imm', width: 0, raw: tok };
        const w = regWidth(tok);
        if (w) return { kind: 'reg', width: w, raw: tok };
        return { kind: '?', width: 0, raw: tok };
    }
    function splitOps(s) {
        const out = []; let depth = 0, cur = '';
        for (const c of s) {
            if (c === '[') depth++; else if (c === ']') depth--;
            if (c === ',' && depth === 0) { out.push(cur); cur = ''; } else cur += c;
        }
        if (cur.trim()) out.push(cur);
        return out;
    }
    const PREFIX = /^(lock|rep|repe|repz|repne|repnz)\s+/i;
    function parseLine(line) {
        line = line.replace(/[;#].*$/, '').replace(/\/\/.*$/, '').trim();
        if (!line || /:\s*$/.test(line)) return null;
        line = line.replace(PREFIX, '');
        const sp = line.search(/\s/);
        const mn = (sp < 0 ? line : line.slice(0, sp)).toUpperCase();
        const rest = sp < 0 ? '' : line.slice(sp + 1).trim();
        const ops = rest ? splitOps(rest).map(classify) : [];
        // inferir el ancho de un operando de memoria sin prefijo desde otro operando.
        const rw = ops.filter(o => o.kind === 'reg').map(o => o.width);
        for (const o of ops) if (o.kind === 'mem' && !o.width && rw.length) o.width = Math.max(...rw);
        return { mn, ops, text: line };
    }
    function formOps(str) {
        if (!str) return [];
        return str.split(', ').map(o => {
            const m = o.match(/^op\d+ (\w+?)(\d+) ([rwis]*)$/);
            return m ? { kind: m[1], width: +m[2], flags: m[3] } : null;
        }).filter(o => o && !/[is]/.test(o.flags));
    }
    function score(user, fops) {
        if (user.length !== fops.length) return -1;
        let s = 0;
        for (let i = 0; i < fops.length; i++) {
            const u = user[i], f = fops[i];
            if (u.kind !== '?' && u.kind !== f.kind) return -1;
            if (u.width && f.width) { if (u.width !== f.width) return -1; s += 2; } else s += 1;
        }
        return s;
    }
    function match(p) {
        const cands = byClass.get(p.mn);
        if (!cands) return null;
        let best = null, bestS = -1;
        for (const r of cands) {
            const s = score(p.ops, formOps(r[10]));
            if (s > bestS) { bestS = s; best = r; }
        }
        return bestS < 0 ? null : best;
    }
    function maxLat(s) {
        if (!s) return 0;
        let mx = 0;
        for (const m of s.matchAll(/([0-9.]+)[RAFM]/g)) { const v = parseFloat(m[1]); if (v > mx) mx = v; }
        return mx;
    }
    function cost(r, a) { const cid = AR[a].map[r[0]]; return cid < 0 ? null : AR[a].classes[cid]; }

    // Descripcion de un puerto (tooltip).  AMD por funcion; Intel por grupo.
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
        if (AMD_PORT[name.toUpperCase()]) return name + ': ' + AMD_PORT[name.toUpperCase()];
        const m = name.match(/^p([0-9]+)([a-z]*)$/i);
        if (m) return 'la µop se despacha a UNO de los puertos ' +
            m[1].split('').map(d => 'p' + d).join(', ') + (m[2] ? ' (cluster ' + m[2].toUpperCase() + ')' : '');
        return name + ': unidad de ejecucion';
    }
    function portsInline(s) {
        if (!s) return '<span class="dim">&mdash;</span>';
        return s.split(' ').map(t => {
            const m = t.match(/^([0-9.]+)x(.+)$/);
            if (!m) return esc(t);
            return m[1] + 'x<span class="pport" data-tip="' + esc(portDesc(m[2])) + '">' + esc(m[2]) + '</span>';
        }).join(' ');
    }

    // recursos que lee/escribe una instruccion (registros + FLAGS + MEM).
    function resources(p, r) {
        const rd = new Set(), wr = new Set();
        const rmask = parseInt(r[6], 16) || 0, wmask = parseInt(r[7], 16) || 0, mf = r[8];
        p.ops.forEach((o, i) => {
            if (o.kind === 'reg') {
                if (rmask & (1 << i)) rd.add(canon(o.raw));
                if (wmask & (1 << i)) wr.add(canon(o.raw));
            } else if (o.kind === 'mem') {
                (o.addr || []).forEach(a => rd.add(canon(a)));   // registros de la direccion
                if (rmask & (1 << i)) rd.add('MEM');              // carga
                if (wmask & (1 << i)) wr.add('MEM');              // almacenamiento
            }
        });
        if (mf & 8) rd.add('FLAGS');
        if (mf & 4) wr.add('FLAGS');
        return { rd, wr };
    }

    const arSel = $('ar');
    AR.forEach((a, i) => {
        const o = document.createElement('option');
        o.value = i; o.textContent = a.name;
        if (a.name === 'intel-skylake') o.selected = true;
        arSel.appendChild(o);
    });

    function run() {
        const a = +arSel.value || 0;
        const items = [];
        for (const line of $('src').value.split('\n')) {
            const p = parseLine(line);
            if (p) items.push({ p, r: match(p) });
        }
        // pase 1: coste + recursos + presion de puertos.
        let nOk = 0, nMiss = 0, sumU = 0, tp = 0;
        const portPress = new Map();
        for (const it of items) {
            if (!it.r) { nMiss++; continue; }
            nOk++;
            const t = cost(it.r, a);
            it.t = t; it.lat = t ? maxLat(t[4]) : 0; it.res = resources(it.p, it.r);
            if (t) {
                sumU += t[1]; tp += parseFloat(t[0]) || 0;
                if (t[5]) for (const tok of t[5].split(' ')) {
                    const m = tok.match(/^([0-9.]+)x(.+)$/);
                    if (m) portPress.set(m[2], (portPress.get(m[2]) || 0) + parseFloat(m[1]));
                }
            }
        }
        // pase 2: camino critico (grafo de dependencias reg/flags/memoria).
        const lastW = {}, finish = [], predOf = [];
        let critical = 0, endIdx = -1;
        items.forEach((it, i) => {
            if (!it.r) { finish[i] = 0; predOf[i] = -1; return; }
            let ready = 0, pred = -1;
            it.res.rd.forEach(s => { const w = lastW[s]; if (w != null && finish[w] >= ready) { ready = finish[w]; pred = w; } });
            finish[i] = ready + it.lat; predOf[i] = pred;
            it.res.wr.forEach(s => { lastW[s] = i; });
            if (finish[i] > critical) { critical = finish[i]; endIdx = i; }
        });
        const critSet = new Set();
        for (let i = endIdx; i >= 0; i = predOf[i]) critSet.add(i);
        // filas (marcando el camino critico).
        let rows = '';
        items.forEach((it, i) => {
            const p = it.p, r = it.r;
            if (!r) {
                rows += '<tr><td class="mono">' + esc(p.text) + '</td>' +
                    '<td class="miss" colspan="6">forma no encontrada' +
                    (byClass.has(p.mn) ? ' (operandos no encajan)' : ' (mnemonico desconocido)') + '</td></tr>';
                return;
            }
            const t = it.t, crit = critSet.has(i);
            rows += '<tr' + (crit ? ' class="crit" data-tip="en el camino critico: esta cadena de dependencias fija el limite de latencia"' : '') + '>' +
                '<td class="mono">' + (crit ? '<span class="critdot">&#9679;</span> ' : '') + esc(p.text) + '</td>' +
                '<td class="mono">' + esc(r[1]) + (r[12] ? ' <span class="dim">' + esc(r[12]) + '</span>' : '') + '</td>' +
                '<td class="mono">' + esc(r[2]) + '</td>' +
                '<td class="n">' + (t ? t[1] : '&middot;') + '</td>' +
                '<td class="n mono">' + (t ? esc(t[0]) : '&middot;') + '</td>' +
                '<td class="n mono">' + (t ? (it.lat ? it.lat.toFixed(2) : '<span class="dim">0</span>') : '<span class="dim">sin dato</span>') + '</td>' +
                '<td class="mono">' + (t ? portsInline(t[5]) : '<span class="dim">&mdash;</span>') + '</td></tr>';
        });
        const est = Math.max(tp, critical), latBound = critical >= tp;
        let topPort = '', topVal = 0;
        portPress.forEach((v, k) => { if (v > topVal) { topVal = v; topPort = k; } });
        const bneck = latBound
            ? 'latencia &mdash; el camino critico (filas <span class="critdot">&#9679;</span>) fija <b>' + critical.toFixed(2) + '</b> ciclos'
            : 'throughput &mdash; limitan los puertos; grupo mas cargado: <b>' + esc(topPort) + '</b> (' + topVal.toFixed(2) + ' µops)';
        $('res').innerHTML = rows ?
            '<div class="wrap"><table><thead><tr><th>instruccion</th><th>forma</th><th>iclass</th>' +
            '<th class="n">uops</th><th class="n">recip_tp</th><th class="n">latencia</th><th>puertos</th></tr></thead>' +
            '<tbody>' + rows + '</tbody></table></div>' +
            '<div class="an-sum"><b>Analisis del bloque (' + esc(AR[a].name) + ')</b> &mdash; ' +
            nOk + ' emparejadas' + (nMiss ? ', ' + nMiss + ' sin forma' : '') + '.' +
            '<table class="sumt">' +
            '<tr><td>micro-operaciones (uops)</td><td class="n">' + sumU + '</td></tr>' +
            '<tr' + (!latBound ? ' class="est"' : '') + '><td>coste por <b>throughput</b> (&Sigma; recip_tp)</td><td class="n">' + tp.toFixed(2) + ' ciclos</td></tr>' +
            '<tr' + (latBound ? ' class="est"' : '') + '><td>coste por <b>latencia</b> (camino critico)</td><td class="n">' + critical.toFixed(2) + ' ciclos</td></tr>' +
            '<tr class="est"><td>estimacion del bloque = max(throughput, latencia)</td><td class="n">' + est.toFixed(2) + ' ciclos</td></tr>' +
            '<tr><td>cuello de botella</td><td>' + bneck + '</td></tr></table></div>'
            : '<p class="hint">Sin instrucciones que analizar.</p>';
        $('summary').textContent = nOk + ' emparejadas' + (nMiss ? ' / ' + nMiss + ' sin forma' : '');
    }

    // --- resaltado de sintaxis del editor (overlay <pre> tras el <textarea>) ---
    const KW = /^(byte|word|dword|qword|xmmword|ymmword|zmmword|ptr)$/i;
    const isReg = w => regWidth(w) > 0;
    function hlMem(s) {
        return esc(s).replace(/[a-z][a-z0-9]*/gi, w => isReg(w) ? '<span class="c-reg">' + w + '</span>' : w);
    }
    function hlLine(line) {
        const cm = line.search(/([;#]|\/\/)/);
        let code = line, tail = '';
        if (cm >= 0) { code = line.slice(0, cm); tail = '<span class="c-com">' + esc(line.slice(cm)) + '</span>'; }
        let out = '', first = true, m;
        const re = /(\s+)|(\[[^\]]*\])|([A-Za-z_.][\w.]*)|([-+]?0x[0-9a-fA-F]+|[-+]?\d+)|(.)/g;
        while ((m = re.exec(code))) {
            if (m[1]) out += esc(m[1]);
            else if (m[2]) { out += hlMem(m[2]); first = false; }
            else if (m[3]) {
                if (first) { out += '<span class="c-mn">' + esc(m[3]) + '</span>'; first = false; }
                else if (isReg(m[3])) out += '<span class="c-reg">' + esc(m[3]) + '</span>';
                else if (KW.test(m[3])) out += '<span class="c-kw">' + esc(m[3]) + '</span>';
                else out += esc(m[3]);
            } else if (m[4]) { out += '<span class="c-num">' + esc(m[4]) + '</span>'; first = false; }
            else out += esc(m[5]);
        }
        return out + tail;
    }
    function syncScroll() { const h = $('hl'), s = $('src'); h.scrollTop = s.scrollTop; h.scrollLeft = s.scrollLeft; }
    function syncHl() { $('hl').innerHTML = $('src').value.split('\n').map(hlLine).join('\n') + '\n'; syncScroll(); }

    $('run').onclick = run;
    arSel.onchange = run;
    $('src').addEventListener('input', syncHl);
    $('src').addEventListener('scroll', syncScroll);
    $('src').addEventListener('keydown', e => { if ((e.ctrlKey || e.metaKey) && e.key === 'Enter') run(); });
    syncHl();
    run();
})();
