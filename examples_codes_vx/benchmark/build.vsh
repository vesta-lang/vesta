import "stdlib/vsh/build_lib.vsh"


fn get_extension(filename) {
    """
    Permite obtener la extension de un archivo dado su nombre. 
    Se asume que el nombre del archivo tiene una extensión separada por un punto.
    """
    let parts = split(filename, ".")
    if (len(parts) < 2) { return "" }
    return parts[len(parts) - 1]
}

fn stem(filename) {
    """
    Permite obtener el nombre base de un archivo dado su nombre. 
    Se asume que el nombre del archivo tiene una extensión separada por un punto.
    """
    let parts = split(filename, ".")
    return parts[0]
}

//println("${ansi_clear()} args ${ARGV[1]} len ${len(ARGV)}")

bl_init()
bl_locate_vm()

bl_h1("VEXED build (" + bl_os() + ")")
println("VM:        " + bl_vm_exe)
println("CWD:       " + getcwd())


let benchmark_dir = ""
if __file__ != null and len(__file__) > 0 {
    benchmark_dir = dirname(__file__)
}
if len(benchmark_dir) == 0 or not is_dir(benchmark_dir) {
    benchmark_dir = bl_path_join(getcwd(), "examples_codes_vex/benchmark")
}
if not is_dir(benchmark_dir) {
    bl_fail("No se localiza el directorio benchmark/.  build.vsh debe vivir en el proyecto.")
    bl_summary()
}
bl_ok("benchmark_dir: " + benchmark_dir)

// Carpeta donde caen los .velb generados.  Por defecto un subdirectorio
// `out/` dentro del proyecto benchmark (NO en el dir del interprete, que
// puede ser de solo lectura como Program Files).  El usuario puede
// override con la variable de entorno VEXED_OUT_DIR.
let out_dir = bl_path_join(benchmark_dir, "out")
let env_out = getenv("VEXED_OUT_DIR")
if env_out != null and len(env_out) > 0 {
    out_dir = env_out
}

// Crear el dir si no existe.  shell_ex es portable -- mkdir -p en unix
// y `if not exist X mkdir X` en cmd.
if not is_dir(out_dir) {
    if bl_is_windows() {
        shell("if not exist " + bl_quote_arg(out_dir) + " mkdir " + bl_quote_arg(out_dir))
    } else {
        shell("mkdir -p " + bl_quote_arg(out_dir))
    }
}
bl_set_out_dir(out_dir)
bl_ok("out_dir:    " + out_dir)


let files: list =  listdir(".")
for file in files {
    if (get_extension(file) == "vex") {
        bl_h1("Compilando ${file}")
        bl_compile_vex_with(bl_path_join(benchmark_dir, file), stem(file), "--ir-opt 3")
        bl_compile_vex_with(bl_path_join(benchmark_dir, file), stem(file), "--vex-emit-ir --ir-opt 3")
    }
}


bl_summary()
