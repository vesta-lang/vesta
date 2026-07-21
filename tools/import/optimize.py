#!/usr/bin/env python3
"""Etapa OPTIMIZE del IR: asigna identidad + deduplica coste.

Importer -> IR -> **optimize** -> serialize.  Aqui vive TODA la logica de
transformacion (fuera del serializador y del importador):
  - FormID = indice denso por orden lexicografico de @ref ir.form_key (no un
    hash: compacto, determinista, verificable).
  - normalizacion de puertos a indices por-microarq.
  - deduplicacion de coste en @ref ir.SchedulerClass (clave CUANTIZADA a
    centesimas para que 0.5 y 0.5000001 no generen clases distintas).
  - @c form_class como array indexado por FormID (acceso O(1)).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ir  # noqa: E402


def _q(x):
    """Cuantiza a centesimas enteras.  El centinela -1.0 ('no disponible') ->
    -100, que sigue siendo distinto de cualquier valor real >= 0."""
    return int(round(x * 100))


def assign_ids(forms):
    """Ordena las formas por su clave estructural y asigna el FormID denso.

    @return (forms_ordenadas, { form_key: FormID }).  Un renombrado de la fuente
    NO reordena (la clave no depende del nombre); solo anadir/quitar una forma
    desplaza los IDs siguientes (modelo denso, como TableGen)."""
    forms.sort(key=ir.form_key)
    key_to_id = {}
    for i, fm in enumerate(forms):
        k = ir.form_key(fm)
        assert k not in key_to_id, "clave estructural duplicada: %r" % (k,)
        key_to_id[k] = i
    return forms, key_to_id


def build_arch_schedule(spec, key_to_id, timing, num_forms):
    """Modelo de scheduling deduplicado de UNA microarq.

    @param key_to_id  { form_key: FormID }.
    @param timing     { form_key: ir.RawSchedule }.
    @param num_forms  total de formas (tamano del array form_class).
    @return @ref ir.ArchSchedule.
    """
    port_index, port_order = {}, []

    def grp_idx(g):
        if g not in port_index:
            port_index[g] = len(port_order)
            port_order.append(g)
        return port_index[g]

    class_of = {}
    classes = []
    form_class = [-1] * num_forms
    for key, t in timing.items():
        fid = key_to_id.get(key)
        if fid is None:
            continue
        slots, slots_key = [], []
        for p in t.ports:
            gi = grp_idx(p.port_group)
            slots.append(ir.PortSlot(gi, p.uops))
            slots_key.append((gi, _q(p.uops)))
        slots.sort(key=lambda s: (s.group, s.uops))
        slots_key.sort()
        edges_key = tuple(sorted((e.start_operand, e.target_operand, e.kind,
                                  _q(e.cycles), int(e.upper_bound))
                                 for e in t.latencies))
        ckey = (edges_key, _q(t.recip_tp), t.uops, int(t.microcoded),
                int(t.macro_fusible), _q(t.div_cycles), tuple(slots_key))
        cid = class_of.get(ckey)
        if cid is None:
            cid = len(classes)
            class_of[ckey] = cid
            classes.append(ir.SchedulerClass(
                latencies=list(t.latencies), recip_tp=t.recip_tp, uops=t.uops,
                microcoded=t.microcoded, macro_fusible=t.macro_fusible,
                div_cycles=t.div_cycles, ports=slots))
        assert form_class[fid] == -1, "FormID %d duplicado en el timing" % fid
        form_class[fid] = cid

    # Sanidad: todo indice de puerto referenciado existe en la leyenda.
    for c in classes:
        for p in c.ports:
            assert 0 <= p.group < len(port_order), "indice de puerto invalido"
    return ir.ArchSchedule(spec=spec, port_names=list(port_order),
                           classes=classes, form_class=form_class)
