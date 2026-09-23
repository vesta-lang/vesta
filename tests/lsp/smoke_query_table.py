#!/usr/bin/env python3
"""VestaVM - el catalogo de consultas ANUNCIADO y el DESPACHADO son el mismo.

Un servidor puede anunciar una consulta que no atiende, o atender una que no
anuncia, y ninguna de las dos cosas se nota nunca: el cliente que descubre por
la lista simplemente no la usa, y el que la conoce de memoria la usa igual.  Es
un fallo MUDO, y ya habia pasado -- `vesta/irDiff`, `vesta/paramHints` y
`vesta/symbolInfo` se despachaban sin estar anunciadas, porque el anuncio y el
despacho eran dos listas escritas a mano.

Ahora las dos salen de la misma tabla (`query::Registry`), asi que la prueba es
que eso siga siendo verdad EJECUTANDO el servidor:

  1. Se pide `initialize` y se lee `capabilities.experimental.vestaMethods`.
  2. La lista no puede estar vacia (si lo esta, la tabla no se poblo y todo lo
     demas pasaria en falso).
  3. Se INVOCA cada metodo anunciado.  Uno anunciado y no despachado no
     responde: el servidor lo deja pasar como desconocido y la peticion se
     queda sin contestar.  Esperar la respuesta de cada id es lo que convierte
     "no esta cableado" en un fallo visible.

No se mira el CONTENIDO de cada respuesta -- de eso ya se encarga
smoke_inspector.py --; aqui solo importa que cada consulta anunciada exista de
verdad al otro lado.

Uso:
    python tests/lsp/smoke_query_table.py <ruta-al-vesta_lsp[.exe]>

Devuelve 0 si todo pasa, !=0 en fallo.  Solo usa la stdlib de Python.
"""
import json
import os
import subprocess
import sys

SRC = (
    "i64 suma(i64 a, i64 b) { return a + b; }\n"
    "i64 main() { return suma(2, 3); }\n"
)
URI = "file:///query_table_demo.vx"

# Parametros que algunas consultas exigen ademas del uri.  Se mandan SIEMPRE:
# sobran para las que no los piden, y una consulta que los rechazara por
# sobrantes seria en si misma el fallo que esto busca.
EXTRA = {"line": 1, "character": 1, "arch": "", "os": "", "cpu": ""}


def frame(obj):
    """Enmarca un objeto JSON como mensaje LSP (Content-Length + CRLF)."""
    body = json.dumps(obj).encode("utf-8")
    return b"Content-Length: %d\r\n\r\n%s" % (len(body), body)


def read_one(stream):
    """Lee UN mensaje enmarcado, o None si el flujo se acabo.

    Se lee por tramas y se manda UNA peticion cada vez, no todas de golpe.  No
    es mania: escribir las veintitantas seguidas sin leer LLENA el buffer de la
    tuberia -- una sola respuesta pasa de los 8 KB --, y entonces el servidor se
    bloquea escribiendo mientras el cliente se bloquea escribiendo.  Con dos o
    tres consultas no salta porque la salida cabe; con la tabla entera, cuelga.
    """
    header = b""
    while not header.endswith(b"\r\n\r\n"):
        ch = stream.read(1)
        if not ch:
            return None
        header += ch
    size = 0
    for line in header.split(b"\r\n"):
        if line.lower().startswith(b"content-length:"):
            size = int(line.split(b":", 1)[1])
    body = b""
    while len(body) < size:
        chunk = stream.read(size - len(body))
        if not chunk:
            return None
        body += chunk
    try:
        return json.loads(body.decode("utf-8"))
    except (ValueError, UnicodeDecodeError):
        return {}


class Server:
    """Un vesta_lsp vivo al que se le pregunta de una en una."""

    def __init__(self, exe):
        self.proc = subprocess.Popen([exe], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE)

    def send(self, *messages):
        self.proc.stdin.write(b"".join(frame(m) for m in messages))
        self.proc.stdin.flush()

    def ask(self, req_id, method, params):
        """Manda UNA peticion y devuelve su respuesta, o None si no llega.

        Las notificaciones que lleguen por medio (diagnosticos) se descartan:
        lo que se espera es la respuesta de @p req_id.
        """
        self.send({"jsonrpc": "2.0", "id": req_id, "method": method,
                   "params": params})
        while True:
            msg = read_one(self.proc.stdout)
            if msg is None:
                return None
            if msg.get("id") == req_id:
                return msg

    def close(self):
        try:
            self.send({"jsonrpc": "2.0", "method": "exit"})
            self.proc.stdin.close()
        except OSError:
            pass
        try:
            self.proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def main():
    if len(sys.argv) < 2:
        print("uso: smoke_query_table.py <vesta_lsp>", file=sys.stderr)
        return 2
    exe = sys.argv[1]
    if not os.path.isfile(exe):
        print("no existe el binario: " + exe, file=sys.stderr)
        return 2

    server = Server(exe)
    try:
        # --- 1) que anuncia ---
        init = server.ask(1, "initialize", {"processId": None,
                                            "rootUri": None,
                                            "capabilities": {}})
        if init is None:
            print("FALLO: initialize no respondio", file=sys.stderr)
            return 1
        server.send({"jsonrpc": "2.0", "method": "initialized", "params": {}})

        caps = init.get("result", {}).get("capabilities", {})
        methods = caps.get("experimental", {}).get("vestaMethods", [])
        if not methods:
            print("FALLO: no se anuncia ninguna consulta -- la tabla esta "
                  "vacia, asi que el resto pasaria en falso", file=sys.stderr)
            return 1
        print("anunciadas: %d consultas" % len(methods))

        # --- 2) todas las anunciadas responden ---
        server.send({"jsonrpc": "2.0", "method": "textDocument/didOpen",
                     "params": {"textDocument": {"uri": URI,
                                                 "languageId": "vx",
                                                 "version": 1, "text": SRC}}})
        silent = []
        for i, name in enumerate(methods):
            params = {"uri": URI}
            params.update(EXTRA)
            if server.ask(1000 + i, name, params) is None:
                silent.append(name)
        if silent:
            print("FALLO: anunciadas pero SIN despachar: " + ", ".join(silent),
                  file=sys.stderr)
            return 1
    finally:
        server.close()

    print("OK: las %d consultas anunciadas responden" % len(methods))
    return 0


if __name__ == "__main__":
    sys.exit(main())
