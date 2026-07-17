// Analizador de asm: empareja cada linea con su forma en window.VESTA_DB y hace
// un analisis del bloque -- uops, coste por throughput y camino critico de
// dependencias (registros, flags, memoria) -- para la microarquitectura elegida.
(function () {
    const DB = window.VESTA_DB;
    // ISA activa por ?isa= (recarga al cambiarla).  Multi-ISA + multi-core.
    const ORDER = DB.order || ['x86'];
    const _params = new URLSearchParams(location.search);
    let ISA = _params.get('isa');
    if (!ISA || !DB.isas || !DB.isas[ISA]) ISA = ORDER[0];
    const CUR = (DB.isas && DB.isas[ISA]) || DB;
    const F = CUR.forms, AR = CUR.arches;
    const DEF_ARCH = { x86: 'intel-skylake', arm64: 'neoverse-n2',
                       arm32: 'cortex-a76-a32', riscv: 'sifive-p670' };
    const T = window.t || (k => k);   // traduccion (i18n.js); identidad si no esta
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
    // Latencia del nodo.  Si HAY aristas, su maximo (puede ser 0 = move-elim
    // real).  Si NO hay aristas (CPUID, DIV, serializantes: su coste esta en
    // throughput/uops, no en latencia), se usa como proxy el throughput
    // reciproco / div_cycles -> asi una instruccion cara no aparece como 0.
    function nodeLat(t) {
        if (!t) return 0;
        if (t[4]) return maxLat(t[4]);
        const tp = parseFloat(t[0]) || 0, div = parseFloat(t[3]) || 0;
        return Math.max(tp, div > 0 ? div : 0);
    }
    function cost(r, a) { const cid = AR[a].map[r[0]]; return cid < 0 ? null : AR[a].classes[cid]; }

    const portDesc = window.VESTA_PORTDESC;
    function portsInline(s) {
        if (!s) return '<span class="dim">&mdash;</span>';
        return s.split(' ').map(t => {
            const m = t.match(/^([0-9.]+)x(.+)$/);
            if (!m) return esc(t);
            return m[1] + 'x<span class="pport" data-tip="' + esc(portDesc(m[2])) + '">' + esc(m[2]) + '</span>';
        }).join(' ');
    }
    // Color estable por registro fisico (mismo registro = mismo color -> se ven
    // las dependencias); y coloreado de una linea de asm por operandos.
    function regHue(c) { let h = 0; for (const ch of c) h = (h * 37 + ch.charCodeAt(0)) >>> 0; return h % 360; }
    function colorInstr(text) {
        return esc(text).replace(/\[[^\]]*\]|[A-Za-z_][A-Za-z0-9_]*/g, tok => {
            if (tok[0] === '[') return tok.replace(/[A-Za-z_][A-Za-z0-9_]*/g, w =>
                regWidth(w) ? regSpan(w) : w);
            return regWidth(tok) ? regSpan(tok) : tok;
        });
    }
    function regSpan(w) {
        const c = canon(w), hue = regHue(c);
        return '<span class="rtok" data-tip="' + T('tip.reg', { r: esc(c) }) + '" style="color:hsl(' + hue + ' 70% 42%)">' + esc(w) + '</span>';
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

    const isaSel = $('isa');
    if (isaSel) {
        isaSel.innerHTML = ORDER.map(k =>
            '<option value="' + k + '"' + (k === ISA ? ' selected' : '') + '>' +
            esc((DB.labels && DB.labels[k]) || k) + '</option>').join('');
        isaSel.onchange = () => { location.search = '?isa=' + isaSel.value; };
    }

    const arSel = $('ar');
    const _defArch = DEF_ARCH[ISA];
    AR.forEach((a, i) => {
        const o = document.createElement('option');
        o.value = i; o.textContent = a.name;
        if (a.name === _defArch) o.selected = true;
        arSel.appendChild(o);
    });

    function parseAll(text) {
        const out = [];
        for (const line of text.split('\n')) { const p = parseLine(line); if (p) out.push({ p, r: match(p) }); }
        return out;
    }

    // Analiza un bloque (lista de items) y devuelve su HTML + metricas.
    function analyzeBlock(items, a, label) {
        let nOk = 0, nMiss = 0, sumU = 0, tp = 0;
        const portPress = new Map();
        for (const it of items) {
            if (!it.r) { nMiss++; continue; }
            nOk++;
            const t = cost(it.r, a);
            it.t = t; it.lat = nodeLat(t); it.res = resources(it.p, it.r);
            if (t) {
                sumU += t[1]; tp += parseFloat(t[0]) || 0;
                if (t[5]) for (const tok of t[5].split(' ')) {
                    const m = tok.match(/^([0-9.]+)x(.+)$/);
                    if (m) portPress.set(m[2], (portPress.get(m[2]) || 0) + parseFloat(m[1]));
                }
            }
        }
        const lastW = {}, finish = [], predOf = [], predRes = [];
        let critical = 0, endIdx = -1;
        items.forEach((it, i) => {
            if (!it.r) { finish[i] = 0; predOf[i] = -1; it.start = it.finish = 0; return; }
            let ready = 0, pred = -1, pres = null;
            it.res.rd.forEach(s => { const w = lastW[s]; if (w != null && finish[w] >= ready) { ready = finish[w]; pred = w; pres = s; } });
            it.start = ready; finish[i] = ready + it.lat; predOf[i] = pred; predRes[i] = pres; it.finish = finish[i];
            it.res.wr.forEach(s => { lastW[s] = i; });
            if (finish[i] > critical) { critical = finish[i]; endIdx = i; }
        });
        const critSet = new Set();
        for (let i = endIdx; i >= 0; i = predOf[i]) critSet.add(i);
        const width = window.VESTA_ISSUE_WIDTH[AR[a].name] || 4;
        const frontEnd = sumU / width, est = Math.max(frontEnd, tp, critical);
        let topPort = '', topVal = 0;
        portPress.forEach((v, k) => { if (v > topVal) { topVal = v; topPort = k; } });
        const feB = est === frontEnd, tpB = est === tp && !feB, ltB = est === critical && !feB && !tpB;
        const bneck = ltB
            ? 'latencia &mdash; el camino critico (filas <span class="critdot">&#9679;</span>) fija <b>' + critical.toFixed(2) + '</b> ciclos'
            : tpB
                ? 'throughput &mdash; limitan los puertos; grupo mas cargado: <b>' + esc(topPort) + '</b> (' + topVal.toFixed(2) + ' µops)'
                : 'front-end &mdash; el decodificador/emision (<b>' + width + '</b> µops/ciclo) no da abasto para ' + sumU + ' µops';
        let rows = '';
        items.forEach((it, i) => {
            const p = it.p, r = it.r;
            if (!r) {
                rows += '<tr><td class="mono">' + colorInstr(p.text) + '</td>' +
                    '<td class="miss" colspan="6">' + T('an.notfound') +
                    (byClass.has(p.mn) ? ' ' + T('an.badops') : ' ' + T('an.badmn')) + '</td></tr>';
                return;
            }
            const t = it.t, crit = critSet.has(i);
            const why = T('tip.matched', {
                ic: r[2], ops: (p.ops.map(o => o.kind + (o.width ? o.width : '')).join(', ') || T('vw.none'))
            });
            rows += '<tr' + (crit ? ' class="crit" data-tip="' + T('tip.crit') + '"' : '') + '>' +
                '<td class="mono">' + (crit ? '<span class="critdot">&#9679;</span> ' : '') + colorInstr(p.text) + '</td>' +
                '<td class="mono" data-tip="' + esc(why) + '">' + esc(r[1]) + (r[12] ? ' <span class="dim">' + esc(r[12]) + '</span>' : '') + '</td>' +
                '<td class="mono">' + esc(r[2]) + '</td>' +
                '<td class="n">' + (t ? t[1] : '&middot;') + '</td>' +
                '<td class="n mono">' + (t ? esc(t[0]) : '&middot;') + '</td>' +
                '<td class="n mono">' + (t ? (it.lat ? it.lat.toFixed(2) : '<span class="dim">0</span>') : '<span class="dim">' + T('an.nodata') + '</span>') + '</td>' +
                '<td class="mono">' + (t ? portsInline(t[5]) : '<span class="dim">&mdash;</span>') + '</td></tr>';
        });
        const timeline = renderTimeline(items, critSet, Math.max(critical, 1), null, T('cap.timeline'));
        const chain = renderCritChain({ endIdx, predOf, linkReg: predRes, items });
        const chainBlock = chain ? '<div class="cc-cap">' + T('an.cccap') + '</div>' + chain : '';
        const cyc = ' ' + T('an.cycles');
        const html = rows ?
            '<div class="wrap"><table><thead><tr><th>' + T('an.col.instr') + '</th><th>' + T('an.col.form') + '</th><th>' + T('th.iclass') + '</th>' +
            '<th class="n">' + T('th.uops') + '</th><th class="n">' + T('th.reciptp') + '</th><th class="n">' + T('an.col.lat') + '</th><th>' + T('an.col.ports') + '</th></tr></thead>' +
            '<tbody>' + rows + '</tbody></table></div>' +
            '<div class="an-sum"><b>' + (label || T('an.block')) + ' (' + esc(AR[a].name) + ')</b> &mdash; ' +
            nOk + ' ' + T('an.matched') + (nMiss ? ', ' + nMiss + ' ' + T('an.nomatch') : '') + '.' +
            '<table class="sumt">' +
            '<tr><td>' + T('an.uops') + '</td><td class="n">' + sumU + '</td></tr>' +
            '<tr' + (feB ? ' class="est"' : '') + '><td>' + T('an.fe', { w: width }) + '</td><td class="n">' + frontEnd.toFixed(2) + cyc + '</td></tr>' +
            '<tr' + (tpB ? ' class="est"' : '') + '><td>' + T('an.tp') + '</td><td class="n">' + tp.toFixed(2) + cyc + '</td></tr>' +
            '<tr' + (ltB ? ' class="est"' : '') + '><td>' + T('an.lat') + '</td><td class="n">' + critical.toFixed(2) + cyc + '</td></tr>' +
            '<tr class="est"><td>' + T('an.est') + '</td><td class="n">' + est.toFixed(2) + cyc + '</td></tr>' +
            '<tr><td>' + T('an.bneck') + '</td><td>' + bneck + '</td></tr></table></div>' + timeline + chainBlock
            : '<p class="hint">' + T('an.empty') + '</p>';
        return {
            html, est, uops: sumU, lat: critical, tp, fe: frontEnd, ok: nOk, miss: nMiss,
            items, critSet, critical, predOf, endIdx, linkReg: predRes
        };
    }

    // iconos de una instruccion en el timeline (microcodigo, carga, store, rompe dep).
    function markers(it) {
        if (!it.r) return '';
        let m = '';
        if (it.t && (it.t[2] & 1)) m += '<span class="mk mk-u" data-tip="' + T('mk.micro') + '">µ</span>';
        const sem = blockSem(it);
        if (sem && sem.barrier) m += '<span class="mk mk-x" data-tip="' + T('mk.barrier') + '">&#9873;</span>';
        if (it.res.rd.has('MEM')) m += '<span class="mk mk-l" data-tip="' + T('mk.load') + '">L</span>';
        if (it.res.wr.has('MEM')) m += '<span class="mk mk-s" data-tip="' + T('mk.store') + '">S</span>';
        if (it.lat === 0 && it.res.wr.size) m += '<span class="mk mk-b" data-tip="' + T('mk.breakdep') + '">&#9889;</span>';
        return m ? '<span class="mks">' + m + '</span>' : '';
    }
    // timeline (barras start->finish a escala `total`); elim = Set de textos eliminados.
    function renderTimeline(items, critSet, total, elim, cap) {
        let tl = '';
        items.forEach((it, i) => {
            if (!it.r) return;
            const crit = critSet.has(i), gone = elim && elim.has(it.p.text);
            const left = (it.start / total) * 100, w = Math.max((it.lat / total) * 100, 1.2);
            tl += '<div class="tl-row' + (gone ? ' elim' : '') + '">' +
                '<span class="tl-label mono">' + markers(it) + colorInstr(it.p.text) + '</span>' +
                '<div class="tl-track"><div class="tl-bar' + (crit ? ' crit' : '') +
                '" style="left:' + left.toFixed(2) + '%;width:' + w.toFixed(2) + '%" data-tip="ciclo ' +
                it.start.toFixed(0) + ' &rarr; ' + it.finish.toFixed(0) + ' (latencia ' + it.lat.toFixed(2) + ')">' +
                (it.lat ? it.lat.toFixed(0) : '') + '</div></div></div>';
        });
        if (!tl) return '';
        return '<div class="tl">' + (cap ? '<div class="tl-cap">' + cap + '</div>' : '') + tl +
            '<div class="tl-axis"><span>0</span><span>' + total.toFixed(0) + ' ' + T('an.cycles') + '</span></div></div>';
    }
    // camino critico como cadena de dependencias con flechas etiquetadas.
    function renderCritChain(A) {
        const chain = [];
        for (let i = A.endIdx; i >= 0; i = A.predOf[i]) chain.unshift(i);
        if (chain.length < 2) return '';
        let h = '<div class="cchain">';
        chain.forEach((i, idx) => {
            const it = A.items[i];
            if (idx > 0) {
                const reg = A.linkReg[i] || '?', prod = A.items[A.predOf[i]];
                h += '<div class="cc-arrow" data-tip="' + T('cc.dep', { r: esc(reg), n: prod.lat.toFixed(0) }) + '">' +
                    '&#8595; <span class="cc-reg">' + esc(reg) + '</span></div>';
            }
            h += '<div class="cc-node' + (idx === chain.length - 1 ? ' sink' : '') + '">' +
                '<span class="mono">' + colorInstr(it.p.text) + '</span>' +
                '<span class="cc-lat" data-tip="' + T('cc.lat') + '">' + it.lat.toFixed(0) + ' ' + T('an.cycles') + '</span></div>';
        });
        return h + '</div>';
    }

    // --- optimizador (peephole + eliminacion de codigo muerto), con liveness ---
    function blockSem(it) {
        const r = it.r; if (!r) return null;
        const rmask = parseInt(r[6], 16) || 0, wmask = parseInt(r[7], 16) || 0, mf = r[8];
        const rd = new Set(), wr = new Set(); let mw = false;
        it.p.ops.forEach((o, i) => {
            if (o.kind === 'reg') { if (rmask & (1 << i)) rd.add(canon(o.raw)); if (wmask & (1 << i)) wr.add(canon(o.raw)); }
            else if (o.kind === 'mem') { (o.addr || []).forEach(x => rd.add(canon(x))); if (rmask & (1 << i)) rd.add('MEM'); if (wmask & (1 << i)) mw = true; }
        });
        // efectos semanticos manuales del overlay (r[9]): una instruccion
        // serializante / barrera / atomica / con orden de memoria NO se puede
        // reordenar ni eliminar aunque su resultado no se use.
        const ov = r[9] || '';
        const barrier = /\b(serializing|no_reorder|barrier|atomic|ll_sc|syscall|privileged|mem_acquire|mem_release|mem_seq_cst)\b/.test(ov);
        const memfence = /\b(barrier|mem_acquire|mem_release|mem_seq_cst)\b/.test(ov);
        return {
            rd, wr, mw, wf: !!(mf & 4), rf: !!(mf & 8),
            ctrl: /^(J|CALL|RET|LOOP|SYSCALL|INT|UD|HLT)/.test(it.p.mn),
            barrier, memfence
        };
    }
    function regDeadAfter(list, i, R) {   // valor de R muerto tras i? (sobrescrito antes de leerse)
        for (let j = i + 1; j < list.length; j++) {
            if (!list[j].r) return false;             // instr desconocida -> conservador (vivo)
            const s = blockSem(list[j]);
            if (s.rd.has(R)) return false;            // se lee -> vivo
            if (s.wr.has(R)) return true;             // se sobrescribe antes de leerse -> muerto
        }
        return false;                                 // salida del bloque -> vivo
    }
    function flagsDeadAfter(list, i) {
        for (let j = i + 1; j < list.length; j++) {
            if (!list[j].r) return false;
            const s = blockSem(list[j]);
            if (s.rf) return false;                   // alguien lee flags -> vivas
            if (s.wf) return true;                    // se sobrescriben -> muertas
        }
        return true;                                  // flags muertas al final del bloque
    }
    function pow2Log(raw) { const v = Number(raw); return Number.isInteger(v) && v > 1 && (v & (v - 1)) === 0 ? Math.log2(v) : -1; }

    // Dos instrucciones estan EN CONFLICTO (hay que preservar su orden) si
    // comparten un recurso y al menos una lo escribe (RAW/WAR/WAW en registros,
    // flags o memoria; memoria de forma conservadora: cualquier par toca-toca).
    function conflict(a, b) {
        for (const R of a.wr) if (b.wr.has(R) || b.rd.has(R)) return true;
        for (const R of b.wr) if (a.rd.has(R)) return true;
        if ((a.wf && (b.wf || b.rf)) || (b.wf && a.rf)) return true;
        const am = a.mw || a.rd.has('MEM'), bm = b.mw || b.rd.has('MEM');
        return am && bm;
    }
    // Planificacion (list scheduling) que respeta TODAS las dependencias: da un
    // orden topologico valido priorizando el camino critico (altura por
    // latencia).  No cambia la semantica ni las cotas; ayuda a la emision en
    // orden / a que el decodificador vea trabajo independiente.
    function listSchedule(seg, a) {
        const n = seg.length, sem = seg.map(blockSem);
        const lat = seg.map(it => nodeLat(cost(it.r, a)));
        const succ = Array.from({ length: n }, () => []), inDeg = new Array(n).fill(0);
        for (let i = 0; i < n; i++) for (let j = i + 1; j < n; j++)
            if (conflict(sem[i], sem[j])) { succ[i].push(j); inDeg[j]++; }
        const height = new Array(n).fill(0);
        for (let i = n - 1; i >= 0; i--) { let h = 0; for (const s of succ[i]) h = Math.max(h, height[s]); height[i] = lat[i] + h; }
        const done = new Array(n).fill(false), rem = inDeg.slice(), order = [];
        for (let k = 0; k < n; k++) {
            const ready = [];
            for (let i = 0; i < n; i++) if (!done[i] && rem[i] === 0) ready.push(i);
            ready.sort((x, y) => height[y] - height[x] || x - y);
            const pick = ready[0]; done[pick] = true; order.push(seg[pick]);
            for (const s of succ[pick]) rem[s]--;
        }
        return order;
    }
    // Reordena por segmentos.  Son barreras (nada las cruza): instrucciones no
    // reconocidas, saltos/llamadas, y las que el overlay marca como
    // serializantes / fences / atomicas (cpuid, mfence, lock ...).  Ademas la
    // memoria se trata de forma CONSERVADORA: como no se sabe si dos accesos se
    // solapan ni si estan alineados, cualquier par toca-memoria conserva su
    // orden (ver conflict()).
    function scheduleReorder(items, a) {
        const isBarrier = it => { if (!it.r) return true; const s = blockSem(it); return s.ctrl || s.barrier; };
        const result = []; let seg = [], moved = false;
        const flush = () => {
            if (seg.length > 1) { const s = listSchedule(seg, a); if (s.some((x, i) => x !== seg[i])) moved = true; result.push(...s); }
            else result.push(...seg);
            seg = [];
        };
        for (const it of items) { if (isBarrier(it)) { flush(); result.push(it); } else seg.push(it); }
        flush();
        return { order: result, moved };
    }

    function optimize(items, a, en) {
        const log = [];
        const lines = [];
        items.forEach((it, i) => {
            const p = it.p, r = it.r;
            if (!r) { lines.push(p.text); return; }
            // mov reg, mismo-reg -> no-op
            if (en.selfcopy && p.mn === 'MOV' && p.ops.length === 2 && p.ops[0].kind === 'reg' && p.ops[1].kind === 'reg' && canon(p.ops[0].raw) === canon(p.ops[1].raw)) {
                log.push({ from: p.text, to: null, rule: T('rule.selfcopy'), why: T('why.selfcopy') }); return;
            }
            // mov reg, 0 -> xor reg, reg (idioma de puesta a cero)
            if (en.zero && p.mn === 'MOV' && p.ops.length === 2 && p.ops[0].kind === 'reg' && p.ops[1].kind === 'imm' && /^0(x0+)?$/i.test(p.ops[1].raw) && flagsDeadAfter(items, i)) {
                const R = p.ops[0].raw, to = 'xor ' + R + ', ' + R;
                log.push({ from: p.text, to, rule: T('rule.zero'), why: T('why.zero') });
                lines.push(to); return;
            }
            // imul reg, [reg,] 2^k -> shl reg, k (reduccion de fuerza)
            let k = -1, R = null;
            if (p.mn === 'IMUL' && p.ops.length === 2 && p.ops[0].kind === 'reg' && p.ops[1].kind === 'imm') { k = pow2Log(p.ops[1].raw); R = p.ops[0].raw; }
            else if (p.mn === 'IMUL' && p.ops.length === 3 && p.ops[0].kind === 'reg' && p.ops[1].kind === 'reg' && canon(p.ops[0].raw) === canon(p.ops[1].raw) && p.ops[2].kind === 'imm') { k = pow2Log(p.ops[2].raw); R = p.ops[0].raw; }
            if (en.strength && k >= 1 && R && flagsDeadAfter(items, i)) {
                const to = 'shl ' + R + ', ' + k;
                log.push({ from: p.text, to, rule: T('rule.strength'), why: T('why.strength', { k: k }) });
                lines.push(to); return;
            }
            lines.push(p.text);
        });
        // eliminacion de codigo muerto (sobre el codigo ya reescrito).
        const it2 = parseAll(lines.join('\n'));
        const keep = it2.map(() => true);
        if (en.dce) it2.forEach((it, i) => {
            if (!it.r) return;
            const s = blockSem(it);
            if (s.mw || s.ctrl || s.barrier || s.wr.size === 0) return;   // stores/saltos/barreras/comparaciones: se conservan
            let dead = true; s.wr.forEach(R => { if (!regDeadAfter(it2, i, R)) dead = false; });
            if (dead && (!s.wf || flagsDeadAfter(it2, i))) {
                keep[i] = false;
                log.push({ from: it.p.text, to: null, rule: T('rule.dce'), why: T('why.dce') });
            }
        });
        // fase 3: reordenacion valida para favorecer la ejecucion paralela.
        const kept = it2.filter((_, i) => keep[i]);
        const { order, moved } = en.reorder ? scheduleReorder(kept, a) : { order: kept, moved: false };
        if (moved) log.push({
            from: null, to: null, rule: T('rule.reorder'),
            why: T('why.reorder')
        });
        return { items: order, log };
    }

    function run() {
        const a = +arSel.value || 0;
        const items = parseAll($('src').value);
        if (!items.length) { $('res').innerHTML = '<p class="hint">' + T('an.empty') + '</p>'; $('summary').textContent = ''; return; }
        const A0 = analyzeBlock(items, a, T('an.block'));
        const en = {
            zero: $('opt-zero').checked, strength: $('opt-strength').checked,
            selfcopy: $('opt-selfcopy').checked, dce: $('opt-dce').checked,
            reorder: $('opt-reorder').checked,
        };
        const opt = optimize(items, a, en);
        let out = A0.html;
        if (opt.log.length) {
            const A1 = analyzeBlock(opt.items, a, T('an.blockopt'));
            const code = opt.items.map(x => hlLine(x.p.text)).join('\n');
            const optlist = opt.log.map(o =>
                '<li><span class="badge">' + esc(o.rule) + '</span> ' +
                (o.from == null ? '' :
                    '<span class="mono">' + esc(o.from) + '</span>' +
                    (o.to ? ' &rarr; <span class="mono c-mn">' + esc(o.to) + '</span>' : ' &rarr; <span class="dim">' + T('an.eliminated') + '</span>')) +
                '<div class="why">' + esc(o.why) + '</div></li>').join('');
            const d = (x, y) => { const p = x ? ((x - y) / x * 100) : 0; return y < x ? '<span class="better">-' + p.toFixed(0) + '%</span>' : y > x ? '<span class="worse">+' + (-p).toFixed(0) + '%</span>' : '<span class="dim">=</span>'; };
            // timelines antes/despues a la MISMA escala -> se ve donde se van los ciclos.
            const total = Math.max(A0.critical, A1.critical, 1);
            const elim = new Set(opt.log.filter(o => o.to === null && o.from).map(o => o.from));
            const cyc = ' ' + T('an.cycles');
            const cmpTl = '<div class="tlcmp">' +
                '<div class="tlcmp-col"><div class="tlcmp-h">' + T('an.before') + ' &mdash; ' + A0.critical.toFixed(0) + cyc + ' <span class="dim">' + T('an.elimnote') + '</span></div>' +
                renderTimeline(A0.items, A0.critSet, total, elim, null) + '</div>' +
                '<div class="tlcmp-col"><div class="tlcmp-h">' + T('an.after') + ' &mdash; ' + A1.critical.toFixed(0) + cyc + ' ' + d(A0.critical, A1.critical) + '</div>' +
                renderTimeline(A1.items, A1.critSet, total, null, null) + '</div></div>';
            out += '<h3 class="opt-h">' + T('an.optcode') + '</h3>' +
                '<pre class="codeblock">' + code + '</pre>' +
                '<div class="opt-log"><b>' + T('an.optapplied') + ' (' + opt.log.length + ')</b><ul>' + optlist + '</ul></div>' +
                '<table class="sumt cmp"><tr><th></th><th>' + T('an.cmp.orig') + '</th><th>' + T('an.cmp.opt') + '</th><th></th></tr>' +
                '<tr><td>' + T('th.uops') + '</td><td class="n">' + A0.uops + '</td><td class="n">' + A1.uops + '</td><td>' + d(A0.uops, A1.uops) + '</td></tr>' +
                '<tr><td>' + T('an.col.lat') + '</td><td class="n">' + A0.lat.toFixed(2) + '</td><td class="n">' + A1.lat.toFixed(2) + '</td><td>' + d(A0.lat, A1.lat) + '</td></tr>' +
                '<tr class="est"><td>' + T('an.est') + '</td><td class="n">' + A0.est.toFixed(2) + '</td><td class="n">' + A1.est.toFixed(2) + '</td><td>' + d(A0.est, A1.est) + '</td></tr></table>' +
                '<h4 class="opt-h" style="font-size:13px">' + T('an.cmp.ba') + '</h4>' + cmpTl +
                A1.html;
        } else {
            out += '<div class="opt-log dim">' + T('an.noopt') + '</div>';
        }
        $('res').innerHTML = out;
        $('summary').textContent = A0.ok + ' ' + T('an.matched') + (A0.miss ? ' / ' + A0.miss + ' ' + T('an.nomatch') : '');
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
    document.querySelectorAll('.opts input[type=checkbox]').forEach(c => c.addEventListener('change', run));
    $('src').addEventListener('input', syncHl);
    $('src').addEventListener('scroll', syncScroll);
    $('src').addEventListener('keydown', e => { if ((e.ctrlKey || e.metaKey) && e.key === 'Enter') run(); });
    syncHl();
    run();
})();
