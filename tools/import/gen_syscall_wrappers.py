#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generador de los wrappers tipados de std.syscall.linux.

Fuente de firmas: los JSON de https://api.syscall.sh/ descargados en
tools/import/syscall_data/{x64,x86,arm64,arm}.json (name + arg0..arg5 con el
tipo C real).  Para cada syscall cuya constante `_NR_*` existe en el/los arch
del repo (linux/x86_64.vx, linux/x86_32.vx) se emite un wrapper que castea
`invoke` al `cfn` con la aridad/tipos reales y pasa el `_NR_*` como primer
argumento.

Patron INVOKE DIRECTO (no el ctx): el compilador INLINA la llamada -> codigo
minimo, identico en x86-64 y x86-32.  El ctx via CALLIND rompe en x86-32 porque
el 1er argumento colisiona con EAX (donde va el id de la syscall) en regparm3.

Un wrapper solo se emite si su `_NR_*` existe:
  - en ambos     -> wrapper cross-arch.
  - solo x86_64  -> `@Target("arch:x86_64 && !os:windows")`.
  - solo x86     -> `@Target("arch:x86 && !os:windows")`.

ASCII (excepto la enye) + comentarios en espanol.  Salida:
stdlib/vx/std/syscall/linux.vx.
"""
import json
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
SYS = os.path.join(ROOT, "stdlib", "vx", "std", "syscall")
DATA = os.path.join(HERE, "syscall_data")

# Retorno especial por nombre.  El resto retorna `ssize_t` (el long/isize que
# el syscall deja en RAX/EAX; el usuario castea a i32 para un fd, o compara con
# 0 para exito/-errno).
# Tipo de retorno por nombre.  Default = i32 (fd/pid/status/-errno).  El syscall
# deja un long en RAX/EAX; para un fd/pid basta i32 (el error -errno cabe).
RET_PTR = {"mmap", "mremap", "brk", "shmat", "mmap2"}  # -> uintptr

# Syscalls que hablan de REGIONES de memoria.  En ellas, un argumento llamado
# `addr` (o equivalente) no es un puntero a datos que nadie vaya a
# dereferenciar: es la DIRECCION de una region, y el kernel la trata como un
# numero -- de hecho el propio kernel la declara `void *` en unas
# (`mmap`) y `unsigned long` en otras (`munmap`).
#
# Traducir esa asimetria literalmente dejaba la capa incoherente CONSIGO MISMA:
# `mmap` devolvia `void *` y `munmap` pedia `usize`, asi que encadenarlas
# obligaba a un cast aunque el programa no saliera en ningun momento de los
# tipos de la libreria.  Aqui todas usan `uintptr`, que es el tipo del
# lenguaje para "direccion".
MEM_SYSCALLS = {
    "mmap", "mmap2", "munmap", "mremap", "brk", "mprotect", "madvise",
    "msync", "mlock", "mlock2", "munlock", "mincore", "shmat", "shmdt",
    "remap_file_pages", "process_madvise", "pkey_mprotect", "mbind",
    "migrate_pages", "move_pages", "get_mempolicy", "set_mempolicy",
}
# Nombres de argumento que denotan una direccion dentro de esas syscalls.
ADDR_ARG_NAMES = {"addr", "start", "old_address", "new_address", "shmaddr",
                  "shmadr"}
RET_VOID = {"exit", "exit_group", "rt_sigreturn", "restart_syscall"}  # no retorna
RET_I64 = {  # retornan un offset/tamano/puntero-como-entero de ancho completo
    "lseek", "getcwd", "times", "ptrace", "set_tid_address", "get_robust_list",
    "set_robust_list", "kcmp", "io_getevents",
}
RET_SSIZE = {  # retornan un numero de BYTES (con signo)
    "read", "write", "pread64", "pwrite64", "readv", "writev", "preadv",
    "pwritev", "preadv2", "pwritev2", "recvfrom", "sendto", "recvmsg",
    "sendmsg", "recvmmsg", "sendmmsg", "readlink", "readlinkat", "getrandom",
    "copy_file_range", "splice", "tee", "vmsplice", "sendfile",
    "process_vm_readv", "process_vm_writev", "getdents", "getdents64",
    "listxattr", "llistxattr", "flistxattr", "getxattr", "lgetxattr",
    "fgetxattr",
}

# Firmas OVERRIDE para syscalls que la API marca con args "?" (desconocidos)
# pero cuya firma conocemos y es util.  El resto con "?" cae al fallback
# generico (6 args `usize`).  Cada valor es la lista de args C.
OVERRIDE = {
    "mmap": ["void *addr", "size_t length", "int prot", "int flags", "int fd",
             "loff_t offset"],
    "mmap2": ["void *addr", "size_t length", "int prot", "int flags", "int fd",
              "loff_t pgoffset"],
    "arch_prctl": ["int code", "unsigned long addr"],
    "umount2": ["const char *target", "int flags"],
}

# Envoltorios de x86-32 escritos a mano porque su firma no encaja en la
# convencion de `int 0x80`: seis argumentos ocuparian siete registros y el
# sexto caeria en EBP, que es el frame pointer.
#
# El kernel de i386 ya resolvio esto en su dia: `old_mmap` (nr 90) recibe los
# seis valores en UNA estructura y solo su direccion en un registro.  Es
# literalmente la razon de que exista.
MANUAL_X32 = {
    "mmap": '''@Target("arch:x86 && !os:windows")
public uintptr mmap(uintptr addr, size_t length, i32 prot, i32 flags, i32 fd,
                    i64 offset_) {
    static syscall_ctx_invoke ctx = syscall_ctx_invoke();
    // El _NR_mmap de i386 (90) es `old_mmap`: los seis valores viajan en una
    // estructura y al kernel solo se le pasa su DIRECCION, que es como esquiva
    // quedarse sin registros.  El offset va en BYTES (mmap2, que lo toma en
    // paginas, necesitaria los seis registros y no sirve aqui).
    u32[6] a;
    a[0] = (u32) addr;
    a[1] = (u32) length;
    a[2] = (u32) prot;
    a[3] = (u32) flags;
    a[4] = (u32) fd;
    a[5] = (u32) offset_;
    return (uintptr)((cfn(syscall_id, u32*) -> u64)(ctx.invoke_method))(
        _NR_mmap, &a[0]);
}''',
}

# Palabras que son TIPO (no nombre de variable): si el ultimo token de un arg
# es una de estas, el arg viene SIN nombre (se genera aN).
TYPE_WORDS = {
    "void", "int", "long", "short", "char", "unsigned", "signed", "float",
    "double", "struct", "union", "const", "size_t", "ssize_t", "unsigned int",
}

# Palabras reservadas de Vesta: un nombre de parametro que coincida se renombra
# con `_` al final (p.ej. `new` -> `new_` en symlink/link).
VX_KEYWORDS = {
    "new", "delete", "class", "struct", "union", "enum", "if", "else", "for",
    "while", "do", "return", "break", "continue", "switch", "case", "default",
    "match", "spawn", "async", "await", "try", "catch", "finally", "throw",
    "import", "public", "private", "protected", "static", "const", "void",
    "this", "super", "true", "false", "null", "in", "is", "as", "typedef",
    "using", "namespace", "register", "asm", "cfn", "fn", "type", "unique",
    "shared", "borrow", "comptime", "extern", "monitor", "synchronized",
    "auto", "var", "let", "sizeof", "alignof", "operator", "yield", "resume",
}


# Keywords de datos del inline-asm (times/db/dw/... son especiales en el lexer).
ASM_WORDS = {"times", "db", "dw", "dd", "dq", "byte", "word", "dword", "qword",
             "ptr", "offset"}

# Set de nombres RESERVADOS para argumentos: keywords + asm + TODOS los nombres
# de syscall generados (un arg que se llame igual que una syscall -- p.ej. `brk`
# en brk(), `times` en utime() -- colisiona con el wrapper en el lowering).  Se
# rellena en main() antes de emitir.
#
# `ctx` entra tambien: es el nombre de la variable local que cada wrapper
# declara para su metodo de invocacion.  Un parametro con ese nombre
# (io_destroy, io_setup) quedaba SOMBREADO por ella y al kernel se le pasaba el
# struct en lugar del argumento -- mal en silencio salvo por un warning de
# "argumento de tipo distinto al de la firma".
RESERVED_ARGS = set(VX_KEYWORDS) | ASM_WORDS | {"ctx"}


def sanitize(name):
    """Renombra un identificador que colisione con una keyword/nombre reservado
    de Vesta (le anade `_`)."""
    return name + "_" if name in RESERVED_ARGS else name


def sanitize_fn(name):
    """Nombre de FUNCION: solo evita las keywords del lenguaje."""
    return name + "_" if name in VX_KEYWORDS else name


def split_arg(raw, idx):
    """(tipo_C, nombre) de un arg como "const char *filename" o "int".  Genera
    aN si el arg no trae nombre."""
    s = raw.strip()
    if not s:
        return None
    # Tokenizar aislando los '*'.
    toks = s.replace("*", " * ").split()
    if not toks:
        return None
    last = toks[-1]
    has_name = (last != "*" and last not in TYPE_WORDS
                and not last.endswith("_t")
                and re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", last) is not None
                and last not in ("int", "long", "char", "void", "unsigned"))
    if has_name:
        name = sanitize(last)
        ctype = " ".join(toks[:-1])
    else:
        name = "a%d" % idx
        ctype = s
    return (map_ctype(ctype), name)


def map_ctype(ctype):
    """Tipo C -> tipo Vesta.  Aproximado pero suficiente para el ABI (lo que
    importa es el ANCHO: 32 vs ancho-de-arch, y si es puntero)."""
    t = ctype.strip()
    if t == "?" or t == "":
        return "usize"  # arg desconocido -> ancho de arch (fallback)
    if "*" in t:
        base = t.replace("*", " ").split()
        is_const = "const" in base
        if "char" in base and "unsigned" not in base:
            return "const char *" if is_const else "char *"
        return "const void *" if is_const else "void *"
    words = t.lower().split()
    joined = " ".join(words)
    if "unsigned long" in joined:
        return "usize"
    if words and words[-1] == "long" and "unsigned" not in words:
        return "ssize_t"  # long -> isize (ancho de arch, con signo)
    if "size_t" in words:
        return "size_t"
    # off_t/loff_t/time_t/clockid en 64 -> i64.
    if any(w in words for w in ("off_t", "loff_t", "time_t")):
        return "i64"
    if "u64" in words:
        return "u64"
    if "u32" in words or "__u32" in words:
        return "u32"
    if "__s32" in words:
        return "i32"
    if "unsigned" in words:  # unsigned int / unsigned
        return "u32"
    if t.endswith("_t"):
        # unsigned-ish -> u32 ; el resto (pid_t/clockid_t/timer_t/...) -> i32.
        if t in ("uid_t", "gid_t", "umode_t", "mode_t", "key_t", "qid_t",
                 "key_serial_t", "aio_context_t"):
            return "u32"
        if t in ("loff_t", "off_t", "time_t"):
            return "i64"
        return "i32"
    if "int" in words:
        return "i32"
    return "i64"  # conservador (ancho del registro)


def load_arch(arch):
    """name -> [arg0..arg5] (los no vacios) del JSON del arch."""
    path = os.path.join(DATA, "%s.json" % arch)
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        rows = json.load(f)
    out = {}
    for r in rows:
        args = [r.get("arg%d" % i, "") for i in range(6)]
        out[r["name"]] = [a for a in args if a and a.strip()]
    return out


def read_nr(path):
    """set de nombres (sin _NR_) con constante definida en el .vx del arch."""
    names = set()
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for m in re.finditer(r"\b_NR_([A-Za-z0-9_]+)", f.read()):
            names.add(m.group(1))
    return names


def wrapper(name, cargs):
    """Texto del wrapper (invoke directo) dado el nombre y sus args C."""
    parsed = []
    for i, ca in enumerate(cargs):
        p = split_arg(ca, i)
        if p is not None:
            parsed.append(p)
    # En las syscalls de memoria, el argumento de direccion es `uintptr`
    # independientemente de como lo declare el kernel (ver MEM_SYSCALLS).
    if name in MEM_SYSCALLS:
        parsed = [("uintptr", n) if n in ADDR_ARG_NAMES else (t, n)
                  for (t, n) in parsed]
    # Tipo de retorno.
    if name in RET_VOID:
        rdecl, rcast = "void", None
    elif name in RET_PTR:
        rdecl, rcast = "uintptr", "(uintptr)"
    elif name in RET_SSIZE:
        rdecl, rcast = "ssize_t", "(ssize_t)"
    elif name in RET_I64:
        rdecl, rcast = "i64", "(i64)"
    else:
        rdecl, rcast = "i32", "(i32)"  # fd/pid/status/-errno
    fn = sanitize_fn(name)  # nombre publico (evita keyword); _NR_ usa el original
    params = ", ".join("%s %s" % (t, n) for (t, n) in parsed)
    cfn_types = "syscall_id" + "".join(", %s" % t for (t, _n) in parsed)
    call_args = "_NR_%s" % name + "".join(", %s" % n for (_t, n) in parsed)
    # Cada wrapper mantiene su PROPIO `static syscall_ctx_invoke ctx`: el metodo
    # de invocacion es PERSONALIZABLE por-syscall (reasignar `ctx.invoke_method`
    # a un invoke alternativo).  Se castea `ctx.invoke_method` al cfn con la
    # aridad/tipos reales y se pasa el `_NR_*` como 1er argumento.
    ctx = "    static syscall_ctx_invoke ctx = syscall_ctx_invoke();"
    call = "((cfn(%s) -> u64)(ctx.invoke_method))(%s)" % (cfn_types, call_args)
    if rdecl == "void":
        return "public void %s(%s) {\n%s\n    %s;\n}" % (fn, params, ctx, call)
    return "public %s %s(%s) {\n%s\n    return %s%s;\n}" % (
        rdecl, fn, params, ctx, rcast, call)


def main():
    sig64 = load_arch("x64")
    sig32 = load_arch("x86")
    nr64 = read_nr(os.path.join(SYS, "linux", "x86_64.vx"))
    nr32 = read_nr(os.path.join(SYS, "linux", "x86_32.vx"))

    # Poblar los nombres reservados para argumentos con TODOS los nombres de
    # syscall (un arg que se llame igual que otra syscall colisiona con el
    # wrapper en el lowering).
    RESERVED_ARGS.update(nr64 | nr32)

    both, only64, only32 = [], [], []
    for name in sorted(nr64 | nr32):
        in64, in32 = name in nr64, name in nr32
        # OVERRIDE > firma de la API (x64 primero).
        if name in OVERRIDE:
            cargs = OVERRIDE[name]
        elif name in sig64:
            cargs = sig64[name]
        elif name in sig32:
            cargs = sig32[name]
        else:
            continue  # sin firma en la API -> no generamos (raro/interno)
        # LIMITACION x86-32: seis argumentos no caben.  La convencion de
        # `int 0x80` usa eax + ebx/ecx/edx/esi/edi/ebp, o sea SIETE de los ocho
        # registros, y el sexto cae en EBP -- que es el frame pointer.
        # Colocarlo ahi destruye el marco de quien llama, asi que el envoltorio
        # compilaba y luego moria al retornar.
        #
        # La salida historica de Linux para i386 es `old_mmap` (nr 90): recibe
        # UN puntero a una estructura con los seis valores, y por eso no
        # necesita el sexto registro.  Implementarlo pide una variante propia
        # por arquitectura (construir la estructura y pasar su direccion), y
        # esta pendiente.
        #
        # Mientras tanto NO se declaran en 32 bits.  Asi quien las use recibe un
        # error de COMPILACION que dice que solo existen para otro objetivo, en
        # vez de un binario que segfaltea sin explicacion.
        if len([a for a in cargs if a.strip()]) >= 6:
            in32 = False
            if not in64:
                continue
        if in64 and in32:
            both.append((name, cargs))
        elif in64:
            only64.append((name, cargs))
        else:
            only32.append((name, cargs))

    out = []
    out.append("namespace std.syscall.linux;")
    out.append("")
    out.append("import std.syscall.abi only invoke_syscall, syscall_id;")
    out.append("")
    out.append("import std.types only nullptr, size_t, ssize_t, uintptr, usize;")
    out.append("")
    out.append("// trae al scope TODA la implementacion del arch (invoke + los")
    out.append("// 400+ _NR_*) y la re-exporta.")
    out.append('@Target("arch:x86_64 && !os:windows")')
    out.append("public import std.syscall.linux.x86_64 only *;")
    out.append("")
    out.append('@Target("arch:x86 && !os:windows")')
    out.append("public import std.syscall.linux.x86_32 only *;")
    out.append("")
    out.append("// `syscall_ctx_invoke` permite PERSONALIZAR el metodo de")
    out.append("// invocacion por-syscall (reasignar `invoke_method`).  Los")
    out.append("// wrappers de abajo usan el `invoke` DIRECTO (se INLINA ->")
    out.append("// codigo minimo, cross-arch).")
    out.append("public struct syscall_ctx_invoke {")
    out.append("    invoke_syscall invoke_method = invoke;")
    out.append("};")
    out.append("")
    out.append("// ===================================================================")
    out.append("// Wrappers tipados de las syscalls Linux.  GENERADOS por")
    out.append("// tools/import/gen_syscall_wrappers.py desde las firmas de")
    out.append("// api.syscall.sh -- NO EDITAR A MANO.")
    out.append("//")
    out.append("// Cada wrapper mantiene su PROPIO `static syscall_ctx_invoke ctx` y")
    out.append("// castea `ctx.invoke_method` al `cfn` con la aridad/tipos reales,")
    out.append("// pasando el `_NR_*` como 1er argumento.  Reasignar `ctx.invoke_method`")
    out.append("// permite PERSONALIZAR como se invoca cada syscall.  Retorno: i32")
    out.append("// (fd/pid/-errno) salvo I/O -> ssize_t, mmap/brk -> uintptr, lseek -> i64.")
    out.append("// ===================================================================")
    out.append("")
    out.append("// --- Presentes en x86-64 Y x86-32 (cross-arch) ---")
    out.append("")
    out.append("\n\n".join(wrapper(n, a) for (n, a) in both))
    if only64:
        out.append("")
        out.append("")
        out.append("// --- Solo x86-64 ---")
        out.append("")
        for (n, a) in only64:
            out.append('@Target("arch:x86_64 && !os:windows")')
            out.append(wrapper(n, a))
            out.append("")
    if only32:
        out.append("")
        out.append("// --- Solo x86-32 ---")
        out.append("")
        for (n, a) in only32:
            out.append('@Target("arch:x86 && !os:windows")')
            out.append(wrapper(n, a))
            out.append("")
    if MANUAL_X32:
        out.append("")
        out.append("// --- x86-32: escritos a mano ---")
        out.append("//")
        out.append("// Los de SEIS argumentos no caben en la convencion de")
        out.append("// `int 0x80` (ver la nota de arriba); el kernel de i386")
        out.append("// ofrece para ellos una entrada que recibe menos")
        out.append("// registros, y es la que se usa aqui.")
        out.append("")
        for n in sorted(MANUAL_X32):
            out.append(MANUAL_X32[n])
            out.append("")
    out.append("")

    dst = os.path.join(SYS, "linux.vx")
    with open(dst, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out))
    print("generado %s" % dst)
    print("  cross-arch: %d | solo x64: %d | solo x32: %d"
          % (len(both), len(only64), len(only32)))


if __name__ == "__main__":
    main()
