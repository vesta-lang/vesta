#!/usr/bin/env python3
"""Etapa OPTIMIZER del IR: deduplica el coste en SchedulerClasses.

Importer -> IR -> **optimize** -> Serializer.  La deduplicacion NO vive en el
serializador: al ser una etapa IR->IR aparte, un dia se pueden fusionar/dividir
clases, comparar dos bases o generar estadisticas sin re-serializar.

Toma las mediciones crudas por microarq (@c {semantic_id: SchedulerClass}) y
produce una @ref ir.ArchSchedule: puertos normalizados a indices por-microarq,
clases deduplicadas por comportamiento identico (clave CUANTIZADA a centesimas
para que 0.5 y 0.5000001 no generen clases distintas), y el mapeo forma->clase.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ir  # noqa: E402


def _q(x):
    """Cuantiza a centesimas enteras (evita 0.5 != 0.5000001 en las claves)."""
    return int(round(x * 100))


def build_arch_schedule(spec, id_of, timing):
    """Construye el modelo de scheduling deduplicado de UNA microarq.

    @param spec    @ref ir.MicroArchSpec.
    @param id_of   { semantic_id: form_dense_id }.
    @param timing  { semantic_id: SchedulerClass } (medicion cruda).
    @return @ref ir.ArchSchedule.
    """
    port_index, port_order = {}, []

    def grp_idx(g):
        if g not in port_index:
            port_index[g] = len(port_order)
            port_order.append(g)
        return port_index[g]

    class_of = {}     # clave cuantizada -> class_id
    classes = []      # list[ir.SchedulerClass]
    form_class = []
    for sid, t in timing.items():
        if sid not in id_of:
            continue
        slots = sorted((grp_idx(p.port_group), p.uops) for p in t.ports)
        edges_key = tuple(sorted((e.start_operand, e.target_operand, e.kind,
                                  _q(e.cycles), int(e.upper_bound))
                                 for e in t.latencies))
        ports_key = tuple((gi, _q(u)) for gi, u in slots)
        key = (edges_key, _q(t.recip_tp), t.uops, int(t.microcoded),
               int(t.macro_fusible), _q(t.div_cycles), ports_key)
        cid = class_of.get(key)
        if cid is None:
            cid = len(classes)
            class_of[key] = cid
            classes.append(ir.SchedulerClass(
                latencies=list(t.latencies), recip_tp=t.recip_tp, uops=t.uops,
                microcoded=t.microcoded, macro_fusible=t.macro_fusible,
                div_cycles=t.div_cycles,
                ports=[ir.PortSlot(gi, u) for gi, u in slots]))
        form_class.append((id_of[sid], cid))

    form_class.sort()
    return ir.ArchSchedule(spec=spec, port_names=list(port_order),
                           classes=classes, form_class=form_class)
