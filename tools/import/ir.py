#!/usr/bin/env python3
"""IR comun de la base de datos de instrucciones (neutral respecto a la fuente).

Este modulo define el modelo INTERNO -- independiente de uops.info, Intel XED,
LLVM o los guides de ARM.  Todo importador (`uops_info.py`, `arm_guides.py`,
`llvm_sched.py`) produce EXACTAMENTE estas estructuras, y `build_database.py`
serializa desde aqui.  Asi, cambiar de fuente o anadir una ISA no toca el
modelo ni el serializador.

Tres capas separadas:
  - SINTAXIS (por-ISA): @ref InstrForm + @ref Operand.
  - TIMING (por-microarq): @ref MicroArchInstr (+ @ref LatencyEdge, @ref PortUse).
  - identidad de la microarq: @ref MicroArchSpec.
"""
from dataclasses import dataclass, field


@dataclass
class Operand:
    """Un operando de una forma de instruccion.

    Los registros IMPLICITOS (rax:rdx de mul/div, EAX de cpuid...) son tambien
    operandos -- con @c implicit/@c suppressed a True --, no una lista aparte:
    una sola representacion del mismo concepto.
    """
    idx: int              # base 0
    kind: str             # "reg" | "mem" | "imm" | "flags" | "agen" | "relbr"
    width: int            # bits (0 si no aplica)
    read: bool
    write: bool
    implicit: bool        # registro fijo (no lo elige el programador)
    suppressed: bool      # no aparece en el texto del ensamblador
    register_set: str     # conjunto de registros permitido (texto crudo), o ""


@dataclass
class LatencyEdge:
    """Latencia de UN camino operando-fuente -> operando-destino.

    El XML da varias por instruccion (op0->op1 = 1, op1->op0 = 3...).  NO se
    colapsan a un solo numero aqui: el serializador decide (peor caso, media,
    todas); el importador nunca destruye el dato.
    """
    start_operand: int    # base 0 (-1 = no especificado)
    target_operand: int   # base 0 (-1 = no especificado)
    cycles: float
    kind: str = "reg"     # "reg" | "mem" | "addr" | "addr_index"
    upper_bound: bool = False  # el XML marco la cifra como cota superior


@dataclass
class PortUse:
    """Uso de un grupo de puertos: @c uops micro-ops a @c port_group.

    @c port_group es el TOKEN de la fuente ("p0156"); el serializador lo
    convierte a un indice por-microarq.  El parser ya no deja cadenas de
    formato ("1*p0156+2*p23") sin desmenuzar.
    """
    port_group: str
    uops: float


@dataclass
class InstrForm:
    """La SINTAXIS de una forma de instruccion.  Clave: @c iform (no el
    mnemonico: @c IMUL son varias iform con costes distintos)."""
    iform: str
    mnemonic: str
    opcode: str
    extension: str
    # Firma de anchos de los operandos EXPLiCITOS.  Necesaria porque `iform`
    # NO es unica: las formas de ancho variable (GPRv/IMMz) se listan una vez
    # por ancho concreto (16/32/64), con costes distintos en DIV/MUL.  La
    # identidad real de una forma es iform + width_sig.
    width_sig: tuple = ()
    uid: str = ""                                  # iform + "/" + anchos (estable)
    operands: list = field(default_factory=list)  # list[Operand]
    # Mascaras derivadas (bit i = operando explicito i escrito/leido); el
    # compilador las usa directas, sin re-recorrer operandos.
    read_mask: int = 0
    write_mask: int = 0
    has_mem: bool = False
    has_imm: bool = False
    writes_flags: bool = False
    reads_flags: bool = False


@dataclass
class MicroArchInstr:
    """El COSTE de una forma en UNA microarquitectura."""
    latencies: list = field(default_factory=list)  # list[LatencyEdge]
    recip_tp: float = -1.0
    uops: int = 0
    microcoded: bool = False
    macro_fusible: bool = False
    micro_fusible: bool = False
    div_cycles: float = -1.0    # latencia del divisor (DIV/IDIV; -1 si no aplica)
    ports: list = field(default_factory=list)       # list[PortUse]


@dataclass
class MicroArchSpec:
    """Identidad de una microarquitectura: nombre en la fuente vs canonico."""
    xml_name: str         # como la nombra la fuente (p.ej. "SKL", "ADL-P")
    canonical_name: str   # nombre de Vesta (p.ej. "intel-skylake")
    family: str = ""      # "intel" | "amd" | "arm" (para -mcpu=<familia>-generic)
