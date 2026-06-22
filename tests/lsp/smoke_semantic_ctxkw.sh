#!/usr/bin/env bash
#
# VestaVM - smoke test del resaltado de keywords contextuales, builtins y
# parametros de plantilla en el LSP de Vex.
#
# Verifica que el clasificador del LSP colorea correctamente:
#   - `comptime`      -> Keyword       (idx 13)
#   - `static_assert` -> Function      (idx 11)
#   - `T` (en <T>)    -> TypeParameter (idx 6)
#   - un identificador normal          -> Variable (idx 8)
#
# Decodifica el array plano "data" (quintetos con deltas) a posiciones
# absolutas (linea, caracter) y comprueba el tokenType en cada posicion
# conocida del fuente.
#
# Uso:
#   tests/lsp/smoke_semantic_ctxkw.sh <ruta-al-vesta_lsp.exe>
#
# Devuelve 0 si todo pasa, !=0 en fallo.  No depende de Python.

set -u

LSP_BIN="${1:-}"
if [ -z "$LSP_BIN" ] || [ ! -x "$LSP_BIN" ]; then
    echo "uso: $0 <ruta-al-vesta_lsp.exe>" >&2
    echo "  (no se encontro el binario: '$LSP_BIN')" >&2
    exit 2
fi

emit() {
    local body="$1"
    local len=${#body}
    printf 'Content-Length: %d\r\n\r\n%s' "$len" "$body"
}

# Fuente Vex (lineas 0-based tras el split por \n):
#   L0: comptime <T> u32 vec_dim() { return 4; }
#   L1: i32 main() { static_assert(true, "ok"); i32 normal = 1; return 0; }
# `comptime` empieza en col 0 de L0.  `T` esta en col 10 de L0 (tras
# "comptime <").  `static_assert` empieza en col 16 de L1 (tras
# "i32 main() { ").  `normal` empieza en col 47 de L1.
VEX_SRC='comptime <T> u32 vec_dim() { return 4; }\ni32 main() { static_assert(true, \"ok\"); i32 normal = 1; return 0; }'

INIT='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}}'
INITED='{"jsonrpc":"2.0","method":"initialized","params":{}}'
OPEN="{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"file:///ctxkw.vex\",\"languageId\":\"vex\",\"version\":1,\"text\":\"${VEX_SRC}\"}}}"
SEMTOK='{"jsonrpc":"2.0","id":2,"method":"textDocument/semanticTokens/full","params":{"textDocument":{"uri":"file:///ctxkw.vex"}}}'
SHUTDOWN='{"jsonrpc":"2.0","id":3,"method":"shutdown","params":null}'
EXIT='{"jsonrpc":"2.0","method":"exit","params":null}'

TMP_IN="$(mktemp 2>/dev/null || echo /tmp/lsp_ctx_in.$$)"
TMP_OUT="$(mktemp 2>/dev/null || echo /tmp/lsp_ctx_out.$$)"
{
    emit "$INIT"
    emit "$INITED"
    emit "$OPEN"
    emit "$SEMTOK"
    emit "$SHUTDOWN"
    emit "$EXIT"
} > "$TMP_IN"

"$LSP_BIN" < "$TMP_IN" > "$TMP_OUT" 2>/dev/null

fail=0

DATA_LINE="$(grep '"data"' "$TMP_OUT" | tail -1)"
NUMS="$(printf '%s' "$DATA_LINE" | sed -n 's/.*"data":\[\([0-9,]*\)\].*/\1/p')"

if [ -z "$NUMS" ]; then
    echo "FALLO  no se encontro un array data no vacio en la respuesta"
    rm -f "$TMP_IN" "$TMP_OUT" 2>/dev/null
    exit 1
fi

# Decodificar los quintetos a posiciones absolutas con awk y volcar lineas
# "line col type" para consultar.  deltaLine/deltaStart son relativos al
# token previo; si deltaLine==0 el caracter es relativo al previo.
DECODED="$(printf '%s' "$NUMS" | tr ',' '\n' | awk '
{
    a[NR-1] = $1
}
END {
    line = 0; col = 0;
    for (i = 0; i + 4 < NR; i += 5) {
        dl = a[i]; ds = a[i+1]; len = a[i+2]; typ = a[i+3];
        if (dl == 0) { col = col + ds; } else { line = line + dl; col = ds; }
        print line, col, typ;
    }
}')"

# Busca el tokenType en (line, col).  Devuelve "" si no hay token ahi.
type_at() {
    local ln="$1"; local cl="$2"
    printf '%s\n' "$DECODED" | awk -v L="$ln" -v C="$cl" \
        '$1==L && $2==C { print $3; exit }'
}

check_at() {
    local ln="$1"; local cl="$2"; local want="$3"; local label="$4"
    local got
    got="$(type_at "$ln" "$cl")"
    if [ "$got" = "$want" ]; then
        echo "OK  $label en ${ln}:${cl} = tokenType $got"
    else
        echo "FALLO  $label en ${ln}:${cl}: esperado $want, obtenido '${got}'"
        fail=1
    fi
}

# comptime -> Keyword(13) en 0:0.
check_at 0 0 13 "comptime (Keyword)"
# T -> TypeParameter(6) en 0:10.
check_at 0 10 6 "T (TypeParameter)"
# static_assert -> Function(11) en 1:13.
check_at 1 13 11 "static_assert (Function)"
# normal -> Variable(8) en 1:44.
check_at 1 44 8 "normal (Variable)"

rm -f "$TMP_IN" "$TMP_OUT" 2>/dev/null

if [ "$fail" -eq 0 ]; then
    echo "smoke_semantic_ctxkw: TODO OK"
else
    echo "smoke_semantic_ctxkw: HUBO FALLOS"
fi
exit "$fail"
