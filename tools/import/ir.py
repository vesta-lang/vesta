#!/usr/bin/env python3
"""IR comun de la base de datos de instrucciones (neutral respecto a la fuente).

Modelo INTERNO -- independiente de uops.info, Intel XED, LLVM o los guides de
ARM.  Pipeline: Importer -> IR -> optimize -> serialize.  El compilador nunca ve
el formato de la fuente.

Capas:
  - SINTAXIS/FORMA (por-ISA): @ref InstrForm + @ref Operand + @ref EncodingFeatures.
  - IDENTIDAD: @ref form_key (tupla estructural determinista) -> el FormID es el
    INDICE denso del orden lexicografico de las claves (no un hash).  El
    @ref checksum (FNV1a-64) es solo verificacion, no identidad.
  - COSTE (por-microarq): @ref RawSchedule (medicion cruda) -> @ref SchedulerClass
    (deduplicada) dentro de @ref ArchSchedule.
  - identidad de la microarq: @ref MicroArchSpec.
"""
import struct
from dataclasses import astuple, dataclass, field

# Version del ESQUEMA de identidad: si cambia QUE entra en form_key (p.ej. si el
# encoding deja de contar), subir esto -> los FormID viejos no se reutilizan.
SEMANTIC_SCHEMA = 1


@dataclass(frozen=True)
class Operand:
    """Un operando (los implicitos/suppressed son operandos normales, no una
    lista aparte: rep MOVSB lleva RCX, MOVSB no).  Inmutable -> hashable y
    directamente serializable en @ref form_key."""
    idx: int              # base 0
    kind: str             # "reg" | "mem" | "imm" | "flags" | "agen" | "relbr"
    width: int            # bits (0 si no aplica)
    read: bool
    write: bool
    implicit: bool
    suppressed: bool
    register_set: str     # conjunto permitido (distingue AL de GPR8); entra en id


@dataclass(frozen=True)
class EncodingFeatures:
    """Atributos de encoding que la fuente da y que afectan a la identidad.

    TIPADO (no un dict): un atributo nuevo del XML NO se cuela silenciosamente en
    la identidad -- el importador falla hasta anadirlo aqui conscientemente.  Se
    guarda el valor crudo (cadena) por fidelidad."""
    isa_set: str = ""
    eosz: str = ""
    evex: str = ""
    vex: str = ""
    mask: str = ""
    bcast: str = ""
    roundc: str = ""
    sae: str = ""
    nf: str = ""
    zeroing: str = ""
    incomplete_opcode: str = ""
    rep: str = ""
    locked: str = ""
    high8: str = ""
    agen: str = ""
    immzero: str = ""
    rm: str = ""
    mxcsr: str = ""
    no_reg_match: str = ""
    no_src_dest_match: str = ""

    def nonempty(self):
        """(campo, valor) de los atributos presentes -- para el .vxisa."""
        return [(f, v) for f, v in zip(
            ("isa_set", "eosz", "evex", "vex", "mask", "bcast", "roundc", "sae",
             "nf", "zeroing", "incomplete_opcode", "rep", "locked", "high8",
             "agen", "immzero", "rm", "mxcsr", "no_reg_match",
             "no_src_dest_match"), astuple(self)) if v]


# Tipo de camino de latencia (enum estable; se ALMACENA el entero).
LATENCY_RESULT = 0
LATENCY_ADDRESS = 1
LATENCY_FLAGS = 2
LATENCY_MEMORY = 3
LATENCY_UNKNOWN = 4
LATENCY_KIND_ID = {"reg": LATENCY_RESULT, "addr": LATENCY_ADDRESS,
                   "addr_index": LATENCY_ADDRESS, "flags": LATENCY_FLAGS,
                   "mem": LATENCY_MEMORY, "unknown": LATENCY_UNKNOWN}


@dataclass
class LatencyEdge:
    """Latencia de UN camino operando-fuente -> operando-destino.  @c kind es un
    entero @c LATENCY_* (IR tipado).  No se colapsan las aristas."""
    start_operand: int
    target_operand: int
    cycles: float
    kind: int = LATENCY_RESULT
    upper_bound: bool = False


@dataclass
class PortUse:
    """Uso de puerto CRUDO: el token de la fuente ("p0156").  optimize lo
    normaliza a @ref PortSlot."""
    port_group: str
    uops: float


@dataclass
class PortSlot:
    """Uso de puerto NORMALIZADO a indice por-microarq (nunca cadena)."""
    group: int
    uops: float


@dataclass
class InstrForm:
    """La FORMA (encoding) de una instruccion.  La identidad es @ref form_key
    (estructural), NO el nombre @c iform, que la fuente renombra: @c iform es
    documentacion / nombre humano del enum."""
    iform: str            # documentacion (nombre de la fuente)
    iclass: str           # mnemonico canonico (ADD, DIV, REP_MOVSB, CPUID...)
    mnemonic: str
    opcode: str
    extension: str
    enc: EncodingFeatures = field(default_factory=EncodingFeatures)
    operands: list = field(default_factory=list)   # list[Operand]
    read_mask: int = 0
    write_mask: int = 0
    has_mem: bool = False
    has_imm: bool = False
    writes_flags: bool = False
    reads_flags: bool = False


def _pack_str(buf, s):
    """Cadena length-prefixed (u32 LE + utf-8) -- serializacion propia, NO
    depende de repr() ni del formato interno de Python."""
    b = s.encode("utf-8")
    buf += struct.pack("<I", len(b))
    buf += b


def form_key(form):
    """Clave estructural DETERMINISTA (BYTES canonicos) = identidad de la forma.

    Serializa schema + iclass + extension + opcode + encoding + operandos (con
    register_set) con codificacion propia length-prefixed.  Independiente del
    nombre iform y de repr().  El opcode y el register_set SI entran: identifica
    la FORMA ISA (encoding), no la semantica pura -- add 01/r y 03/r son formas
    distintas (no se pierde encoding).  Al ser bytes, ordena de forma
    determinista (el FormID = su indice denso) y sirve de clave de dedup."""
    buf = bytearray()
    buf += struct.pack("<I", SEMANTIC_SCHEMA)
    for s in (form.iclass, form.extension, form.opcode):
        _pack_str(buf, s)
    for v in astuple(form.enc):        # 20 campos, orden fijo del dataclass
        _pack_str(buf, v)
    buf += struct.pack("<I", len(form.operands))
    for o in form.operands:
        buf += struct.pack("<I", o.idx)
        _pack_str(buf, o.kind)
        buf += struct.pack("<I", o.width)
        buf += struct.pack("<B", int(o.read) | (int(o.write) << 1) |
                           (int(o.implicit) << 2) | (int(o.suppressed) << 3))
        _pack_str(buf, o.register_set)
    return bytes(buf)


def checksum(form):
    """FNV1a-64 sobre @ref form_key -- SOLO verificacion / comparacion entre
    versiones (no es el identificador; el FormID es el indice denso)."""
    h = 0xcbf29ce484222325
    for b in form_key(form):
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def derive_effects(operands):
    """Campos DERIVADOS de la lista de operandos (mascaras + mem/imm/flags).

    Vive en el IR para que cualquier importador (uops.info, ARM, LLVM) reuse la
    MISMA logica en vez de duplicarla.  @return (read_mask, write_mask, has_mem,
    has_imm, writes_flags, reads_flags)."""
    rm = wm = 0
    has_mem = has_imm = wr_flags = rd_flags = False
    for o in operands:
        if o.kind == "mem":
            has_mem = True
        elif o.kind == "imm":
            has_imm = True
        elif o.kind == "flags":
            if o.write:
                wr_flags = True
            if o.read:
                rd_flags = True
        # Mascaras: solo operandos EXPLICITOS de registro/memoria.
        if not o.implicit and not o.suppressed and o.kind in ("reg", "mem"):
            bit = 1 << o.idx
            if o.read:
                rm |= bit
            if o.write:
                wm |= bit
    return (rm, wm, has_mem, has_imm, wr_flags, rd_flags)


@dataclass
class RawSchedule:
    """Medicion CRUDA por-instruccion de una microarq (antes de deduplicar)."""
    latencies: list = field(default_factory=list)  # list[LatencyEdge]
    recip_tp: float = -1.0
    uops: int = 0
    microcoded: bool = False
    macro_fusible: bool = False
    div_cycles: float = -1.0
    ports: list = field(default_factory=list)       # list[PortUse]


@dataclass
class SchedulerClass:
    """Clase de coste COMPARTIDA (deduplicada) de una microarq."""
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
    xml_name: str
    canonical_name: str
    family: str = ""


@dataclass
class ArchSchedule:
    """Modelo de scheduling de UNA microarq: clases + mapeo forma->clase.

    @c form_class es un array indexado por FormID (form_class[id] = class_id, o
    -1 si no hay dato) -> acceso O(1), sin pares ni orden."""
    spec: MicroArchSpec = None
    port_names: list = field(default_factory=list)   # idx -> nombre de grupo
    classes: list = field(default_factory=list)      # list[SchedulerClass]
    form_class: list = field(default_factory=list)   # form_class[FormID]=class_id
