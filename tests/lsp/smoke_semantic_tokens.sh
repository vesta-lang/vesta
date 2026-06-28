#!/usr/bin/env bash
#
# VestaVM - smoke test del resaltado semantico (semantic tokens) del LSP.
#
# Envia por stdin (framing Content-Length) una secuencia JSON-RPC:
#   initialize + initialized + didOpen de un .vex variado + semanticTokens/full
#   + shutdown + exit
# y verifica:
#   - que initialize anuncia "semanticTokensProvider" con su "legend".
#   - que la respuesta a semanticTokens/full trae un array "data" no vacio,
#     de longitud multiplo de 5, con deltas no negativos, y que contiene los
#     indices de tokenType esperados (keyword, type, string, number, comment,
#     y class/function para el identificador enriquecido).
#
# Uso:
#   tests/lsp/smoke_semantic_tokens.sh <ruta-al-vesta_lsp.exe>
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
emit() {
    local body="$1"
    local len=${#body}
    printf 'Content-Length: %d\r\n\r\n%s' "$len" "$body"
}

# Fuente Vex con: keyword (class/return), tipo primitivo (i32), una clase
# declarada (Punto) usada como tipo, un string, un numero, un comentario de
# linea y uno de bloque.  En una sola linea por mensaje (sin saltos internos)
# salvo los \n explicitos que el servidor recibe como parte del texto.
# Se construye con \n literales escapados para JSON.
VEX_SRC='// comentario de linea\nclass Punto { i32 x; i32 y; }\n/* bloque\n multilinea */\ni32 area(Punto p) { return 42; }\nstring saludo() { return \"hola\"; }'

INIT='{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"processId":null,"rootUri":null,"capabilities":{}}}'
INITED='{"jsonrpc":"2.0","method":"initialized","params":{}}'
OPEN="{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"file:///sem.vex\",\"languageId\":\"vex\",\"version\":1,\"text\":\"${VEX_SRC}\"}}}"
SEMTOK='{"jsonrpc":"2.0","id":2,"method":"textDocument/semanticTokens/full","params":{"textDocument":{"uri":"file:///sem.vex"}}}'
SHUTDOWN='{"jsonrpc":"2.0","id":3,"method":"shutdown","params":null}'
EXIT='{"jsonrpc":"2.0","method":"exit","params":null}'

TMP_IN="$(mktemp 2>/dev/null || echo /tmp/lsp_sem_in.$$)"
TMP_OUT="$(mktemp 2>/dev/null || echo /tmp/lsp_sem_out.$$)"
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

# 1) initialize anuncia semanticTokensProvider + legend.
if grep -q '"semanticTokensProvider"' "$TMP_OUT" && \
   grep -q '"tokenTypes"' "$TMP_OUT"; then
    echo "OK  initialize anuncia semanticTokensProvider con legend"
else
    echo "FALLO  initialize NO anuncio semanticTokensProvider/legend"
    fail=1
fi

# 2) Extraer el array "data" de la respuesta a semanticTokens/full.  Tomamos
#    la ultima linea que contenga "data" (la respuesta a la id=2).  El array
#    es plano de enteros: [d,d,d,...].
DATA_LINE="$(grep '"data"' "$TMP_OUT" | tail -1)"
# Aislar el contenido del array tras "data":[ ... ].
NUMS="$(printf '%s' "$DATA_LINE" | sed -n 's/.*"data":\[\([0-9,]*\)\].*/\1/p')"

if [ -z "$NUMS" ]; then
    echo "FALLO  no se encontro un array data no vacio en la respuesta"
    fail=1
else
    # Contar elementos.
    COUNT="$(printf '%s' "$NUMS" | tr ',' '\n' | grep -c '[0-9]')"
    if [ "$COUNT" -gt 0 ] && [ $((COUNT % 5)) -eq 0 ]; then
        echo "OK  data tiene $COUNT enteros (multiplo de 5)"
    else
        echo "FALLO  data no es multiplo de 5 (count=$COUNT)"
        fail=1
    fi

    # Verificar deltas no negativos (todos los enteros del array son uint;
    # el sed ya excluye signos negativos, asi que un '-' rompe el match).
    if printf '%s' "$NUMS" | grep -q '\-'; then
        echo "FALLO  data contiene valores negativos"
        fail=1
    else
        echo "OK  data sin valores negativos"
    fi

    # Verificar que aparecen los indices de tokenType esperados.  El indice es
    # el 4o elemento de cada quinteto (posicion 3, 0-based).  Indices segun la
    # leyenda: type=1, class=2, enum=3, struct=5, function=11, keyword=13,
    # comment=15, string=16, number=17.
    # Extraemos cada 4o valor (tokenType) y comprobamos presencia.
    TYPES="$(printf '%s' "$NUMS" | tr ',' '\n' | awk 'NR%5==4{print}')"
    check_type() {
        local idx="$1"; local name="$2"
        if printf '%s\n' "$TYPES" | grep -qx "$idx"; then
            echo "OK  presente tokenType $name (idx=$idx)"
        else
            echo "FALLO  ausente tokenType $name (idx=$idx)"
            fail=1
        fi
    }
    check_type 13 "keyword"
    check_type 1  "type"
    check_type 16 "string"
    check_type 17 "number"
    check_type 15 "comment"
    check_type 2  "class"
    check_type 11 "function"
fi

rm -f "$TMP_IN" "$TMP_OUT" 2>/dev/null

if [ "$fail" -eq 0 ]; then
    echo "smoke_semantic_tokens: TODO OK"
else
    echo "smoke_semantic_tokens: HUBO FALLOS"
fi
exit "$fail"
