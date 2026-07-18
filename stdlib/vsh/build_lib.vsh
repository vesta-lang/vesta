// =============================================================================
// stdlib/vsh/build_lib.vsh - libreria de construccion portable y multiplataforma
// =============================================================================
//
// Coleccion de helpers VSH para escribir scripts de build (`build.vsh`)
// que sean PORTABLES entre Windows / Linux / macOS y RECICLABLES entre
// proyectos.  No depende de make, ninja o cmake -- el unico requisito es
// que `vm[.exe]` este accesible (en PATH o en una ruta conocida).
//
// USO:
//
//   import "stdlib/vsh/build_lib.vsh"
//
//   bl_init()                              // inicializa estado interno
//   bl_locate_vm()                         // localiza vm[.exe]
//   bl_h1("Compilando proyecto MiProj")    // titulo
//   bl_compile_vex("src/main.vex", "main") // src + nombre base de salida
//   bl_compile_vex("src/lib.vex",  "lib")
//   bl_summary()                           // tabla final + exit code
//
// Funciones publicas:
//
//   bl_init()                  resetea el estado interno (done/failed/warns)
//   bl_os()                    devuelve "windows" / "linux" / "macos"
//   bl_is_windows()            true si bl_os() == "windows"
//   bl_path_sep()              "\\" en windows, "/" en unix
//   bl_path_join(a, b)         junta dos paths con el separador correcto
//   bl_exe_ext()               ".exe" en windows, "" en unix
//   bl_normalize_path(p)       convierte separadores al estilo del OS host
//   bl_quote_arg(s)            escapa un argumento para shell (anade comillas)
//
//   bl_locate_vm([opt_dir])    busca vm[.exe] en (opt_dir, ., ./build,
//                              ./cmake-build-*, getenv VESTAVM_BIN, PATH).
//                              guarda en bl_vm_exe global.  exit(2) si no encuentra.
//   bl_set_out_dir(dir)        define el directorio de salida de los .velb
//
//   bl_h1(text)                titulo de seccion con marco
//   bl_h2(text)                titulo de subseccion (sin marco)
//   bl_ok(msg)                 marca paso OK + apila a bl_done
//   bl_warn(msg)               marca warning + apila a bl_warns
//   bl_fail(msg)               marca FAIL + apila a bl_failed
//   bl_step(name, cmd)         ejecuta cmd via shell_ex; reporta OK/FAIL.
//                              devuelve el exit code.
//
//   bl_compile_vex(src, out)            compila .vex con vm --vex
//   bl_compile_vex_with(src, out, args) idem + flags extra (string)
//   bl_run_velb(velb_path, run_args)    ejecuta .velb (run_args es lista)
//   bl_run_e2e_suite(script_path)       corre script bash de tests + parsea
//
//   bl_summary()              imprime resumen final + exit(0/1) segun fails
//   bl_elapsed_ms()           ms transcurridos desde bl_init()
//
// Estado interno (variables globales accesibles desde el script principal):
//   bl_done   list[string]   pasos completados con exito
//   bl_failed list[string]   pasos que fallaron
//   bl_warns  list[string]   advertencias
//   bl_vm_exe string         ruta absoluta del vm[.exe] localizado
//   bl_out_dir string        directorio donde caen los .velb (default ".")
//   bl_t0     int            time_ms() del bl_init()

let bl_done    = []
let bl_failed  = []
let bl_warns   = []
let bl_vm_exe  = ""
let bl_out_dir = "."
let bl_t0      = 0

// -----------------------------------------------------------------------------
// Detección de plataforma y helpers de path
// -----------------------------------------------------------------------------

fn bl_os() {
    return platform()
}

fn bl_is_windows() {
    return platform() == "windows"
}

fn bl_is_unix() {
    let p = platform()
    return p == "linux" or p == "macos"
}

fn bl_path_sep() {
    if bl_is_windows() { return "\\" }
    return "/"
}

fn bl_exe_ext() {
    if bl_is_windows() { return ".exe" }
    return ""
}

fn bl_path_join(a, b) {
    if len(a) == 0 { return b }
    let sep = bl_path_sep()
    let last = substr(a, len(a) - 1, 1)
    if last == "/" or last == "\\" {
        return a + b
    }
    return a + sep + b
}

fn bl_normalize_path(p) {
    // Convierte separadores al estilo del OS.  No-op en unix (ya usa /);
    // en Windows convierte / a \ para que cmd.exe los acepte.
    if bl_is_windows() {
        return replace(p, "/", "\\")
    }
    return p
}

fn bl_quote_arg(s) {
    // Envuelve en comillas dobles si contiene espacios o caracteres
    // problematicos para el shell.  Los \ no se escapan porque cmd.exe
    // los preserva literalmente dentro de "..." (a diferencia de bash).
    if contains(s, " ") or contains(s, "\t") {
        return "\"" + s + "\""
    }
    return s
}

// -----------------------------------------------------------------------------
// Inicializacion / estado
// -----------------------------------------------------------------------------

fn bl_init() {
    bl_done    = []
    bl_failed  = []
    bl_warns   = []
    bl_t0      = time_ms()
}

fn bl_elapsed_ms() {
    return time_ms() - bl_t0
}

fn bl_set_out_dir(dir) {
    bl_out_dir = dir
}

// -----------------------------------------------------------------------------
// Localizacion del vm
// -----------------------------------------------------------------------------

fn bl_locate_vm() {
    let exe = "vm" + bl_exe_ext()
    let candidates = []
    // 1. Variable de entorno VESTAVM_BIN (override explicito).
    // getenv() devuelve null si la variable no esta definida.
    let envv = getenv("VESTAVM_BIN")
    if envv != null and len(envv) > 0 { append(candidates, envv) }
    // 2. interpreter_path() / interpreter_dir() exponen donde esta el vm
    // que esta corriendo este script.  Es el caso normal: el usuario
    // invoca `vm --script ...` y queremos REUTILIZAR ese mismo vm.
    let ip = interpreter_path()
    if ip != null and len(ip) > 0 { append(candidates, ip) }
    let id = interpreter_dir()
    if id != null and len(id) > 0 { append(candidates, bl_path_join(id, exe)) }
    // 3. Directorios comunes relativos al cwd (caso build-tree).
    let here = getcwd()
    append(candidates, bl_path_join(here, exe))
    append(candidates, bl_path_join(bl_path_join(here, "build"), exe))
    append(candidates, bl_path_join(bl_path_join(here, "cmake-build-windows"), exe))
    append(candidates, bl_path_join(bl_path_join(here, "cmake-build-release"), exe))
    append(candidates, bl_path_join(bl_path_join(here, "cmake-build-debug"), exe))
    append(candidates, bl_path_join(bl_path_join(here, "cmake-build-linux"), exe))
    // 4. parent ../bin
    append(candidates, bl_path_join(bl_path_join(dirname(here), "bin"), exe))
    // Probar uno a uno
    for c in candidates {
        if is_file(c) {
            bl_vm_exe = c
            return c
        }
    }
    bl_fail("vm[.exe] no encontrado.  Define VESTAVM_BIN o pon vm en una de:")
    for c in candidates {
        println("       " + c)
    }
    exit(2)
}

// -----------------------------------------------------------------------------
// Output con color (usa builtins colorize / println de VSH)
// -----------------------------------------------------------------------------

fn bl_h1(text) {
    println("")
    println(colorize("==================================================", "cyan"))
    println(colorize(" " + text, "bold"))
    println(colorize("==================================================", "cyan"))
}

fn bl_h2(text) {
    println("")
    println(colorize("--- " + text + " ---", "bold"))
}

fn bl_ok(msg) {
    println(colorize("[OK]   ", "green") + msg)
    append(bl_done, msg)
}

fn bl_warn(msg) {
    println(colorize("[WARN] ", "yellow") + msg)
    append(bl_warns, msg)
}

fn bl_fail(msg) {
    println(colorize("[FAIL] ", "red") + msg)
    append(bl_failed, msg)
}

// -----------------------------------------------------------------------------
// Ejecucion de comandos
// -----------------------------------------------------------------------------

fn bl_step(name, cmd) {
    println(colorize("> ", "cyan") + cmd)
    let r = shell_ex(cmd)
    if r["code"] == 0 {
        bl_ok(name)
    } else {
        bl_fail(name + " (code=" + str(r["code"]) + ")")
        let out = r["output"]
        if len(out) > 0 {
            let lines = split(out, "\n")
            let n = len(lines)
            let start = 0
            if n > 8 { start = n - 8 }
            let i = start
            while i < n {
                println("       " + lines[i])
                i = i + 1
            }
        }
    }
    return r["code"]
}

// -----------------------------------------------------------------------------
// Compilacion de Vex
// -----------------------------------------------------------------------------

fn bl_compile_vex(src, out_basename) {
    return bl_compile_vex_with(src, out_basename, "")
}

fn bl_compile_vex_with(src, out_basename, extra_flags) {
    if len(bl_vm_exe) == 0 {
        bl_fail("bl_compile_vex: vm no localizado.  Llama bl_locate_vm() antes.")
        return 1
    }
    if not is_file(src) {
        bl_fail("Fuente no encontrada: " + src)
        return 1
    }
    let out_path = bl_path_join(bl_out_dir, out_basename)
    let cmd = bl_quote_arg(bl_vm_exe) + " --vex " + bl_quote_arg(src) + " -o " + bl_quote_arg(out_path)
    if len(extra_flags) > 0 { cmd = cmd + " " + extra_flags }
    return bl_step("Compilar " + basename(src), cmd)
}

fn bl_run_velb(velb_path, run_args) {
    if len(bl_vm_exe) == 0 {
        bl_fail("bl_run_velb: vm no localizado.")
        return 1
    }
    if not is_file(velb_path) {
        bl_fail("Velb no encontrado: " + velb_path)
        return 1
    }
    let cmd = bl_quote_arg(bl_vm_exe) + " --run " + bl_quote_arg(velb_path)
    for a in run_args { cmd = cmd + " " + bl_quote_arg(a) }
    return bl_step("Ejecutar " + basename(velb_path), cmd)
}

// Corre un script bash de tests y parsea las lineas OK:/FAIL: del output.
// Retorna mapa {"ok": int, "fail": int, "code": int}.
fn bl_run_e2e_suite(script_path, build_dir) {
    if not is_file(script_path) {
        bl_warn("Suite no encontrada: " + script_path)
        let m = {}
        m["ok"]   = 0
        m["fail"] = 0
        m["code"] = -1
        return m
    }
    // Necesita bash en PATH (Git Bash en Windows, /bin/bash en unix).
    let cmd = "bash " + bl_quote_arg(script_path) + " " + bl_quote_arg(build_dir)
    println(colorize("> ", "cyan") + cmd)
    let r = shell_ex(cmd)
    let oks = 0
    let fails = 0
    let lines = split(r["output"], "\n")
    let i = 0
    while i < len(lines) {
        let ln = lines[i]
        if starts_with(ln, "OK:")   { oks   = oks   + 1 }
        if starts_with(ln, "FAIL:") { fails = fails + 1 }
        i = i + 1
    }
    if r["code"] == 0 {
        bl_ok("Suite e2e: " + str(oks) + " OK / " + str(fails) + " FAIL")
    } else {
        bl_fail("Suite e2e fallo (code=" + str(r["code"]) + ", " + str(oks) + " OK, " + str(fails) + " FAIL)")
    }
    let m = {}
    m["ok"]   = oks
    m["fail"] = fails
    m["code"] = r["code"]
    return m
}

// -----------------------------------------------------------------------------
// Resumen final + exit
// -----------------------------------------------------------------------------

fn bl_summary() {
    bl_h1("Resumen")
    println(colorize("Pasos OK:    ", "green")  + str(len(bl_done)))
    println(colorize("Warnings:    ", "yellow") + str(len(bl_warns)))
    println(colorize("Pasos FAIL:  ", "red")    + str(len(bl_failed)))
    let ms = bl_elapsed_ms()
    println("Tiempo:       " + str(ms / 1000) + "." + str(ms % 1000) + "s")
    if len(bl_failed) > 0 {
        println("")
        println(colorize("FALLOS:", "red"))
        for f in bl_failed {
            println("  - " + f)
        }
        exit(1)
    }
    if len(bl_warns) > 0 {
        println("")
        println(colorize("Warnings:", "yellow"))
        for w in bl_warns {
            println("  - " + w)
        }
    }
    println("")
    println(colorize("BUILD OK", "green") + " (" + bl_os() + ")")
    exit(0)
}
