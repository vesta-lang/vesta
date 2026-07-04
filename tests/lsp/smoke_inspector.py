#!/usr/bin/env python3
"""VestaVM - smoke test del inspector del LSP de Vex (portable Linux/Windows).

Reemplaza al antiguo smoke_inspector.sh (bash, no portable a Windows).  Envia
por stdin una secuencia JSON-RPC (initialize + initialized + didOpen de un .vx
con una funcion aritmetica JIT-compilable, un loop para la complejidad y una
clase) al binario vesta_lsp e invoca CADA peticion a medida vesta/*, verificando
que cada una responde de forma no vacia o estructurada:

  - vesta/bytecode    -> tiene texto .vel.
  - vesta/ir post/pre -> tiene texto IR.
  - vesta/complexity  -> JSON con >=1 funcion.
  - vesta/diagram     -> mermaid de ir-post con texto.
  - vesta/functions   -> lista con >=1 funcion (incluye 'suma').
  - vesta/aotCompat   -> JSON con compatible/issues.
  - vesta/jitAsm      -> disasm (text) o unsupported con razon.
  - vesta/aotAsm      -> disasm (text) o incompatible con razon.

Ademas valida las VISTAS POR OS/ARQUITECTURA (params.os/params.arch): el asm
nativo generado para Windows x86-64 vs Linux x86-32 debe responder sin error.

Uso:
    python tests/lsp/smoke_inspector.py <ruta-al-vesta_lsp[.exe]>

Devuelve 0 si todo pasa, !=0 en fallo.  Solo usa la stdlib de Python.
"""
import json
import os
import subprocess
import sys

# Fuente Vex: (a) suma aritmetica pura JIT-compilable, (b) loop para la
# complejidad lineal, (c) una clase con un metodo.
SRC = (
    "i64 suma(i64 a, i64 b) { return a + b; }\n"
    "i64 acumular(i64 n) { i64 s = 0; i64 i = 0; while (i < n) "
    "{ s = s + i; i = i + 1; } return s; }\n"
    "class Punto { public i64 x; public i64 getX() { return this.x; } }\n"
    "i64 main() { return suma(2, 3); }\n"
)
URI = "file:///inspector_demo.vx"


def frame(obj):
    """Enmarca un objeto JSON como mensaje LSP (Content-Length + CRLF)."""
    body = json.dumps(obj).encode("utf-8")
    return b"Content-Length: %d\r\n\r\n%s" % (len(body), body)


def build_input():
    msgs = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize",
         "params": {"processId": None, "rootUri": None, "capabilities": {}}},
        {"jsonrpc": "2.0", "method": "initialized", "params": {}},
        {"jsonrpc": "2.0", "method": "textDocument/didOpen",
         "params": {"textDocument": {"uri": URI, "languageId": "vx",
                                     "version": 1, "text": SRC}}},
        {"jsonrpc": "2.0", "id": 10, "method": "vesta/bytecode",
         "params": {"uri": URI}},
        {"jsonrpc": "2.0", "id": 11, "method": "vesta/ir",
         "params": {"uri": URI, "phase": "post"}},
        {"jsonrpc": "2.0", "id": 12, "method": "vesta/ir",
         "params": {"uri": URI, "phase": "pre"}},
        {"jsonrpc": "2.0", "id": 13, "method": "vesta/complexity",
         "params": {"uri": URI}},
        {"jsonrpc": "2.0", "id": 14, "method": "vesta/diagram",
         "params": {"uri": URI, "kind": "ir-post", "format": "mermaid"}},
        {"jsonrpc": "2.0", "id": 15, "method": "vesta/functions",
         "params": {"uri": URI}},
        {"jsonrpc": "2.0", "id": 16, "method": "vesta/aotCompat",
         "params": {"uri": URI, "tier": "bare"}},
        {"jsonrpc": "2.0", "id": 17, "method": "vesta/jitAsm",
         "params": {"uri": URI, "function": "suma"}},
        {"jsonrpc": "2.0", "id": 18, "method": "vesta/aotAsm",
         "params": {"uri": URI, "function": "suma"}},
        # Vistas por OS/arch: el mismo asm nativo para Windows x86-64 y Linux
        # x86-32 debe responder sin error (target_sysv/mode32 distintos).
        {"jsonrpc": "2.0", "id": 20, "method": "vesta/aotAsm",
         "params": {"uri": URI, "function": "suma",
                    "os": "windows", "arch": "x86-64"}},
        {"jsonrpc": "2.0", "id": 21, "method": "vesta/aotAsm",
         "params": {"uri": URI, "function": "suma",
                    "os": "linux", "arch": "x86-32"}},
        {"jsonrpc": "2.0", "id": 99, "method": "shutdown", "params": None},
        {"jsonrpc": "2.0", "method": "exit", "params": None},
    ]
    return b"".join(frame(m) for m in msgs)


def parse_responses(data):
    """Extrae los objetos JSON de un stream LSP (Content-Length)."""
    out = {}
    i, n = 0, len(data)
    while i < n:
        j = data.find(b"Content-Length:", i)
        if j < 0:
            break
        k = data.find(b"\r\n\r\n", j)
        if k < 0:
            break
        length = int(data[j + 15:k].strip())
        body = data[k + 4:k + 4 + length]
        i = k + 4 + length
        try:
            obj = json.loads(body.decode("utf-8", errors="replace"))
        except Exception:
            continue
        if isinstance(obj, dict) and obj.get("id") is not None:
            out[obj["id"]] = obj
    return out, data


def main():
    if len(sys.argv) < 2 or not os.path.exists(sys.argv[1]):
        sys.stderr.write("uso: python smoke_inspector.py <vesta_lsp[.exe]>\n")
        return 2
    lsp = sys.argv[1]
    try:
        p = subprocess.run([lsp], input=build_input(), capture_output=True,
                           timeout=120)
    except subprocess.TimeoutExpired:
        sys.stderr.write("FALLO: el servidor LSP no termino (timeout)\n")
        return 1
    resp, raw = parse_responses(p.stdout)

    fails = 0

    def result_of(rid):
        r = resp.get(rid)
        return r.get("result") if r else None

    def check(rid, pred, desc):
        nonlocal fails
        res = result_of(rid)
        ok = False
        try:
            ok = pred(res)
        except Exception:
            ok = False
        print(("OK  " if ok else "FALLO  ") + desc)
        if not ok:
            fails += 1

    def has_text(res):
        return isinstance(res, dict) and isinstance(res.get("text"), str) \
            and res["text"] != ""

    # 0) initialize anuncia los metodos experimentales vesta/*.
    print(("OK  " if b"vestaMethods" in raw else "FALLO  ") +
          "initialize anuncia vestaMethods (experimental)")
    if b"vestaMethods" not in raw:
        fails += 1

    check(10, has_text, "vesta/bytecode devuelve text")
    check(11, has_text, "vesta/ir (post) devuelve text")
    check(12, has_text, "vesta/ir (pre) devuelve text")
    check(13, lambda r: isinstance(r, dict) and "functions" in r,
          "vesta/complexity devuelve functions")
    check(14, has_text, "vesta/diagram (mermaid ir-post) devuelve text")
    check(15, lambda r: isinstance(r, dict) and any(
        f.get("name") == "suma" for f in r.get("functions", [])),
          "vesta/functions incluye la funcion suma")
    check(16, lambda r: isinstance(r, dict) and "compatible" in r
          and "issues" in r, "vesta/aotCompat devuelve compatible+issues")
    check(17, lambda r: isinstance(r, dict) and (
        has_text(r) or r.get("unsupported")),
          "vesta/jitAsm devuelve disasm o unsupported")
    check(18, lambda r: isinstance(r, dict) and (
        has_text(r) or r.get("incompatible")),
          "vesta/aotAsm devuelve disasm o incompatible")
    # Vistas por OS/arch: sin campo 'error'.
    check(20, lambda r: isinstance(r, dict) and not r.get("error"),
          "vesta/aotAsm windows/x86-64 sin error")
    check(21, lambda r: isinstance(r, dict) and not r.get("error"),
          "vesta/aotAsm linux/x86-32 sin error")

    if fails == 0:
        print("smoke_inspector: TODO OK")
        return 0
    print("smoke_inspector: HUBO FALLOS")
    return 1


if __name__ == "__main__":
    sys.exit(main())
