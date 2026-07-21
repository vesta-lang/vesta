# SPDX-License-Identifier: MIT
"""Cliente JSON-RPC de alto nivel para el LSP de Vesta (``vesta_lsp``).

El servidor ``vesta_lsp`` habla LSP (JSON-RPC 2.0 con framing
``Content-Length``) por stdin/stdout y, ademas de los metodos estandar
(hover, completado, definicion, referencias, semantic tokens), expone
metodos propios bajo el prefijo ``vesta/*`` (IR, bytecode, asm JIT/AOT,
diagramas, complejidad, reporte de modos, etc.).

Esta clase encapsula el transporte (arranque del proceso, framing, correlacion
de respuestas por ``id``, captura de notificaciones ``publishDiagnostics``) y
ofrece un metodo Python por cada peticion, devolviendo directamente el JSON ya
parseado.

Ejemplo::

    from vesta_lsp_client import VestaLspClient

    with VestaLspClient("build/vesta_lsp.exe") as lsp:
        uri = lsp.open("ejemplo.vx", text="i64 main() { return 7; }")
        for d in lsp.diagnostics(uri):
            print(d["range"], d["message"])
        print(lsp.functions(uri))
        print(lsp.diagram(uri, kind="types", fmt="mermaid")["text"])
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from typing import Any, Dict, List, Optional


def _exe_names() -> List[str]:
    """Nombres del binario del LSP segun el SO (con y sin extension)."""
    return ["vesta_lsp.exe", "vesta_lsp"] if os.name == "nt" \
        else ["vesta_lsp"]


def _install_roots() -> List[str]:
    """Raices de instalacion comunes de VestaVM segun el SO.

    Sigue el modelo de instalacion del proyecto: per-usuario y system-wide,
    con override por las variables de entorno ``VESTA_HOME`` / ``VEX_HOME``.
    Bajo cada raiz se prueban las subrutas ``lsp/bin``, ``bin`` y la propia
    raiz (ver :func:`discover_lsp`).
    """
    roots: List[str] = []
    for env in ("VESTA_LSP_HOME", "VESTA_HOME", "VEX_HOME"):
        v = os.environ.get(env)
        if v:
            roots.append(v)
    if os.name == "nt":
        for env in ("ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA",
                    "APPDATA", "ProgramData"):
            base = os.environ.get(env)
            if base:
                roots.append(os.path.join(base, "VestaVM"))
    else:
        home = os.path.expanduser("~")
        roots += [
            "/usr/local/vesta", "/opt/vesta", "/usr/local", "/usr",
            os.path.join(home, ".local", "share", "vesta"),
            os.path.join(home, ".local"),
            os.path.join(home, "vesta"),
        ]
    return roots


def _repo_build_dirs() -> List[str]:
    """Directorios de build tipicos, subiendo desde este paquete.

    Permite usar el cliente directamente desde un checkout del repo sin
    instalar (los binarios estan en ``cmake-build-*/`` o ``build/``).
    """
    out: List[str] = []
    here = os.path.dirname(os.path.abspath(__file__))
    d = here
    for _ in range(8):  # subir hasta 8 niveles buscando la raiz del repo.
        for sub in ("cmake-build-release", "cmake-build-debug", "build",
                    "cmake-build-windows", "out"):
            out.append(os.path.join(d, sub))
        parent = os.path.dirname(d)
        if parent == d:
            break
        d = parent
    return out


def discover_lsp(explicit: Optional[str] = None) -> Optional[str]:
    """Localiza el ejecutable ``vesta_lsp`` de forma portable.

    Orden de busqueda (el primero que exista gana):

    1. El argumento ``explicit`` si se da y existe.
    2. La variable de entorno ``VESTA_LSP`` (ruta directa al binario).
    3. El ``PATH`` del sistema (``shutil.which``).
    4. Raices de instalacion de VestaVM (``lsp/bin``, ``bin`` y la raiz),
       incluyendo ``VESTA_HOME`` / ``VEX_HOME`` y las ubicaciones estandar
       por SO (``%ProgramFiles%/VestaVM``, ``/usr/local/vesta``, ...).
    5. Directorios de build del repo (``cmake-build-*/``, ``build/``) subiendo
       desde este paquete.

    Funciona igual en Windows, Linux y macOS.

    :param explicit: ruta candidata prioritaria (p.ej. la de un argumento CLI).
    :returns: ruta absoluta al binario, o ``None`` si no se encuentra.
    """
    return find_binary(_exe_names(), env_var="VESTA_LSP",
                       subdirs=[os.path.join("lsp", "bin"), "bin", ""],
                       explicit=explicit)


def find_binary(names: List[str], *, env_var: Optional[str] = None,
                subdirs: Optional[List[str]] = None,
                explicit: Optional[str] = None) -> Optional[str]:
    """Localiza un ejecutable de VestaVM de forma portable (motor comun).

    Orden: ``explicit`` -> variable de entorno ``env_var`` -> ``PATH`` -> raices
    de instalacion (``VESTA_HOME``/``VEX_HOME`` + ubicaciones estandar por SO) ->
    builds del repo (``cmake-build-*``, ``build``).  Es el motor detras de
    :func:`discover_lsp` y :func:`vesta_lsp_client.runtime.discover_vm`.

    :param names: nombres candidatos del binario (con y sin extension).
    :param env_var: variable de entorno con la ruta directa al binario.
    :param subdirs: subdirectorios a probar bajo cada raiz de instalacion.
    :param explicit: ruta candidata prioritaria.
    :returns: ruta absoluta o ``None``.
    """
    if subdirs is None:
        subdirs = ["bin", ""]
    if explicit and os.path.isfile(explicit):
        return os.path.abspath(explicit)
    if env_var:
        env_bin = os.environ.get(env_var)
        if env_bin and os.path.isfile(env_bin):
            return os.path.abspath(env_bin)
    for name in names:
        found = shutil.which(name)
        if found:
            return os.path.abspath(found)
    for root in _install_roots() + _repo_build_dirs():
        for sub in subdirs:
            for name in names:
                cand = os.path.join(root, sub, name)
                if os.path.isfile(cand):
                    return os.path.abspath(cand)
    return None


class LspError(RuntimeError):
    """Error devuelto por el servidor LSP o del propio transporte.

    Se lanza cuando una respuesta JSON-RPC trae un objeto ``error`` o cuando
    el servidor cierra la conexion de forma inesperada.
    """


class VestaLspClient:
    """Cliente sencillo del servidor ``vesta_lsp``.

    Arranca el binario como subproceso y realiza el handshake ``initialize`` /
    ``initialized`` en el constructor.  Usalo preferentemente como gestor de
    contexto para garantizar el cierre limpio (``shutdown`` + ``exit``)::

        with VestaLspClient("vesta_lsp.exe") as lsp:
            ...

    :param lsp_path: Ruta al ejecutable ``vesta_lsp`` (``.exe`` en Windows).
        Si es ``None`` (por defecto) se auto-detecta con :func:`discover_lsp`
        (PATH + rutas de instalacion comunes + builds del repo).
    :param root_uri: ``rootUri`` opcional del workspace (para resolver
        ``import "..."`` de proyectos multi-fichero).  Puede ser una ruta de
        disco o un URI ``file://``.
    :param timeout: Segundos maximos de espera al leer una respuesta.  ``None``
        = sin limite.
    :raises LspError: si no se encuentra el binario o el handshake falla.
    """

    def __init__(
        self,
        lsp_path: Optional[str] = None,
        *,
        root_uri: Optional[str] = None,
        timeout: Optional[float] = 30.0,
    ) -> None:
        resolved = discover_lsp(lsp_path)
        if not resolved:
            raise LspError(
                "no se encontro el binario vesta_lsp. Pasa la ruta explicita, "
                "define la variable de entorno VESTA_LSP, o anyadelo al PATH.")
        # CreateProcess (Windows) no resuelve rutas relativas con '/': usar
        # siempre ruta absoluta con separadores nativos.
        self._bin = resolved
        self._timeout = timeout
        self._next = 1
        self._closed = False
        # uri -> lista de diagnosticos de la ultima publishDiagnostics.
        self._diagnostics: Dict[str, List[Dict[str, Any]]] = {}

        try:
            self._proc = subprocess.Popen(
                [self._bin],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                bufsize=0,
            )
        except OSError as exc:
            # Caso tipico: se paso una ruta que no es el ejecutable del LSP
            # (p.ej. un fichero .vx).  WinError 193 = "no es una aplicacion
            # Win32 valida"; ENOEXEC en POSIX.
            raise LspError(
                "no se pudo arrancar el LSP en %r: %s. "
                "Comprueba que la ruta apunta al ejecutable vesta_lsp "
                "(no a un fichero fuente)." % (self._bin, exc)) from exc
        root = self.to_uri(root_uri) if root_uri else None
        init = self._request(
            "initialize",
            {"processId": None, "rootUri": root, "capabilities": {}},
        )
        # Capacidades del servidor (incluye la leyenda de semantic tokens y la
        # lista de metodos vesta/* bajo experimental.vestaMethods).
        self.server_capabilities: Dict[str, Any] = \
            (init or {}).get("capabilities", {})
        self._notify("initialized", {})

    @property
    def binary(self) -> str:
        """Ruta absoluta del binario ``vesta_lsp`` en uso."""
        return self._bin

    def semantic_token_legend(self) -> List[str]:
        """Nombres de los tipos de semantic token, indexados por su codigo.

        Es la ``legend.tokenTypes`` que el servidor anuncia en ``initialize``.
        El ``tokenType`` que devuelve :func:`decode_semantic_tokens` es un
        indice en esta lista.  Vacia si el servidor no la anuncia.
        """
        prov = self.server_capabilities.get("semanticTokensProvider", {})
        return list(prov.get("legend", {}).get("tokenTypes", []))

    # ------------------------------------------------------------------ #
    #  Gestor de contexto                                                 #
    # ------------------------------------------------------------------ #
    def __enter__(self) -> "VestaLspClient":
        return self

    def __exit__(self, *_exc: Any) -> None:
        self.close()

    def close(self) -> None:
        """Cierra el servidor (``shutdown`` + ``exit``) y espera al proceso.

        Idempotente: llamarlo varias veces no tiene efecto tras el primero.
        """
        if self._closed:
            return
        self._closed = True
        try:
            self._request("shutdown", None)
            self._notify("exit", {})
        except Exception:
            pass
        try:
            self._proc.stdin.close()
        except Exception:
            pass
        try:
            self._proc.wait(timeout=5)
        except Exception:
            self._proc.kill()

    # ------------------------------------------------------------------ #
    #  Utilidades                                                         #
    # ------------------------------------------------------------------ #
    @staticmethod
    def to_uri(path: str) -> str:
        """Convierte una ruta de disco en un URI ``file://`` portable.

        Si ``path`` ya parece un URI (``file://...``) se devuelve tal cual.
        En Windows produce ``file:///C:/...``.
        """
        if path.startswith("file://"):
            return path
        p = os.path.abspath(path).replace("\\", "/")
        if len(p) >= 2 and p[1] == ":":  # unidad Windows (C:/...)
            return "file:///" + p
        return "file://" + p

    # ------------------------------------------------------------------ #
    #  Transporte JSON-RPC                                                #
    # ------------------------------------------------------------------ #
    def _frame(self, obj: Dict[str, Any]) -> bytes:
        body = json.dumps(obj).encode("utf-8")
        return b"Content-Length: %d\r\n\r\n%s" % (len(body), body)

    def _write(self, obj: Dict[str, Any]) -> None:
        if self._proc.poll() is not None:
            raise LspError("el servidor LSP ya termino")
        self._proc.stdin.write(self._frame(obj))
        self._proc.stdin.flush()

    def _notify(self, method: str, params: Any) -> None:
        """Envia una notificacion (sin ``id``, sin respuesta)."""
        self._write({"jsonrpc": "2.0", "method": method, "params": params})

    def _read_message(self) -> Dict[str, Any]:
        """Lee un mensaje JSON-RPC enmarcado del stdout del servidor."""
        out = self._proc.stdout
        length = None
        # 1) Cabeceras hasta la linea en blanco.
        while True:
            line = out.readline()
            if not line:
                raise LspError("el servidor LSP cerro el stdout")
            s = line.decode("utf-8", "replace").strip()
            if s == "":
                break
            low = s.lower()
            if low.startswith("content-length:"):
                length = int(s.split(":", 1)[1].strip())
        if length is None:
            raise LspError("respuesta sin Content-Length")
        # 2) Cuerpo exacto.
        body = b""
        while len(body) < length:
            chunk = out.read(length - len(body))
            if not chunk:
                raise LspError("stdout truncado leyendo el cuerpo")
            body += chunk
        return json.loads(body.decode("utf-8", "replace"))

    def _handle_notification(self, msg: Dict[str, Any]) -> None:
        """Procesa notificaciones server->cliente (p.ej. publishDiagnostics)."""
        if msg.get("method") == "textDocument/publishDiagnostics":
            params = msg.get("params", {})
            uri = params.get("uri", "")
            self._diagnostics[uri] = params.get("diagnostics", [])

    def _request(self, method: str, params: Any) -> Any:
        """Envia una peticion y devuelve su ``result`` (lanza en ``error``).

        Las notificaciones que lleguen mientras se espera la respuesta (como
        ``publishDiagnostics``) se capturan por el camino.
        """
        rid = self._next
        self._next += 1
        self._write({"jsonrpc": "2.0", "id": rid, "method": method,
                     "params": params})
        # Leer mensajes hasta encontrar la respuesta con nuestro id.
        while True:
            msg = self._read_message()
            if "id" in msg and msg.get("id") == rid:
                if "error" in msg and msg["error"] is not None:
                    raise LspError("%s: %s" % (method, msg["error"]))
                return msg.get("result")
            if "method" in msg and "id" not in msg:
                self._handle_notification(msg)
            # Respuestas a otros id (no deberia pasar en uso sincrono): ignorar.

    def request(self, method: str, params: Any) -> Any:
        """Peticion JSON-RPC generica (escotilla de escape).

        Util para metodos no envueltos aun por un helper.  Devuelve el
        ``result`` ya parseado.
        """
        return self._request(method, params)

    # ------------------------------------------------------------------ #
    #  Documentos                                                         #
    # ------------------------------------------------------------------ #
    def open(
        self,
        path_or_uri: str,
        *,
        text: Optional[str] = None,
        language_id: str = "vx",
        version: int = 1,
    ) -> str:
        """Abre un documento (``textDocument/didOpen``) y captura sus diagnosticos.

        :param path_or_uri: Ruta de disco o URI ``file://``.
        :param text: Contenido del documento.  Si es ``None`` y ``path_or_uri``
            es una ruta existente, se lee del disco.
        :param language_id: ``languageId`` LSP (por defecto ``"vx"``).
        :param version: Version inicial del documento.
        :returns: El URI del documento (usalo en el resto de metodos).

        Tras abrir, ``diagnostics(uri)`` devuelve los diagnosticos publicados.
        """
        uri = self.to_uri(path_or_uri)
        if text is None:
            local = path_or_uri
            if local.startswith("file:///"):
                local = local[len("file:///"):]
            elif local.startswith("file://"):
                local = local[len("file://"):]
            with open(local, "r", encoding="utf-8") as fh:
                text = fh.read()
        self._notify(
            "textDocument/didOpen",
            {"textDocument": {"uri": uri, "languageId": language_id,
                              "version": version, "text": text}},
        )
        self._drain_diagnostics(uri)
        return uri

    def change(self, uri: str, text: str, *, version: int = 2) -> None:
        """Reemplaza el contenido de un documento abierto (``didChange``)."""
        self._notify(
            "textDocument/didChange",
            {"textDocument": {"uri": uri, "version": version},
             "contentChanges": [{"text": text}]},
        )
        self._drain_diagnostics(uri)

    def close_document(self, uri: str) -> None:
        """Cierra un documento en el servidor (``textDocument/didClose``)."""
        self._notify("textDocument/didClose",
                     {"textDocument": {"uri": uri}})
        self._diagnostics.pop(uri, None)

    def _drain_diagnostics(self, uri: str, max_msgs: int = 16) -> None:
        """Lee la notificacion ``publishDiagnostics`` que sigue a un did*.

        El servidor la emite de forma sincrona tras procesar el did*, asi que
        el proximo mensaje del stdout es esa notificacion.  Se leen hasta
        ``max_msgs`` por robustez.
        """
        for _ in range(max_msgs):
            msg = self._read_message()
            if "method" in msg and "id" not in msg:
                self._handle_notification(msg)
                if (msg.get("method") == "textDocument/publishDiagnostics"
                        and msg.get("params", {}).get("uri") == uri):
                    return

    def diagnostics(self, uri: str) -> List[Dict[str, Any]]:
        """Diagnosticos publicados para ``uri`` (lista, posiblemente vacia)."""
        return self._diagnostics.get(uri, [])

    # ------------------------------------------------------------------ #
    #  Peticiones estandar LSP                                            #
    # ------------------------------------------------------------------ #
    def _pos(self, method: str, uri: str, line: int, character: int,
             extra: Optional[Dict[str, Any]] = None) -> Any:
        params = {"textDocument": {"uri": uri},
                  "position": {"line": line, "character": character}}
        if extra:
            params.update(extra)
        return self._request(method, params)

    def hover(self, uri: str, line: int, character: int) -> Any:
        """Hover LSP en una posicion (0-based).  Devuelve el ``result`` (o None)."""
        return self._pos("textDocument/hover", uri, line, character)

    def completion(self, uri: str, line: int, character: int) -> Any:
        """Completado en una posicion.  Devuelve la lista/objeto de items."""
        return self._pos("textDocument/completion", uri, line, character)

    def definition(self, uri: str, line: int, character: int) -> Any:
        """Ir a definicion.  Devuelve Location(s) o None."""
        return self._pos("textDocument/definition", uri, line, character)

    def references(self, uri: str, line: int, character: int,
                   include_declaration: bool = True) -> Any:
        """Referencias del simbolo bajo el cursor.  Devuelve una lista de Location."""
        return self._pos(
            "textDocument/references", uri, line, character,
            {"context": {"includeDeclaration": include_declaration}})

    def semantic_tokens(self, uri: str) -> Any:
        """Semantic tokens del documento completo (``{"data": [...]}``).

        Usa :func:`decode_semantic_tokens` para pasar el array plano de deltas
        a posiciones absolutas ``(linea, columna, tipo)``.
        """
        return self._request("textDocument/semanticTokens/full",
                             {"textDocument": {"uri": uri}})

    # ------------------------------------------------------------------ #
    #  Peticiones propias de Vesta (vesta/*)                              #
    # ------------------------------------------------------------------ #
    def symbol_info(self, uri: str, line: int, character: int) -> Any:
        """Info del simbolo bajo el cursor: nombre, categoria, firma y doc.

        Es la fuente del hover rico; funciona tambien sobre builtins y
        conceptos (que no aparecen en el indice del documento).
        """
        return self._request(
            "vesta/symbolInfo",
            {"textDocument": {"uri": uri}, "uri": uri,
             "line": line, "character": character,
             "position": {"line": line, "character": character}})

    def param_hints(self, uri: str) -> Any:
        """Inlay hints con el nombre de cada parametro en las llamadas."""
        return self._request("vesta/paramHints",
                             {"textDocument": {"uri": uri}, "uri": uri})

    def ir(self, uri: str, phase: str = "post",
           os_: Optional[str] = None, arch: Optional[str] = None) -> Any:
        """Texto del SSA IR del modulo.

        :param phase: ``"post"`` (optimizado) o ``"pre"`` (crudo del lowering).
        :param os_/arch: target opcional (``linux``/``windows``, ``x86-64``/
            ``x86-32``) para seleccionar las ramas ``@Target``.
        """
        return self._request(
            "vesta/ir",
            self._with_target({"textDocument": {"uri": uri}, "uri": uri,
                               "phase": phase}, os_, arch))

    def ir_diff(self, uri: str) -> Any:
        """Diff entre el IR pre y post optimizacion (que hizo el optimizador)."""
        return self._request("vesta/irDiff",
                             {"textDocument": {"uri": uri}, "uri": uri})

    def bytecode(self, uri: str,
                 os_: Optional[str] = None, arch: Optional[str] = None) -> Any:
        """Texto del bytecode ``.vel`` final del modulo."""
        return self._request(
            "vesta/bytecode",
            self._with_target({"textDocument": {"uri": uri}, "uri": uri},
                              os_, arch))

    def functions(self, uri: str) -> Any:
        """Lista de funciones del modulo (nombre, firma, coste, ...)."""
        return self._request("vesta/functions",
                             {"textDocument": {"uri": uri}, "uri": uri})

    def complexity(self, uri: str) -> Any:
        """Complejidad Big-O por funcion (parcial + total interprocedural)."""
        return self._request("vesta/complexity",
                             {"textDocument": {"uri": uri}, "uri": uri})

    def diagram(self, uri: str, kind: str = "ir-post", fmt: str = "mermaid",
                *, cost: bool = False, function: str = "",
                os_: Optional[str] = None, arch: Optional[str] = None) -> Any:
        """Diagrama del modulo en un formato.

        :param kind: ``"ast"`` | ``"ir-pre"`` | ``"ir-post"`` | ``"vel"`` |
            ``"types"`` (classDiagram de la POO) | ``"asm"`` (CFG del codigo
            nativo de una funcion).
        :param fmt: ``"mermaid"`` | ``"graphviz"`` | ``"html"``.
        :param cost: anota los nodos-funcion con su coste Big-O (kinds IR).
        :param function: para ``kind="asm"``, la funcion a diagramar (vacio =
            la primera compilable / ``main``).
        :param os_/arch: target opcional.
        :returns: ``{"text": "<diagrama>"}`` o ``{"error": ...}``.
        """
        params = {"textDocument": {"uri": uri}, "uri": uri,
                  "kind": kind, "format": fmt, "cost": cost}
        if function:
            params["function"] = function
        return self._request("vesta/diagram",
                             self._with_target(params, os_, arch))

    def jit_asm(self, uri: str, function: str = "",
                os_: Optional[str] = None, arch: Optional[str] = None) -> Any:
        """Desensamblado x86-64 del codigo JIT de una funcion (o la primera)."""
        return self._request(
            "vesta/jitAsm",
            self._with_target({"textDocument": {"uri": uri}, "uri": uri,
                               "function": function}, os_, arch))

    def aot_asm(self, uri: str, function: str = "",
                os_: Optional[str] = None, arch: Optional[str] = None) -> Any:
        """Desensamblado del codigo AOT nativo de una funcion (o la primera)."""
        return self._request(
            "vesta/aotAsm",
            self._with_target({"textDocument": {"uri": uri}, "uri": uri,
                               "function": function}, os_, arch))

    def aot_compat(self, uri: str, tier: str = "bare") -> Any:
        """Compatibilidad AOT del modulo a un tier (``bare``|``embed``|``full``).

        Devuelve ``{compatible, issues, ok_functions, tier}``.
        """
        return self._request("vesta/aotCompat",
                             {"textDocument": {"uri": uri}, "uri": uri,
                              "tier": tier})

    def modes(self, uri: str, mode: str = "", tier: str = "bare") -> Any:
        """Reporte del modulo en los tres modos de ejecucion.

        El LSP no asume el modo: devuelve una entrada por cada uno de
        ``interp`` / ``jit`` / ``aot``.  Si se pasa ``mode`` (uno de esos tres)
        devuelve solo ese.  ``tier`` solo afecta al modo AOT.

        :returns: ``{"modes": [ {"mode": ..., ...}, ... ]}``.
        """
        params = {"textDocument": {"uri": uri}, "uri": uri, "tier": tier}
        if mode:
            params["mode"] = mode
        return self._request("vesta/modes", params)

    def macro_expand(self, uri: str) -> Any:
        """Codigo que generan los ``@Macro`` del modulo (expectaciones)."""
        return self._request("vesta/macroExpand",
                             {"textDocument": {"uri": uri}, "uri": uri})

    def comptime_values(self, uri: str) -> Any:
        """Valores ``comptime`` (consts + builtins ``sizeof``/``kind``/...)."""
        return self._request("vesta/comptimeValues",
                             {"textDocument": {"uri": uri}, "uri": uri})

    # ------------------------------------------------------------------ #
    #  Compilacion (el LSP embebe el compilador)                          #
    # ------------------------------------------------------------------ #
    def compile(self, uri: str, *, output: Optional[str] = None,
                mode: str = "vm", project: Optional[bool] = None,
                debug: bool = False, instrument: Optional[str] = None,
                keep_labels: bool = False, emit_map: bool = False) -> Any:
        """Compila el documento a ``.velb`` con el compilador embebido del LSP.

        No ejecuta nada (la ejecucion corre en un proceso aparte; ver
        :class:`vesta_lsp_client.VestaRunner`).  Si el documento esta abierto se
        usa su buffer; si no, se lee del disco.

        :param uri: URI del fichero a compilar.
        :param output: prefijo de salida (produce ``<output>.velb``); ``None``
            = derivar del nombre del fichero.
        :param mode: ``"vm"`` | ``"jit"`` (producen ``.velb``) | ``"aot"``
            (artefacto nativo ``.exe``/``.o``/``.so``/``.bin`` PE/ELF, tambien
            con el compilador embebido).  Para AOT hay parametros extra opcionales
            (``format``, ``emit``, ``arch``, ``tier``, ...) que se pueden pasar
            via :meth:`request` a ``vesta/compile``.
        :param project: forzar compilacion de proyecto (resuelve ``import``);
            ``None`` = auto-detectar si el fuente tiene ``import "..."``.
        :param debug: emitir info de depuracion (mapeo linea<->bytecode).
        :param instrument: modo de instrumentacion (``None`` = ninguno).
        :param keep_labels: conservar nombres de label en el ``.velb``.
        :param emit_map: emitir un ``.velb-map`` con info de simbolos.
        :returns: ``{ok, output, diagnostics, frontend_us, mode, project,
            message?}``.
        """
        params: Dict[str, Any] = {
            "textDocument": {"uri": uri}, "uri": uri, "mode": mode,
            "debug": debug, "keepLabels": keep_labels, "emitMap": emit_map,
        }
        if output is not None:
            params["output"] = output
        if project is not None:
            params["project"] = project
        if instrument is not None:
            params["instrument"] = instrument
        method = "vesta/compileProject" if project else "vesta/compile"
        return self._request(method, params)

    def compile_project(self, uri: str, *, output: Optional[str] = None,
                        mode: str = "vm", debug: bool = False,
                        instrument: Optional[str] = None) -> Any:
        """Compila un PROYECTO multi-fichero (resuelve ``import``) a ``.velb``.

        :param uri: URI del ``.vx`` raiz del proyecto.
        :returns: igual que :meth:`compile`.
        """
        return self.compile(uri, output=output, mode=mode, project=True,
                            debug=debug, instrument=instrument)

    # ------------------------------------------------------------------ #
    #  Helpers internos                                                   #
    # ------------------------------------------------------------------ #
    @staticmethod
    def _with_target(params: Dict[str, Any], os_: Optional[str],
                     arch: Optional[str]) -> Dict[str, Any]:
        if os_:
            params["os"] = os_
        if arch:
            params["arch"] = arch
        return params


def decode_semantic_tokens(data: List[int]) -> List[tuple]:
    """Decodifica el array plano de semantic tokens (quintetos con deltas).

    El protocolo LSP codifica cada token como 5 enteros
    ``(deltaLine, deltaStart, length, tokenType, tokenModifiers)`` relativos al
    token anterior.  Esta funcion los pasa a posiciones absolutas.

    :param data: el array ``result["data"]`` de :meth:`VestaLspClient.semantic_tokens`.
    :returns: lista de tuplas ``(linea, columna, longitud, tipo, modificadores)``
        en coordenadas absolutas 0-based.
    """
    out = []
    line = col = 0
    for i in range(0, len(data) - 4, 5):
        dl, ds, length, typ, mods = data[i:i + 5]
        if dl == 0:
            col += ds
        else:
            line += dl
            col = ds
        out.append((line, col, length, typ, mods))
    return out
