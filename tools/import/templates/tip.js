// Tooltip propio (fiable e inmediato; el `title` nativo tarda y falla en celdas).
// Cualquier elemento con atributo data-tip muestra su texto al pasar el cursor.
(function () {
    const box = document.createElement('div');
    box.className = 'tipbox';
    box.style.display = 'none';
    document.body.appendChild(box);
    function hide() { box.style.display = 'none'; }
    document.addEventListener('mouseover', e => {
        const el = e.target.closest('[data-tip]');
        if (!el) { hide(); return; }
        box.textContent = el.getAttribute('data-tip');
        box.style.display = 'block';
        const r = el.getBoundingClientRect();
        let left = window.scrollX + r.left;
        left = Math.min(left, window.scrollX + document.documentElement.clientWidth - box.offsetWidth - 8);
        box.style.left = Math.max(8, left) + 'px';
        box.style.top = (window.scrollY + r.bottom + 6) + 'px';
    });
    document.addEventListener('mouseout', e => { if (e.target.closest('[data-tip]')) hide(); });
    document.addEventListener('scroll', hide, true);
})();
