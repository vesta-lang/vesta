#!/usr/bin/env bash
#
# VestaVM - smoke test de la navegacion del servidor LSP de Vex (Fase 4).
#
# Monta un mini-workspace en disco con una "libreria" lib_mat.vex (define la
# funcion cuadrado) y un main_use.vex que la usa dos veces.  Lanza vesta_lsp,
# fija el rootUri al workspace y verifica:
#   - hover sobre la llamada a 'cuadrado' devuelve contents no vacios que
#     mencionan que es una funcion y su complejidad Big-O.
#   - definition sobre la llamada apunta a lib_mat.vex (CROSS-FILE).
#   - references sobre 'cuadrado' (includeDeclaration=true) devuelve >=3
#     Locations repartidas entre los dos ficheros.
#
# Uso:
#   tests/lsp/smoke_navigation.sh <ruta-al-vesta_lsp.exe>
#
# Devuelve 0 si todo pasa, !=0 en fallo.  No depende de Python.

set -u

LSP_BIN="${1:-}"
if [ -z "$LSP_BIN" ] || [ ! -x "$LSP_BIN" ]; then
    echo "uso: $0 <ruta-al-vesta_lsp.exe>" >&2
    echo "  (no se encontro el binario: '$LSP_BIN')" >&2
    exit 2
fi

# --- Montar el workspace en disco -------------------------------------------
# Se crea bajo el directorio del propio test para tener una ruta estable que
# el binario nativo (Windows) pueda abrir.  Limpieza al final.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WS_DIR="$SCRIPT_DIR/.tmp_nav"
rm -rf "$WS_DIR"
mkdir -p "$WS_DIR"

# Libreria: define 'cuadrado'.  Se usa i64 para evitar warnings de narrowing.
cat > "$WS_DIR/lib_mat.vex" <<'EOF'
i64 cuadrado(i64 x) {
    return x * x;
}
EOF

# Consumidor: usa 'cuadrado' dos veces.
cat > "$WS_DIR/main_use.vex" <<'EOF'
i64 main() {
    i64 a = cuadrado(3);
    i64 b = cuadrado(4);
    return a + b;
}
EOF

# Rutas en formato que entienda el binario nativo + URIs file://.
if command -v cygpath >/dev/null 2>&1; then
    WS_WIN="$(cygpath -w "$WS_DIR")"
    # cygpath da F:\C\VM\...; pasamos a barras normales para la URI.
    WS_URI_PATH="$(printf '%s' "$WS_WIN" | sed 's#\\#/#g')"
    ROOT_URI="file:///$WS_URI_PATH"
    MAIN_URI="file:///$WS_URI_PATH/main_use.vex"
    LIB_URI="file:///$WS_URI_PATH/lib_mat.vex"
else
    # POSIX: el path ya es absoluto con barras.
    ROOT_URI="file://$WS_DIR"
    MAIN_URI="file://$WS_DIR/main_use.vex"
    LIB_URI="file://$WS_DIR/lib_mat.vex"
fi

# Texto de main_use.vex en una sola linea (con \n escapados) para el didOpen.
MAIN_TEXT='i64 main() {\n    i64 a = cuadrado(3);\n    i64 b = cuadrado(4);\n    return a + b;\n}\n'

# --- Construir el flujo JSON-RPC --------------------------------------------
emit() {
    local body="$1"
    local len=${#body}
    printf 'Content-Length: %d\r\n\r\n%s' "$len" "$body"
}

# La llamada 'cuadrado' en main_use.vex esta en la linea 1 (0-based), columna
# 12 (0-based): "    i64 a = cuadrado(3);" -> 4 espacios + "i64 a = " = 12.
INIT="{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":\"${ROOT_URI}\",\"capabilities\":{}}}"
INITED='{"jsonrpc":"2.0","method":"initialized","params":{}}'
OPEN_MAIN="{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"${MAIN_URI}\",\"languageId\":\"vex\",\"version\":1,\"text\":\"${MAIN_TEXT}\"}}}"
HOVER="{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/hover\",\"params\":{\"textDocument\":{\"uri\":\"${MAIN_URI}\"},\"position\":{\"line\":1,\"character\":13}}}"
DEFIN="{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"textDocument/definition\",\"params\":{\"textDocument\":{\"uri\":\"${MAIN_URI}\"},\"position\":{\"line\":1,\"character\":13}}}"
REFS="{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"textDocument/references\",\"params\":{\"textDocument\":{\"uri\":\"${MAIN_URI}\"},\"position\":{\"line\":1,\"character\":13},\"context\":{\"includeDeclaration\":true}}}"
SHUTDOWN='{"jsonrpc":"2.0","id":9,"method":"shutdown","params":null}'
EXIT='{"jsonrpc":"2.0","method":"exit","params":null}'

TMP_IN="$(mktemp 2>/dev/null || echo /tmp/lsp_nav_in.$$)"
TMP_OUT="$(mktemp 2>/dev/null || echo /tmp/lsp_nav_out.$$)"
{
    emit "$INIT"
    emit "$INITED"
    emit "$OPEN_MAIN"
    emit "$HOVER"
    emit "$DEFIN"
    emit "$REFS"
    emit "$SHUTDOWN"
    emit "$EXIT"
} > "$TMP_IN"

"$LSP_BIN" < "$TMP_IN" > "$TMP_OUT" 2>/dev/null

# --- Verificaciones ----------------------------------------------------------
fail=0

# 1) Respuesta de hover (id=2) con contents no vacios mencionando funcion y
#    Big-O.  Heuristica robusta sobre el JSON en linea: el bloque de respuesta
#    con "id":2 debe contener "cuadrado", "funcion" y "Complejidad".
if grep -q '"id":2' "$TMP_OUT" && \
   grep '"id":2' "$TMP_OUT" | grep -q 'cuadrado' && \
   grep '"id":2' "$TMP_OUT" | grep -qi 'funcion' && \
   grep '"id":2' "$TMP_OUT" | grep -qi 'Complejidad'; then
    echo "OK  hover devuelve funcion + Big-O"
else
    echo "FALLO  hover no devolvio funcion/Big-O esperados"
    grep '"id":2' "$TMP_OUT" >&2 || true
    fail=1
fi

# 2) definition (id=3) apunta a lib_mat.vex (cross-file).
if grep -q '"id":3' "$TMP_OUT" && \
   grep '"id":3' "$TMP_OUT" | grep -q 'lib_mat.vex'; then
    echo "OK  definition cross-file (lib_mat.vex)"
else
    echo "FALLO  definition no apunto a lib_mat.vex"
    grep '"id":3' "$TMP_OUT" >&2 || true
    fail=1
fi

# 3) references (id=4) con >=3 Locations repartidas en los dos ficheros.
#    Contamos las apariciones de "uri" en la respuesta id=4 (cada Location
#    lleva un campo uri) y exigimos >=3, y que aparezcan ambos ficheros.
REF_LINE="$(grep '"id":4' "$TMP_OUT")"
URI_COUNT="$(printf '%s' "$REF_LINE" | grep -o '"uri"' | wc -l | tr -d ' ')"
if [ "${URI_COUNT:-0}" -ge 3 ] && \
   printf '%s' "$REF_LINE" | grep -q 'main_use.vex' && \
   printf '%s' "$REF_LINE" | grep -q 'lib_mat.vex'; then
    echo "OK  references cross-file (>=3 locations: ${URI_COUNT}, ambos ficheros)"
else
    echo "FALLO  references insuficientes o no cross-file (count=${URI_COUNT:-0})"
    printf '%s\n' "$REF_LINE" >&2 || true
    fail=1
fi

# Limpieza.
rm -f "$TMP_IN" "$TMP_OUT" 2>/dev/null
rm -rf "$WS_DIR" 2>/dev/null

if [ "$fail" -eq 0 ]; then
    echo "smoke_navigation: TODO OK"
else
    echo "smoke_navigation: HUBO FALLOS"
fi
exit "$fail"
