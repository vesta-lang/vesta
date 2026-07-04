

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

if __name__ == "__main__" {

    println("${ansi_clear()}")
    
    println("compilando archivos .vx en el directorio actual")

    if (shell_ex("vesta -h")["code"] != 0) {
        println("vesta no esta instalada")
        exit(-1)
    }

    println("compilar a .velb o solo emitir a .vel?(vel/velb/ir/c/html): ")
    let build_or_emmit = input()

    if build_or_emmit == "velb" {
        build_or_emmit = ""
    } elif build_or_emmit == "ir" {
        build_or_emmit = "--vx-emit-ir"
    } elif build_or_emmit == "c" {
        build_or_emmit = "--port c"
    } elif build_or_emmit == "html" {
        build_or_emmit = "--diagram-all --diagram-format all"
    } else {
        build_or_emmit = "--vx-emit-only"
    }
    build_or_emmit = build_or_emmit //+ " --instrument trace "
    let files: list =  listdir(".")
    for file in files {

        if (get_extension(file) == "vx") {
            println("encontrado ${get_extension(file)} -> ${yellow(file)}")

            let name: str = split(file, ".")[0]
            // no usar secuencias ansi en el comandos, ya que pueden causar problemas al ejecutar el comando
            let command: str = "vesta --vx ${file} --ir-opt 3 ${build_or_emmit} -o ${name}" 

            println("compilando ${command}")
            let res: map = shell_ex(command)
            if (res["code"] != 0) {
                println("${red()}error al compilar ${name}.vx${white()}")
                println(res["output"])
            }
        }
    }

}
