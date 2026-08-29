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
    Instruction: 'vesta/instruction',
    FunctionReport: 'vesta/functionReport',
    AsmBlock: 'vesta/asmBlock',
    AsmFlow: 'vesta/asmFlow',
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

/**
 * Una fila del diff del IR, con las dos versiones ALINEADAS.
 *
 * No es un texto con `+` y `-`: es la comparacion ya resuelta, para poder
 * ensenarla como se quiera -- unificada, a dos columnas, o contando cuanto
 * cambio --.
 */
export interface IrDiffRow {
    /** same = igual, del = desaparecio, add = es nueva, chg = cambio. */
    k: 'same' | 'del' | 'add' | 'chg';
    /** Como estaba antes de optimizar. */
    l: string;
    /** Como quedo despues. */
    r: string;
}

/** Respuesta de `vesta/irDiff`. */
export interface IrDiffResponse extends VestaResponse {
    rows?: IrDiffRow[];
    /** Funcion comparada; vacia = el modulo entero. */
    function?: string;
}

/** Respuesta de las vistas que devuelven un texto (IR, bytecode, diagramas). */
export interface TextResponse extends VestaResponse {
    text?: string;
}

/** Una funcion del modulo, con la linea en la que empieza. */
export interface FunctionEntry {
    /**
     * Nombre INTERNO (`std__windows__GetCurrentFiber`).
     *
     * Es el que identifica: unico, y el que hay que mandarle al servidor al
     * pedir algo sobre esta funcion.  NO es el que se ensena.
     */
    name: string;
    /**
     * El nombre que se ESCRIBIO (`std.windows.GetCurrentFiber`).
     *
     * El interno no lo escribio nadie: lo construye el compilador al aplanar
     * los namespaces, y ensenarlo obliga a quien lee a traducirlo de cabeza.
     */
    display?: string;
    line: number;
}

/** Respuesta de `vesta/functions`. */
export interface FunctionsResponse extends VestaResponse {
    functions?: FunctionEntry[];
}

/** Coste estimado de una funcion. */
export interface ComplexityEntry {
    /** Nombre interno; identifica. */
    name: string;
    /** El nombre que se escribio; es el que se ensena. */
    display?: string;
    /** Coste del cuerpo propio, sin contar las llamadas. */
    partial: string;
    /** Coste total, incluyendo lo que cuestan las llamadas. */
    total: string;
    confidence: string;
    /** Nombre de la confianza: exacta | heuristica | desconocida. */
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
    /** Nombre interno de la funcion; identifica. */
    fn_name: string;
    /** El nombre que se escribio; es el que se ensena. */
    fn_display?: string;
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
    /** Nombre interno de la funcion a la que pertenece. */
    function: string;
    /** El nombre que se escribio; es el que se ensena. */
    functionDisplay?: string;
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
    /** Identificador del sujeto: el valor SSA, el bloque o la instruccion. */
    subjectId: number;
    /**
     * La operacion que DEFINE al sujeto, cuando se puede situar.
     *
     * "valor" no identifica nada -- en una linea puede haber ocho --, asi que
     * lo que se ensena es su operacion: `%12 = add %7, 40`.
     */
    subjectText: string;
    /**
     * La linea del FUENTE de la que habla.
     *
     * Es lo que se ensena por defecto: la operacion del IR identifica sin lugar
     * a dudas y no dice nada a quien no lo tiene delante.
     */
    sourceText: string;
    /** Regla por la que se dedujo; vacia si es una observacion directa. */
    rule: string;
    /** Hechos de los que se sigue, por su indice en `facts`. */
    from: number[];
    /** Analisis que lo emitio. */
    producer: string;
    /** Sitio exacto que miro (valor, bloque o linea, segun el analisis). */
    site: number;
    /** En que otros analisis se apoya. */
    restsOn: string[];
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
    /** Que mira este analisis, en una frase. */
    purpose?: string;
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

/** Un puerto de ejecucion por el que pasa una instruccion. */
export interface InstructionPort {
    port: number;
    uops: number;
    name?: string;
}

/** Lo que cuesta una instruccion en una microarquitectura concreta. */
export interface InstructionCost {
    /** false = esa microarquitectura no cronometra esta forma. */
    timed: boolean;
    latency?: number;
    /** Cada cuantos ciclos se puede repetir. */
    reciprocalThroughput?: number;
    uops?: number;
    microcoded?: boolean;
    macroFusible?: boolean;
    divCycles?: number;
    ports?: InstructionPort[];
}

/** Respuesta de `vesta/instruction`: la ficha de una instruccion. */
export interface InstructionResponse extends VestaResponse {
    /** false = esa linea no es una instruccion (etiqueta, llave, comentario). */
    found?: boolean;
    /**
     * false = la base no conoce esta instruccion.  No es lo mismo que `found`:
     * ahi si hay una instruccion, y saber que el compilador no la reconoce es
     * lo mas util que se puede decir de ella -- la trata como barrera.
     */
    known?: boolean;
    /** Por que no se conoce, cuando `known` es false. */
    unknownReason?: string;
    /** A que base se pregunto: x86, arm64, arm32, riscv. */
    isa?: string;
    /** Microarquitectura con la que se respondio el coste. */
    microarch?: string;
    /** Clase de planificacion. */
    iclass?: string;
    /** Extension del juego de instrucciones a la que pertenece. */
    extension?: string;
    /** false = sus operandos no estan modelados; se trata conservador. */
    modeled?: boolean;
    /** true = nada se puede mover al otro lado. */
    barrier?: boolean;
    /** true = es una llamada; se le supone todo efecto. */
    isCall?: boolean;
    touchesMemory?: boolean;
    /**
     * Que hizo el compilador con ella: "micro" (se emite tal cual, con su
     * identidad en la base resuelta), "ir" (se elevo a operaciones del IR y se
     * optimiza como el resto del codigo) o "ninguno".
     */
    lifted?: string;
    /** "compilador" o "texto": de donde sale lo que se cuenta. */
    resolvedBy?: string;
    /** Operaciones del IR en las que quedo, cuando se elevo. */
    irOps?: string[];
    reads?: string[];
    writes?: string[];
    readsMemory?: boolean;
    writesMemory?: boolean;
    readsFlags?: boolean;
    writesFlags?: boolean;
    /** Que banderas concretas, cuando la base trae el detalle. */
    flagsRead?: string[];
    flagsWritten?: string[];
    /** Estado del procesador que no es un registro general. */
    readsState?: string[];
    writesState?: string[];
    cost?: InstructionCost;
}

/** Lo que el compilador MIDE de una funcion, sobre el codigo que sale. */
export interface FunctionMeasured {
    /** Sitios donde reserva memoria: propios y con lo que llama. */
    allocPartial: number;
    allocTotal: number;
    /** Bytes de pila: propios y con lo que llama. */
    stackPartial: number;
    /** Solo viene si la pila se puede acotar; si no, `stackBounded` es false. */
    stackTotal?: number;
    /**
     * false = la profundidad de pila NO se puede acotar.
     *
     * Va aparte a proposito: el compilador lo marca con un centinela que es un
     * numero valido, y mandandolo tal cual se pinta como si la funcion gastara
     * dieciocho trillones de bytes.
     */
    stackBounded?: boolean;
    throws: boolean;
    panics: boolean;
    pure: boolean;
    recursive: boolean;
    /** false = hay llamadas cuyos efectos no se pueden cerrar. */
    effectsKnown: boolean;
    /** true = tiene asm, su marco de pila no se ve en el IR. */
    frameOpaque: boolean;
    dynamicCall: boolean;
}

/** Lo que la funcion DECLARA.  Solo lo declarado aparece. */
export interface FunctionDeclared {
    pure?: boolean;
    nothrow?: boolean;
    nopanic?: boolean;
    allocPartial?: number;
    allocTotal?: number;
    stackPartial?: number;
    stackTotal?: number;
}

/** El veredicto de UN contrato declarado. */
export interface ContractCheck {
    /** "@pure", "@alloc", "@stack"... */
    contract: string;
    /** cumple | incumple | no se puede decidir. */
    status: string;
    detail: string;
}

/** Lo que cuesta una funcion, con lo que declaro al lado. */
export interface FunctionCost {
    partial: string;
    total: string;
    confidence: string;
    totalConfidence: string;
    loops: number;
    recursive: boolean;
    /** Lo declarado con `@complexity`; vacio si no declaro. */
    declared: string;
    /** true si lo declarado no cuadra con lo inferido. */
    mismatch: boolean;
}

/** Una funcion en el informe: lo que declara frente a lo que hace. */
export interface FunctionReportEntry {
    /** Nombre interno; identifica. */
    name: string;
    /** El nombre que se escribio; es el que se ensena. */
    display: string;
    line: number;
    cost: FunctionCost;
    measured?: FunctionMeasured;
    declared?: FunctionDeclared;
    checks: ContractCheck[];
    aot: {
        ok: boolean;
        issues: { op: string; reason: string; line: number }[];
    };
}

/** Respuesta de `vesta/functionReport`. */
export interface FunctionReportResponse extends VestaResponse {
    functions?: FunctionReportEntry[];
}

/** Una instruccion del bloque, con su flujo y lo que se sabe de ella. */
export interface AsmBlockInsn {
    /** Su posicion en el bloque; es a lo que apuntan los saltos. */
    index: number;
    text: string;
    /** Linea del fuente, para poder ir a ella. */
    line: number;
    /** Etiquetas definidas justo antes de ella. */
    labels: string[];
    /** sigue | salto | rama | llamada | retorno | indirecto | sin clasificar. */
    flow: string;
    /** Etiqueta destino, si salta. */
    target: string;
    /** Instruccion destino; -1 si no salta o no se pudo resolver. */
    targetIndex: number;
    /** false = la base no conoce esta instruccion. */
    known: boolean;
    iclass?: string;
    cost?: { latency: number; reciprocalThroughput: number; uops: number };
    modeled: boolean;
    barrier: boolean;
    reads: string[];
    writes: string[];
    readsMemory: boolean;
    writesMemory: boolean;
    flagsRead?: string[];
    flagsWritten?: string[];
}

/** Respuesta de `vesta/asmBlock`. */
export interface AsmBlockResponse extends VestaResponse {
    /** false = esa linea no cae dentro de un bloque de ensamblador. */
    found?: boolean;
    isa?: string;
    microarch?: string;
    firstLine?: number;
    lastLine?: number;
    instructions?: AsmBlockInsn[];
    /** true = hay un salto cuyo destino no se sabe: faltan flechas. */
    hasIndirect?: boolean;
    /** true = hay un salto a una etiqueta que no esta en el bloque. */
    hasUnresolved?: boolean;
    /** Mnemonicos de rama que el grafo no supo clasificar. */
    unknownTerminators?: string[];
}

/** Un salto dentro de un bloque, ya resuelto a lineas del fuente. */
export interface AsmJump {
    /** Linea de la que sale, contando desde uno. */
    fromLine: number;
    /** Linea a la que va. */
    toLine: number;
    /** salto | rama | llamada... */
    flow: string;
    /** Etiqueta destino, tal y como esta escrita. */
    target: string;
}

/** Respuesta de `vesta/asmFlow`: el flujo de todos los bloques del fichero. */
export interface AsmFlowResponse extends VestaResponse {
    blocks?: {
        firstLine: number;
        lastLine: number;
        jumps: AsmJump[];
        /**
         * Por donde el flujo SALE del bloque hacia una funcion del modulo.
         *
         * No es un salto entre dos lineas de aqui -- el destino esta fuera --,
         * pero tampoco es nada: sin dibujarlo, un bloque cuyo unico salto va a
         * otra funcion salia sin una sola marca, como si no tuviera flujo.
         */
        exits?: { line: number; symbol: string; flow: string }[];
        /** true = hay un salto cuyo destino no se sabe: faltan flechas. */
        hasIndirect: boolean;
        /** true = hay un salto a una etiqueta LOCAL que no esta en el bloque. */
        hasUnresolved: boolean;
        /** true = algun salto sale a un simbolo del modulo (no es un fallo). */
        hasExternal?: boolean;
    }[];
}

/** Fases del IR que admite `vesta/ir`. */
export type IrPhase = 'pre' | 'post';

/** Vistas que admite `vesta/diagram`. */
export type DiagramKind = 'ast' | 'ir-pre' | 'ir-post' | 'vel' | 'asm';

/** Formatos en los que se puede pedir un diagrama. */
export type DiagramFormat = 'mermaid' | 'graphviz' | 'html';

/** Niveles de runtime que puede asumir la compilacion anticipada. */
export type AotTier = 'bare' | 'embed' | 'full';
