/**
 * @file protocol.ts
 * @brief Forma de las peticiones y respuestas propias del servidor de Vesta.
 *
 * Ademas de los metodos del protocolo estandar, el servidor expone bajo el
 * prefijo `vesta/` las vistas del ecosistema: IR, bytecode, ensamblador del
 * JIT y del AOT, diagramas, coste, compatibilidad y compilacion embebida.  Se
 * anuncian en `capabilities.experimental.vestaMethods`.
 *
 * Ninguna de estas respuestas lanza error de protocolo: cuando algo falla, el
 * servidor contesta un objeto con el campo `error` relleno.  Por eso todos los
 * tipos de aqui heredan de @ref VestaResponse y llevan sus campos opcionales.
 */

/** Nombres de los metodos propios del servidor. */
export const VestaMethod = {
    Ir: 'vesta/ir',
    IrDiff: 'vesta/irDiff',
    Bytecode: 'vesta/bytecode',
    JitAsm: 'vesta/jitAsm',
    AotAsm: 'vesta/aotAsm',
    Complexity: 'vesta/complexity',
    Diagram: 'vesta/diagram',
    Functions: 'vesta/functions',
    AotCompat: 'vesta/aotCompat',
    Modes: 'vesta/modes',
    MacroExpand: 'vesta/macroExpand',
    ComptimeValues: 'vesta/comptimeValues',
    ParamHints: 'vesta/paramHints',
    Asa: 'vesta/asa',
    AsaFacts: 'vesta/asaFacts',
    Targets: 'vesta/targets',
    SymbolInfo: 'vesta/symbolInfo',
    Compile: 'vesta/compile',
    CompileProject: 'vesta/compileProject',
} as const;

/** Base de toda respuesta: el fallo viaja en el resultado, no como excepcion. */
export interface VestaResponse {
    /** Motivo del fallo; ausente si la peticion fue bien. */
    error?: string;
}

/**
 * Con que se compila lo que se mira.
 *
 * No es solo "para que maquina": el nivel de optimizacion, el juego de
 * instrucciones de coma flotante y la microarquitectura cambian el codigo que
 * sale, asi que forman parte de la pregunta.  Campos vacios = lo de por
 * defecto.
 */
export interface InspectTarget {
    os?: string;
    arch?: string;
    /** 0..3; ausente = el nivel con el que compila el analisis normal. */
    opt?: number;
    floatIsa?: string;
    /** Microarquitectura concreta, de las que conoce el compilador. */
    cpu?: string;
}

/** Una arquitectura para la que se puede compilar o mirar. */
export interface TargetArch {
    /** Lo que se manda en `arch`. */
    id: string;
    name: string;
    /** false = se conoce su juego de instrucciones, pero no se genera codigo. */
    codegen: boolean;
    /** Microarquitecturas cronometradas en la base de instrucciones. */
    microarchs: string[];
    /** CPU concretas conocidas, con sus capacidades. */
    cpus: string[];
}

/** Respuesta de `vesta/targets`: lo que el compilador sabe hacer. */
export interface TargetsResponse extends VestaResponse {
    architectures?: TargetArch[];
    floatIsas?: string[];
    optLevels?: number[];
}

/** Respuesta de las vistas que devuelven un texto (IR, bytecode, diagramas). */
export interface TextResponse extends VestaResponse {
    text?: string;
}

/** Una funcion del modulo, con la linea en la que empieza. */
export interface FunctionEntry {
    name: string;
    line: number;
}

/** Respuesta de `vesta/functions`. */
export interface FunctionsResponse extends VestaResponse {
    functions?: FunctionEntry[];
}

/** Coste estimado de una funcion. */
export interface ComplexityEntry {
    name: string;
    /** Coste del cuerpo propio, sin contar las llamadas. */
    partial: string;
    /** Coste total, incluyendo lo que cuestan las llamadas. */
    total: string;
    confidence: string;
    total_confidence: string;
    max_loop_depth: number;
    recursive: boolean;
    /** Coste declarado con `@complexity`, si lo hay. */
    declared?: string;
    /** true si lo declarado no cuadra con lo inferido. */
    contract_mismatch?: boolean;
}

/** Respuesta de `vesta/complexity`. */
export interface ComplexityResponse extends VestaResponse {
    functions?: ComplexityEntry[];
}

/** Una linea del fuente, tal y como la devuelven las vistas correlacionadas. */
export interface SourceLine {
    line: number;
    text: string;
}

/** Una instruccion desensamblada, ligada a su linea fuente y a su op del IR. */
export interface AsmLine {
    /** Desplazamiento en hexadecimal dentro de la funcion. */
    addr: string;
    text: string;
    /** Linea del fuente que la genero; 0 si no se sabe. */
    line: number;
    /** Identidad de la operacion del IR que la genero. */
    ir_id?: number;
}

/** Una fila del listado del IR: la etiqueta de un bloque o una operacion. */
export interface IrRow {
    kind: 'label' | 'op';
    line: number;
    text: string;
}

/** Una posicion del marco de pila de la funcion. */
export interface FrameSlot {
    offset: number;
    label: string;
    size: number;
    kind: string;
    name: string;
}

/** Un argumento de la funcion y el registro por el que llega. */
export interface ArgumentSlot {
    name: string;
    reg: string;
}

/** Una reubicacion pendiente en el codigo AOT. */
export interface Relocation {
    offset: number;
    kind: string;
    symbol: string;
    addend: number;
}

/** Una etiqueta interna de un bloque de ensamblador en linea. */
export interface AsmLabel {
    offset: string;
    name: string;
}

/** Respuesta de `vesta/jitAsm` y de `vesta/aotAsm`. */
export interface AsmResponse extends VestaResponse {
    /** Desensamblado en texto plano. */
    text?: string;
    /** Funcion que se ha compilado. */
    function?: string;
    bytes?: number;
    instructions?: number;
    /** Desensamblado fila a fila, con la linea fuente de cada instruccion. */
    asm_lines?: AsmLine[];
    /** Lineas del fuente de la funcion. */
    source?: SourceLine[];
    /** Listado del IR, para la columna central de la vista correlacionada. */
    ir_listing?: IrRow[];
    /** Operacion del IR por identidad, para cruzarla con `AsmLine.ir_id`. */
    ir_by_id?: Record<string, string>;
    block_names?: string[];
    frame?: FrameSlot[];
    args?: ArgumentSlot[];
    asm_labels?: AsmLabel[];
    /** Solo en AOT. */
    relocs?: Relocation[];
    /** true si el generador del JIT no cubre las operaciones de la funcion. */
    unsupported?: boolean;
    /** true si la funcion no es compatible con la compilacion anticipada. */
    incompatible?: boolean;
    /** Explicacion de por que no se pudo compilar. */
    reason?: string;
}

/** Un motivo por el que una funcion no puede compilarse de forma anticipada. */
export interface AotIssue {
    fn_name: string;
    source_line: number;
    op: string;
    reason: string;
}

/** Respuesta de `vesta/aotCompat`. */
export interface AotCompatResponse extends VestaResponse {
    compatible?: boolean;
    issues?: AotIssue[];
    ok_functions?: string[];
}

/** Informe de uno de los tres modos de ejecucion. */
export interface ModeReport {
    mode: string;
    ok?: boolean;
    errors?: number;
    warnings?: number;
    note?: string;
    tier?: string;
    compatible?: boolean;
    compilable_functions?: string[];
    fallback_functions?: string[];
    ok_functions?: string[];
    issues?: AotIssue[];
}

/** Respuesta de `vesta/modes`. */
export interface ModesResponse extends VestaResponse {
    modes?: ModeReport[];
}

/** Expansion de una macro en un punto de llamada concreto. */
export interface MacroExpansion {
    macro_name: string;
    call_site_loc: string;
    args: string[];
    generated_code: string;
}

/** Macro que no se pudo expandir, y el motivo. */
export interface MacroSkip {
    name: string;
    reason: string;
}

/** Respuesta de `vesta/macroExpand`. */
export interface MacroExpandResponse extends VestaResponse {
    expansions?: MacroExpansion[];
    skipped?: MacroSkip[];
}

/** Una constante resuelta en tiempo de compilacion. */
export interface ComptimeValue {
    name: string;
    scope: string;
    type_kind: string;
    value_str: string;
}

/** Respuesta de `vesta/comptimeValues`. */
export interface ComptimeValuesResponse extends VestaResponse {
    values?: ComptimeValue[];
}

/** Un diagnostico devuelto por la compilacion embebida. */
export interface CompileDiagnostic {
    severity?: string;
    message?: string;
    line?: number;
    character?: number;
    file?: string;
}

/** Respuesta de `vesta/compile` y de `vesta/compileProject`. */
export interface CompileResponse extends VestaResponse {
    ok?: boolean;
    /** Ruta del artefacto generado. */
    output?: string;
    diagnostics?: CompileDiagnostic[];
    /** Tiempo del frontend, en microsegundos. */
    frontend_us?: number;
    mode?: string;
    project?: boolean;
    message?: string;
}

/** Una pista de parametro: donde va y que texto mostrar. */
export interface ParamHint {
    line: number;
    character: number;
    label: string;
}

/** Respuesta de `vesta/paramHints`. */
export interface ParamHintsResponse extends VestaResponse {
    hints?: ParamHint[];
}

/**
 * Un hecho del compilador, atado a la linea del fuente a la que pertenece.
 *
 * Lleva el dato en crudo (`code`, `a`, `b`, `detail`) y su etiqueta ya
 * resuelta en el idioma activo, para poder ensenarla sin reinterpretarla.
 * El ambito importa tanto como el hecho: uno que solo vale para una
 * arquitectura o un backend no se puede ensenar como si valiera siempre.
 */
export interface AsaFact {
    /** Linea del fuente, contando desde uno; 0 si no se pudo atar. */
    line: number;
    function: string;
    /** De que habla: modulo, funcion, valor, bloque, instruccion o simbolo. */
    subject: string;
    /** Analisis que lo afirma. */
    domain: string;
    /** Codigo estable del vocabulario de ese analisis. */
    code: string;
    a: number;
    b: number;
    detail: string;
    /** Texto corto listo para mostrar. */
    label: string;
    /** demostrada | inferida | desconocida. */
    certainty: string;
    /** estatico | ejecucion | perfil | declarado. */
    source: string;
    isa: string;
    os: string;
    backend: string;
}

/** Lo que produjo un analisis, incluido lo que NO pudo saber. */
export interface AsaDomain {
    domain: string;
    facts: number;
    /** Entidades examinadas, incluidas las que no dieron nada. */
    looked: number;
    silent: number;
    micros: number;
    /** Por que se callo, por motivo y cuantas veces. */
    unknown: { code: string; times: number }[];
}

/** Respuesta de `vesta/asaFacts`. */
export interface AsaFactsResponse extends VestaResponse {
    facts?: AsaFact[];
    domains?: AsaDomain[];
}

/** Fases del IR que admite `vesta/ir`. */
export type IrPhase = 'pre' | 'post';

/** Vistas que admite `vesta/diagram`. */
export type DiagramKind = 'ast' | 'ir-pre' | 'ir-post' | 'vel' | 'asm';

/** Formatos en los que se puede pedir un diagrama. */
export type DiagramFormat = 'mermaid' | 'graphviz' | 'html';

/** Niveles de runtime que puede asumir la compilacion anticipada. */
export type AotTier = 'bare' | 'embed' | 'full';
