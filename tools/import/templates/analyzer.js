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
  const GP = { rax: 'A', eax: 'A', ax: 'A', al: 'A', ah: 'A', rbx: 'B', ebx: 'B', bx: 'B', bl: 'B', bh: 'B',
    rcx: 'C', ecx: 'C', cx: 'C', cl: 'C', ch: 'C', rdx: 'D', edx: 'D', dx: 'D', dl: 'D', dh: 'D',
    rsi: 'SI', esi: 'SI', si: 'SI', sil: 'SI', rdi: 'DI', edi: 'DI', di: 'DI', dil: 'DI',
    rbp: 'BP', ebp: 'BP', bp: 'BP', bpl: 'BP', rsp: 'SP', esp: 'SP', sp: 'SP', spl: 'SP' };
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
      return m[1] + 'x<span class="pport" title="' + esc(portDesc(m[2])) + '">' + esc(m[2]) + '</span>';
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
    // analisis del bloque: throughput (suma recip_tp) y camino critico (deps).
    let rows = '', nOk = 0, nMiss = 0, sumU = 0, tp = 0, critical = 0;
    const lastW = {}, finish = [];
    items.forEach((it, i) => {
      const p = it.p, r = it.r;
      if (!r) {
        nMiss++; finish[i] = 0;
        rows += '<tr><td class="mono">' + esc(p.text) + '</td>' +
          '<td class="miss" colspan="6">forma no encontrada' +
          (byClass.has(p.mn) ? ' (operandos no encajan)' : ' (mnemonico desconocido)') + '</td></tr>';
        return;
      }
      const t = cost(r, a), lat = t ? maxLat(t[4]) : 0;
      nOk++; if (t) { sumU += t[1]; tp += parseFloat(t[0]) || 0; }
      // camino critico
      const res = resources(p, r);
      let ready = 0;
      res.rd.forEach(s => { if (lastW[s] != null) ready = Math.max(ready, finish[lastW[s]]); });
      finish[i] = ready + lat;
      res.wr.forEach(s => { lastW[s] = i; });
      if (finish[i] > critical) critical = finish[i];
      rows += '<tr><td class="mono">' + esc(p.text) + '</td>' +
        '<td class="mono">' + esc(r[1]) + (r[12] ? ' <span class="dim">' + esc(r[12]) + '</span>' : '') + '</td>' +
        '<td class="mono">' + esc(r[2]) + '</td>' +
        '<td class="n">' + (t ? t[1] : '&middot;') + '</td>' +
        '<td class="n mono">' + (t ? esc(t[0]) : '&middot;') + '</td>' +
        '<td class="n mono">' + (t ? (lat ? lat.toFixed(2) : '<span class="dim">0</span>') : '<span class="dim">sin dato</span>') + '</td>' +
        '<td class="mono">' + (t ? portsInline(t[5]) : '<span class="dim">&mdash;</span>') + '</td></tr>';
    });
    const est = Math.max(tp, critical);
    const bottleneck = critical > tp ? 'latencia (dependencias)' : 'throughput (puertos)';
    $('res').innerHTML = rows ?
      '<div class="wrap"><table><thead><tr><th>instruccion</th><th>forma</th>' +
      '<th>iclass</th><th class="n">uops</th><th class="n">recip_tp</th>' +
      '<th class="n">latencia</th><th>puertos</th></tr></thead>' +
      '<tbody>' + rows + '</tbody></table></div>' +
      '<div class="an-sum"><b>Analisis del bloque (' + esc(AR[a].name) + ')</b> &mdash; ' +
      nOk + ' instrucciones emparejadas' + (nMiss ? ', ' + nMiss + ' sin forma' : '') + '.' +
      '<table class="sumt"><tr><td>micro-operaciones (uops)</td><td class="n">' + sumU + '</td></tr>' +
      '<tr><td>coste por <b>throughput</b> (&Sigma; recip_tp)</td><td class="n">' + tp.toFixed(2) + ' ciclos</td></tr>' +
      '<tr><td>coste por <b>latencia</b> (camino critico de dependencias)</td><td class="n">' + critical.toFixed(2) + ' ciclos</td></tr>' +
      '<tr class="est"><td>estimacion del bloque = max(throughput, latencia)</td><td class="n">' + est.toFixed(2) + ' ciclos</td></tr>' +
      '<tr><td>cuello de botella</td><td class="n">' + bottleneck + '</td></tr></table></div>'
      : '<p class="hint">Sin instrucciones que analizar.</p>';
    $('summary').textContent = nOk + ' emparejadas' + (nMiss ? ' / ' + nMiss + ' sin forma' : '');
  }

  $('run').onclick = run;
  arSel.onchange = run;
  $('src').addEventListener('keydown', e => { if ((e.ctrlKey || e.metaKey) && e.key === 'Enter') run(); });
  run();
})();
