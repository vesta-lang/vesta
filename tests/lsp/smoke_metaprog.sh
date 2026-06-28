#!/usr/bin/env bash
#
# VestaVM - smoke test de la metaprogramacion en el LSP (Fase 3.5 de Vex).
#
# Envia por stdin una secuencia JSON-RPC (initialize + initialized + didOpen
# de un .vex con una constante comptime entera, una constante comptime
# derivada y un @Macro que GENERA codigo Vex) al binario vesta_lsp y luego
# invoca las DOS peticiones a medida nuevas, verificando que responden de
# forma estructurada:
#   - vesta/macroExpand    -> >=1 expansion con generated_code no vacio.
#   - vesta/comptimeValues -> >=1 valor con su value_str.
#
# Uso:
#   tests/lsp/smoke_metaprog.sh <ruta-al-vesta_lsp.exe>
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

# Fuente Vex con:
#   (a) constante comptime entera ANSWER = 42,
#   (b) constante comptime derivada DOBLE = ANSWER * 2 (computada en compile
#       time),
#   (c) un @Macro `macro_double(n)` que devuelve el codigo Vex "doblar(n)".
# Las comillas dobles internas del cuerpo del macro se escapan como \" para
# que el JSON del didOpen siga siendo valido (van como bytes literales \" en
# el texto del documento).  Una sola linea (los '\n' van como secuencia
# literal \n dentro del string JSON).
SRC='comptime i64 ANSWER = 42;\ncomptime i64 DOBLE = ANSWER * 2;\ni32 doblar(i32 x) { return x * 2; }\n@Macro\ncomptime string macro_double(i64 n) { return \"doblar(\" + to_str(n) + \")\"; }\ni32 main() { i32 r1 = macro_double(21); if (r1 != 42) return 91; return 42; }'

URI='file:///metaprog_demo.vex'

INIT='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}}'
INITED='{"jsonrpc":"2.0","method":"initialized","params":{}}'
OPEN="{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"${URI}\",\"languageId\":\"vex\",\"version\":1,\"text\":\"${SRC}\"}}}"

# Peticiones nuevas del inspector (cada una con su id propio).
REQ_MACRO="{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"vesta/macroExpand\",\"params\":{\"uri\":\"${URI}\"}}"
REQ_CVALS="{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"vesta/comptimeValues\",\"params\":{\"uri\":\"${URI}\"}}"

SHUTDOWN='{"jsonrpc":"2.0","id":99,"method":"shutdown","params":null}'
EXIT='{"jsonrpc":"2.0","method":"exit","params":null}'

TMP_IN="$(mktemp 2>/dev/null || echo /tmp/lsp_meta_in.$$)"
TMP_OUT="$(mktemp 2>/dev/null || echo /tmp/lsp_meta_out.$$)"
{
    emit "$INIT"
    emit "$INITED"
    emit "$OPEN"
    emit "$REQ_MACRO"
    emit "$REQ_CVALS"
    emit "$SHUTDOWN"
    emit "$EXIT"
} > "$TMP_IN"

"$LSP_BIN" < "$TMP_IN" > "$TMP_OUT" 2>/dev/null

# --- Verificaciones ----------------------------------------------------------
fail=0

check_id() {
    local id="$1"; local pattern="$2"; local desc="$3"
    if grep "\"id\":${id}" "$TMP_OUT" | grep -q "$pattern"; then
        echo "OK  ${desc}"
    else
        echo "FALLO  ${desc}"
        fail=1
    fi
}

# 0) initialize anuncia los metodos nuevos.
if grep -q 'vesta/macroExpand' "$TMP_OUT" && grep -q 'vesta/comptimeValues' "$TMP_OUT"; then
    echo "OK  initialize anuncia macroExpand + comptimeValues"
else
    echo "FALLO  initialize no anuncio los metodos nuevos"
    fail=1
fi

# 1) macroExpand: estructura con expansions + skipped, y >=1 expansion cuyo
#    generated_code es "doblar(21)" (lo que produce macro_double(21)).
check_id 20 '"expansions"' "vesta/macroExpand devuelve expansions"
check_id 20 '"skipped"' "vesta/macroExpand devuelve skipped"
check_id 20 'doblar(21)' "vesta/macroExpand incluye el codigo generado"

# 2) comptimeValues: lista con >=1 valor.  ANSWER=42 y DOBLE=84 deben aparecer
#    con sus value_str.
check_id 21 '"values"' "vesta/comptimeValues devuelve values"
check_id 21 'ANSWER' "vesta/comptimeValues incluye la constante ANSWER"
check_id 21 '"42"' "vesta/comptimeValues muestra el valor 42"
check_id 21 '"84"' "vesta/comptimeValues computa DOBLE = 84"

# Limpieza.
rm -f "$TMP_IN" "$TMP_OUT" 2>/dev/null

if [ "$fail" -eq 0 ]; then
    echo "smoke_metaprog: TODO OK"
else
    echo "smoke_metaprog: HUBO FALLOS"
fi
exit "$fail"
