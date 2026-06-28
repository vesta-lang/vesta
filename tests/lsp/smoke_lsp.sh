#!/usr/bin/env bash
#
# VestaVM - smoke test end-to-end del servidor LSP de Vex.
#
# Envia por stdin una secuencia JSON-RPC (initialize + initialized + didOpen
# de un .vex con ERROR + didOpen de un .vex valido + shutdown + exit) al
# binario vesta_lsp y verifica:
#   - que responde a initialize (capabilities).
#   - que publica publishDiagnostics con >=1 diagnostico para el fichero malo.
#   - que publica publishDiagnostics con 0 diagnosticos para el bueno.
#
# Uso:
#   tests/lsp/smoke_lsp.sh <ruta-al-vesta_lsp.exe>
#
# Devuelve 0 si todo pasa, !=0 en fallo.  No depende de Python.

set -u

LSP_BIN="${1:-}"
if [ -z "$LSP_BIN" ] || [ ! -x "$LSP_BIN" ]; then
    echo "uso: $0 <ruta-al-vesta_lsp.exe>" >&2
    echo "  (no se encontro el binario: '$LSP_BIN')" >&2
    exit 2
fi

# --- Construir el flujo de entrada con framing Content-Length ----------------
# Helper: imprime un mensaje JSON enmarcado (Content-Length + CRLF + cuerpo).
# El cuerpo NO debe contener saltos de linea para que la longitud en bytes sea
# exacta y portable entre shells.
emit() {
    local body="$1"
    local len=${#body}
    printf 'Content-Length: %d\r\n\r\n%s' "$len" "$body"
}

# Fuente Vex con un error de compilacion claro (tipo de retorno incompatible:
# la funcion declara i32 pero el cuerpo no devuelve nada coherente, y usa un
# identificador no declarado).  Cualquier error sirve para esperar >=1 diag.
BAD_SRC='i64 main() { return noexiste_xyz + 1; }'
# Fuente Vex valida y SIN diagnosticos.  Se usa i64 (no i32) para evitar el
# warning de narrowing del literal 0 (que por defecto es i64): asi la lista
# de diagnosticos del fichero bueno queda realmente vacia.
GOOD_SRC='i64 main() { return 0; }'

# Construir el JSON de cada mensaje en una sola linea (sin newlines internos).
INIT='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}}'
INITED='{"jsonrpc":"2.0","method":"initialized","params":{}}'
OPEN_BAD="{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"file:///bad.vex\",\"languageId\":\"vex\",\"version\":1,\"text\":\"${BAD_SRC}\"}}}"
OPEN_GOOD="{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"file:///good.vex\",\"languageId\":\"vex\",\"version\":1,\"text\":\"${GOOD_SRC}\"}}}"
SHUTDOWN='{"jsonrpc":"2.0","id":2,"method":"shutdown","params":null}'
EXIT='{"jsonrpc":"2.0","method":"exit","params":null}'

# Generar el stream completo a un fichero temporal.
TMP_IN="$(mktemp 2>/dev/null || echo /tmp/lsp_smoke_in.$$)"
TMP_OUT="$(mktemp 2>/dev/null || echo /tmp/lsp_smoke_out.$$)"
{
    emit "$INIT"
    emit "$INITED"
    emit "$OPEN_BAD"
    emit "$OPEN_GOOD"
    emit "$SHUTDOWN"
    emit "$EXIT"
} > "$TMP_IN"

# Ejecutar el servidor con el stream por stdin; capturar stdout.
"$LSP_BIN" < "$TMP_IN" > "$TMP_OUT" 2>/dev/null

# --- Verificaciones ----------------------------------------------------------
fail=0

# 1) Respuesta a initialize: debe contener "capabilities".
if grep -q '"capabilities"' "$TMP_OUT"; then
    echo "OK  initialize respondido (capabilities presentes)"
else
    echo "FALLO  no se vio respuesta a initialize con capabilities"
    fail=1
fi

# 2) publishDiagnostics para bad.vex con >=1 diagnostico.  Buscamos la
#    notificacion del fichero malo y comprobamos que su lista no esta vacia.
#    Heuristica robusta: en la misma notificacion de bad.vex debe aparecer
#    al menos un objeto de diagnostico (campo "message").
if grep -q 'bad.vex' "$TMP_OUT" && \
   grep 'bad.vex' "$TMP_OUT" | grep -q '"message"'; then
    echo "OK  bad.vex publica >=1 diagnostico"
else
    echo "FALLO  bad.vex no publico diagnosticos"
    fail=1
fi

# 3) publishDiagnostics para good.vex con 0 diagnosticos.  La notificacion del
#    fichero bueno debe llevar "diagnostics":[] (lista vacia).
if grep 'good.vex' "$TMP_OUT" | grep -q '"diagnostics":\[\]'; then
    echo "OK  good.vex publica 0 diagnosticos"
else
    echo "FALLO  good.vex no publico una lista de diagnosticos vacia"
    fail=1
fi

# Limpieza.
rm -f "$TMP_IN" "$TMP_OUT" 2>/dev/null

if [ "$fail" -eq 0 ]; then
    echo "smoke_lsp: TODO OK"
else
    echo "smoke_lsp: HUBO FALLOS"
fi
exit "$fail"
