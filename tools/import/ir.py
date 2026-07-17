#!/usr/bin/env python3
"""IR comun de la base de datos de instrucciones (neutral respecto a la fuente).

Modelo INTERNO -- independiente de uops.info, Intel XED, LLVM o los guides de
ARM.  Todo importador (`uops_info.py`, `arm_guides.py`, `llvm_sched.py`) produce
EXACTAMENTE estas estructuras; la etapa @c optimize las deduplica; y
`build_database.py` las serializa.  El compilador nunca ve el formato de la
fuente.

Cuatro capas separadas:
  - SINTAXIS (por-ISA): @ref InstrForm + @ref Operand.
  - IDENTIDAD: @ref semantic_id (hash de la firma estructural completa; NO el
    nombre iform, que la fuente renombra entre versiones).
  - COSTE (por-microarq): @ref SchedulerClass (dedup) + @ref ArchSchedule.
  - identidad de la microarq: @ref MicroArchSpec.
"""
import hashlib
from dataclasses import dataclass, field


@dataclass
class Operand:
    """Un operando de una forma de instruccion.

    Los registros IMPLICITOS (rax:rdx de mul/div, RCX de rep, EAX de cpuid) son
    tambien operandos -- con @c suppressed a True --, no una lista aparte: una
    sola representacion, y entran en la identidad (rep MOVSB lleva RCX, MOVSB no).
    """
    idx: int              # base 0
    kind: str             # "reg" | "mem" | "imm" | "flags" | "agen" | "relbr"
    width: int            # bits (0 si no aplica)
    read: bool
    write: bool
    implicit: bool        # registro fijo (no lo elige el programador)
    suppressed: bool      # no aparece en el texto del ensamblador
    register_set: str     # conjunto de registros permitido (texto crudo), o ""


# Tipo de camino de latencia (enum estable; se ALMACENA el entero, nunca la
# cadena).  Los nombres son solo para depuracion.
LATENCY_RESULT = 0   # dato reg->reg (result)
LATENCY_ADDRESS = 1  # generacion de direccion (agen)
LATENCY_FLAGS = 2    # a/desde el operando de flags
LATENCY_MEMORY = 3   # load-use (a/desde memoria)
LATENCY_UNKNOWN = 4
LATENCY_KIND_ID = {"reg": LATENCY_RESULT, "addr": LATENCY_ADDRESS,
                   "addr_index": LATENCY_ADDRESS, "flags": LATENCY_FLAGS,
                   "mem": LATENCY_MEMORY, "unknown": LATENCY_UNKNOWN}
LATENCY_KIND_NAME = {v: k for k, v in
                     (("result", LATENCY_RESULT), ("address", LATENCY_ADDRESS),
                      ("flags", LATENCY_FLAGS), ("memory", LATENCY_MEMORY),
                      ("unknown", LATENCY_UNKNOWN))}


@dataclass
class LatencyEdge:
    """Latencia de UN camino operando-fuente -> operando-destino.

    El XML da varias por instruccion (op0->op1 = 1, op1->op0 = 3...).  NO se
    colapsan: el consumidor decide (peor caso, media); el importador nunca
    destruye el dato.  @c kind es un entero @c LATENCY_* (IR tipado)."""
    start_operand: int    # base 0 (-1 = no especificado)
    target_operand: int   # base 0 (-1 = no especificado)
    cycles: float
    kind: int = LATENCY_RESULT
    upper_bound: bool = False  # el XML marco la cifra como cota superior


@dataclass
class PortUse:
    """Uso de un grupo de puertos: @c uops micro-ops a @c port_group.

    En el IR de importacion @c port_group es el TOKEN de la fuente ("p0156");
    la etapa @c optimize lo normaliza a un indice por-microarq (@ref PortSlot).
    """
    port_group: str
    uops: float


@dataclass
class PortSlot:
    """Uso de puerto ya NORMALIZADO a indice por-microarq (nunca cadena)."""
    group: int
    uops: float


@dataclass
class InstrForm:
    """La SINTAXIS de una forma de instruccion.

    La IDENTIDAD (@ref semantic_id) NO sale del nombre @c iform (la fuente lo
    renombra: ADD_GPRv_GPRv -> ADD_GPRv_GPRv_01) sino del hash de @ref
    semantic_key, que compone TODOS los campos estructurales (iclass, opcode,
    extension, atributos de encoding @c enc, y TODOS los operandos, ocultos
    incluidos).  El @c iform / @c uid son SOLO documentacion / nombre del enum.
    """
    iform: str            # documentacion (nombre de la fuente)
    iclass: str           # mnemonico canonico (ADD, DIV, REP_MOVSB, CPUID...)
    mnemonic: str
    opcode: str
    extension: str
    # Atributos de ENCODING relevantes que la fuente da (isa_set, eosz, evex,
    # vex, mask, bcast, roundc, sae, nf, zeroing...).  Dict para que anadir uno
    # nuevo lo incluya en la identidad automaticamente.
    enc: dict = field(default_factory=dict)
    width_sig: tuple = ()                          # anchos explicitos (doc)
    uid: str = ""                                  # iform + "/" + anchos (doc)
    operands: list = field(default_factory=list)   # list[Operand]
    read_mask: int = 0
    write_mask: int = 0
    has_mem: bool = False
    has_imm: bool = False
    writes_flags: bool = False
    reads_flags: bool = False


def semantic_key(form):
    """Serializacion canonica y DETERMINISTA de la identidad estructural.

    Compone iclass|extension|opcode|enc|operandos (todos, con idx/kind/width/
    r/w/impl/supp).  Independiente del nombre iform; si la fuente anade un campo
    de encoding o un operando oculto, la clave cambia (es OTRA forma)."""
    ops = ";".join("%d:%s:%d:%d:%d:%d:%d" % (
        o.idx, o.kind, o.width, int(o.read), int(o.write),
        int(o.implicit), int(o.suppressed)) for o in form.operands)
    enc = ",".join("%s=%s" % (k, form.enc[k]) for k in sorted(form.enc))
    return "|".join([form.iclass, form.extension, form.opcode, enc, ops])


def semantic_id(form):
    """SemanticID de 64 bits = primeros 64 bits de SHA256(@ref semantic_key).

    Estable mientras la semantica sea identica; no depende del nombre.  Estilo
    TableGen pero por hash de contenido en vez de indice secuencial."""
    h = hashlib.sha256(semantic_key(form).encode("ascii")).hexdigest()
    return int(h[:16], 16)


@dataclass
class SchedulerClass:
    """El COSTE de una forma en UNA microarq (ya deduplicado)."""
    latencies: list = field(default_factory=list)  # list[LatencyEdge]
    recip_tp: float = -1.0
    uops: int = 0
    microcoded: bool = False
    macro_fusible: bool = False
    div_cycles: float = -1.0
    ports: list = field(default_factory=list)       # list[PortSlot]


@dataclass
class MicroArchSpec:
    """Identidad de una microarquitectura: nombre en la fuente vs canonico."""
    xml_name: str         # como la nombra la fuente (p.ej. "SKL", "ADL-P")
    canonical_name: str   # nombre de Vesta (p.ej. "intel-skylake")
    family: str = ""      # "intel" | "amd" | "arm" (para -mcpu=<familia>-generic)


@dataclass
class ArchSchedule:
    """Modelo de scheduling de UNA microarq: clases + mapeo forma->clase."""
    spec: MicroArchSpec = None
    port_names: list = field(default_factory=list)   # idx -> nombre de grupo
    classes: list = field(default_factory=list)      # list[SchedulerClass]
    form_class: list = field(default_factory=list)   # list[(form_id, class_id)]
