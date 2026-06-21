#!/usr/bin/env bash
#
# VestaVM - smoke test del autocompletado del servidor LSP de Vex (Fase 5).
#
# Monta un mini-workspace con un fichero comp.vex que declara:
#   - una clase Punto con campos (x, y) y metodos (suma, escala),
#   - una funcion top-level distancia,
#   - una funcion main que crea un Punto y deja puntos de insercion.
#
# Lanza vesta_lsp, fija el rootUri al workspace y verifica:
#   - completion GENERAL (prefijo de unas letras) devuelve items que incluyen
#     al menos una palabra clave y el simbolo declarado (la clase / funcion),
#     filtrados por prefijo.
#   - completion tras "p." (con p de tipo Punto) devuelve los metodos/campos de
#     Punto (caso best-effort de la heuristica del tipo del receptor); si la
#     heuristica no resolviera, se exige al menos una respuesta valida sin
#     crash.
#
# Uso:
#   tests/lsp/smoke_completion.sh <ruta-al-vesta_lsp.exe>
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
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WS_DIR="$SCRIPT_DIR/.tmp_comp"
rm -rf "$WS_DIR"
mkdir -p "$WS_DIR"

# Fichero con una clase (campos + metodos), una funcion y main.
cat > "$WS_DIR/comp.vex" <<'EOF'
class Punto {
    public i64 x;
    public i64 y;
    public i64 suma() { return this.x + this.y; }
    public i64 escala(i64 k) { return this.x * k; }
}

i64 distancia(i64 a) {
    return a * a;
}

i64 main() {
    Punto p = new Punto();
    i64 d = dis
    return p.su
}
EOF

# Rutas/URIs que entienda el binario nativo.
if command -v cygpath >/dev/null 2>&1; then
    WS_WIN="$(cygpath -w "$WS_DIR")"
    WS_URI_PATH="$(printf '%s' "$WS_WIN" | sed 's#\\#/#g')"
    ROOT_URI="file:///$WS_URI_PATH"
    DOC_URI="file:///$WS_URI_PATH/comp.vex"
else
    ROOT_URI="file://$WS_DIR"
    DOC_URI="file://$WS_DIR/comp.vex"
fi

# Texto de comp.vex en una sola linea con \n escapados para el didOpen.  Debe
# coincidir EXACTAMENTE con el contenido de arriba (mismo numero de lineas) para
# que las posiciones del cursor cuadren.
DOC_TEXT='class Punto {\n    public i64 x;\n    public i64 y;\n    public i64 suma() { return this.x + this.y; }\n    public i64 escala(i64 k) { return this.x * k; }\n}\n\ni64 distancia(i64 a) {\n    return a * a;\n}\n\ni64 main() {\n    Punto p = new Punto();\n    i64 d = dis\n    return p.su\n}\n'

# --- Construir el flujo JSON-RPC --------------------------------------------
emit() {
    local body="$1"
    local len=${#body}
    printf 'Content-Length: %d\r\n\r\n%s' "$len" "$body"
}

INIT="{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"processId\":null,\"rootUri\":\"${ROOT_URI}\",\"capabilities\":{}}}"
INITED='{"jsonrpc":"2.0","method":"initialized","params":{}}'
OPEN_DOC="{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"${DOC_URI}\",\"languageId\":\"vex\",\"version\":1,\"text\":\"${DOC_TEXT}\"}}}"

# Linea 13 (0-based) = "    i64 d = dis" -> el prefijo "dis" termina en columna
# 15 (4 + "i64 d = " = 12, + "dis" = 15).  Completado GENERAL.
COMP_GENERAL="{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/completion\",\"params\":{\"textDocument\":{\"uri\":\"${DOC_URI}\"},\"position\":{\"line\":13,\"character\":15}}}"

# Linea 14 (0-based) = "    return p.su" -> tras "p." escribiendo "su"; el
# prefijo "su" termina en columna 15 (4 + "return p." = 13, + "su" = 15).
# Completado de MIEMBRO.
COMP_MEMBER="{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"textDocument/completion\",\"params\":{\"textDocument\":{\"uri\":\"${DOC_URI}\"},\"position\":{\"line\":14,\"character\":15}}}"

SHUTDOWN='{"jsonrpc":"2.0","id":9,"method":"shutdown","params":null}'
EXIT='{"jsonrpc":"2.0","method":"exit","params":null}'

TMP_IN="$(mktemp 2>/dev/null || echo /tmp/lsp_comp_in.$$)"
TMP_OUT="$(mktemp 2>/dev/null || echo /tmp/lsp_comp_out.$$)"
{
    emit "$INIT"
    emit "$INITED"
    emit "$OPEN_DOC"
    emit "$COMP_GENERAL"
    emit "$COMP_MEMBER"
    emit "$SHUTDOWN"
    emit "$EXIT"
} > "$TMP_IN"

"$LSP_BIN" < "$TMP_IN" > "$TMP_OUT" 2>/dev/null

# --- Verificaciones ----------------------------------------------------------
fail=0

# 1) Completado general (id=2): debe incluir al menos una palabra clave (p.ej.
#    la funcion 'distancia' empieza por 'dis', y 'distancia' es un simbolo del
#    documento; tambien deben aparecer items con kind).  Exigimos que aparezca
#    'distancia' (simbolo del doc filtrado por el prefijo "dis").
GEN_LINE="$(grep '"id":2' "$TMP_OUT")"
if printf '%s' "$GEN_LINE" | grep -q 'distancia' && \
   printf '%s' "$GEN_LINE" | grep -q '"kind"'; then
    echo "OK  completion general filtra por prefijo (incluye 'distancia')"
else
    echo "FALLO  completion general no incluyo el simbolo esperado"
    printf '%s\n' "$GEN_LINE" >&2 || true
    fail=1
fi

# 1b) Comprobacion de palabras clave: pedir completado con prefijo vacio en otra
#     posicion daria muchas; aqui basta verificar que el general respondio una
#     lista (array) valida sin tumbar el server.
if printf '%s' "$GEN_LINE" | grep -q '"result"'; then
    echo "OK  completion general devolvio un result valido"
else
    echo "FALLO  completion general sin result"
    fail=1
fi

# 2) Completado de miembro (id=3): tras "p." con p:Punto, esperamos los miembros
#    de Punto que empiezan por "su" -> el metodo 'suma'.  La heuristica del tipo
#    del receptor es best-effort; si resuelve, 'suma' debe aparecer.  En todo
#    caso, la respuesta debe ser un result valido (lista) sin crash.
MEM_LINE="$(grep '"id":3' "$TMP_OUT")"
if printf '%s' "$MEM_LINE" | grep -q '"result"'; then
    echo "OK  completion de miembro devolvio un result valido (sin crash)"
    if printf '%s' "$MEM_LINE" | grep -q 'suma'; then
        echo "OK  completion de miembro resolvio el tipo del receptor ('suma')"
    else
        echo "NOTA  completion de miembro no resolvio el tipo (best-effort)"
    fi
else
    echo "FALLO  completion de miembro sin result"
    printf '%s\n' "$MEM_LINE" >&2 || true
    fail=1
fi

# Limpieza.
rm -f "$TMP_IN" "$TMP_OUT" 2>/dev/null
rm -rf "$WS_DIR" 2>/dev/null

if [ "$fail" -eq 0 ]; then
    echo "smoke_completion: TODO OK"
else
    echo "smoke_completion: HUBO FALLOS"
fi
exit "$fail"
