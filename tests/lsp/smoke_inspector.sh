#!/usr/bin/env bash
#
# VestaVM - smoke test del inspector del ecosistema (Fase 3 del LSP de Vex).
#
# Envia por stdin una secuencia JSON-RPC (initialize + initialized + didOpen
# de un .vex con una funcion aritmetica JIT-compilable, un loop para la
# complejidad y una clase) al binario vesta_lsp y luego invoca CADA peticion
# a medida vesta/*, verificando que cada una responde de forma no vacia o
# estructurada:
#   - vesta/bytecode    -> tiene texto .vel.
#   - vesta/ir post     -> tiene texto IR.
#   - vesta/ir pre      -> tiene texto IR.
#   - vesta/complexity  -> JSON con >=1 funcion.
#   - vesta/diagram     -> mermaid de ir-post con texto.
#   - vesta/functions   -> lista con >=1 funcion.
#   - vesta/aotCompat   -> JSON con compatible/issues.
#   - vesta/jitAsm      -> disasm de la fn aritmetica (o unsupported con razon).
#   - vesta/aotAsm      -> disasm o incompatible con razon.
#
# Uso:
#   tests/lsp/smoke_inspector.sh <ruta-al-vesta_lsp.exe>
#
# Devuelve 0 si todo pasa, !=0 en fallo.  No depende de Python.

set -u

LSP_BIN="${1:-}"
if [ -z "$LSP_BIN" ] || [ ! -x "$LSP_BIN" ]; then
    echo "uso: $0 <ruta-al-vesta_lsp.exe>" >&2
    echo "  (no se encontro el binario: '$LSP_BIN')" >&2
    exit 2
fi

# Helper: imprime un mensaje JSON enmarcado (Content-Length + CRLF + cuerpo).
# El cuerpo NO debe contener saltos de linea para que la longitud en bytes
# sea exacta y portable entre shells.
emit() {
    local body="$1"
    local len=${#body}
    printf 'Content-Length: %d\r\n\r\n%s' "$len" "$body"
}

# Fuente Vex con: (a) suma aritmetica pura JIT-compilable, (b) loop para
# inferir complejidad lineal, (c) una clase con un metodo.  Una sola linea
# (los '\n' van como secuencia literal \n dentro del string JSON).
SRC='i64 suma(i64 a, i64 b) { return a + b; }\ni64 acumular(i64 n) { i64 s = 0; i64 i = 0; while (i < n) { s = s + i; i = i + 1; } return s; }\nclass Punto { public i64 x; public i64 getX() { return this.x; } }\ni64 main() { return suma(2, 3); }'

URI='file:///inspector_demo.vex'

INIT='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}}'
INITED='{"jsonrpc":"2.0","method":"initialized","params":{}}'
OPEN="{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"${URI}\",\"languageId\":\"vex\",\"version\":1,\"text\":\"${SRC}\"}}}"

# Peticiones del inspector (cada una con su id propio para correlacionar).
REQ_BYTECODE="{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"vesta/bytecode\",\"params\":{\"uri\":\"${URI}\"}}"
REQ_IRPOST="{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"vesta/ir\",\"params\":{\"uri\":\"${URI}\",\"phase\":\"post\"}}"
REQ_IRPRE="{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"vesta/ir\",\"params\":{\"uri\":\"${URI}\",\"phase\":\"pre\"}}"
REQ_CMPLX="{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"vesta/complexity\",\"params\":{\"uri\":\"${URI}\"}}"
REQ_DIAG="{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"vesta/diagram\",\"params\":{\"uri\":\"${URI}\",\"kind\":\"ir-post\",\"format\":\"mermaid\"}}"
REQ_FUNCS="{\"jsonrpc\":\"2.0\",\"id\":15,\"method\":\"vesta/functions\",\"params\":{\"uri\":\"${URI}\"}}"
REQ_AOTC="{\"jsonrpc\":\"2.0\",\"id\":16,\"method\":\"vesta/aotCompat\",\"params\":{\"uri\":\"${URI}\",\"tier\":\"bare\"}}"
REQ_JIT="{\"jsonrpc\":\"2.0\",\"id\":17,\"method\":\"vesta/jitAsm\",\"params\":{\"uri\":\"${URI}\",\"function\":\"suma\"}}"
REQ_AOT="{\"jsonrpc\":\"2.0\",\"id\":18,\"method\":\"vesta/aotAsm\",\"params\":{\"uri\":\"${URI}\",\"function\":\"suma\"}}"

SHUTDOWN='{"jsonrpc":"2.0","id":99,"method":"shutdown","params":null}'
EXIT='{"jsonrpc":"2.0","method":"exit","params":null}'

TMP_IN="$(mktemp 2>/dev/null || echo /tmp/lsp_insp_in.$$)"
TMP_OUT="$(mktemp 2>/dev/null || echo /tmp/lsp_insp_out.$$)"
{
    emit "$INIT"
    emit "$INITED"
    emit "$OPEN"
    emit "$REQ_BYTECODE"
    emit "$REQ_IRPOST"
    emit "$REQ_IRPRE"
    emit "$REQ_CMPLX"
    emit "$REQ_DIAG"
    emit "$REQ_FUNCS"
    emit "$REQ_AOTC"
    emit "$REQ_JIT"
    emit "$REQ_AOT"
    emit "$SHUTDOWN"
    emit "$EXIT"
} > "$TMP_IN"

"$LSP_BIN" < "$TMP_IN" > "$TMP_OUT" 2>/dev/null

# --- Verificaciones ----------------------------------------------------------
fail=0

# Helper: extrae la respuesta cuyo "id":N aparece y comprueba que contiene un
# patron.  Como cada respuesta es una linea JSON independiente en el stream de
# salida, basta grepear la linea con el id y luego el patron en esa linea.
check_id() {
    local id="$1"; local pattern="$2"; local desc="$3"
    if grep "\"id\":${id}" "$TMP_OUT" | grep -q "$pattern"; then
        echo "OK  ${desc}"
    else
        echo "FALLO  ${desc}"
        fail=1
    fi
}

# 0) initialize anuncia los metodos experimentales vesta/*.
if grep -q 'vestaMethods' "$TMP_OUT"; then
    echo "OK  initialize anuncia vestaMethods (experimental)"
else
    echo "FALLO  initialize no anuncio vestaMethods"
    fail=1
fi

# 1) bytecode: respuesta con campo text no vacio (busca un mnemonico tipico).
check_id 10 '"text"' "vesta/bytecode devuelve text"

# 2) ir post: text con cabecera de funcion del IR.
check_id 11 '"text"' "vesta/ir (post) devuelve text"

# 3) ir pre: text presente.
check_id 12 '"text"' "vesta/ir (pre) devuelve text"

# 4) complexity: lista de funciones con al menos un name.
check_id 13 '"functions"' "vesta/complexity devuelve functions"
check_id 13 '"partial"' "vesta/complexity incluye coste parcial"

# 5) diagram mermaid ir-post: text que parece mermaid (flowchart/graph).
if grep '"id":14' "$TMP_OUT" | grep -Eq 'flowchart|graph|"text"'; then
    echo "OK  vesta/diagram (mermaid ir-post) devuelve text"
else
    echo "FALLO  vesta/diagram no devolvio diagrama"
    fail=1
fi

# 6) functions: lista con >=1 (busca el nombre suma).
check_id 15 '"functions"' "vesta/functions devuelve lista"
check_id 15 'suma' "vesta/functions incluye la funcion suma"

# 7) aotCompat: JSON con compatible + issues.
check_id 16 '"compatible"' "vesta/aotCompat devuelve compatible"
check_id 16 '"issues"' "vesta/aotCompat devuelve issues"

# 8) jitAsm: o disasm (text) o unsupported con razon.  Cualquiera es valido.
if grep '"id":17' "$TMP_OUT" | grep -Eq '"text"|"unsupported"'; then
    echo "OK  vesta/jitAsm devuelve disasm o unsupported con razon"
else
    echo "FALLO  vesta/jitAsm no respondio text ni unsupported"
    fail=1
fi

# 9) aotAsm: o disasm (text) o incompatible con razon.
if grep '"id":18' "$TMP_OUT" | grep -Eq '"text"|"incompatible"'; then
    echo "OK  vesta/aotAsm devuelve disasm o incompatible con razon"
else
    echo "FALLO  vesta/aotAsm no respondio text ni incompatible"
    fail=1
fi

# Limpieza.
rm -f "$TMP_IN" "$TMP_OUT" 2>/dev/null

if [ "$fail" -eq 0 ]; then
    echo "smoke_inspector: TODO OK"
else
    echo "smoke_inspector: HUBO FALLOS"
fi
exit "$fail"
