

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
    
    println("compilando archivos .ir en el directorio actual")

    if (shell_ex("vesta -h")["code"] != 0) {
        println("vesta no esta instalada")
        exit(-1)
    }

    let files: list =  listdir(".")
    for file in files {

        if (get_extension(file) == "ir") {
            println("encontrado ${get_extension(file)} -> ${yellow(file)}")

            let name: str = split(file, ".")[0]
            // no usar secuencias ansi en el comandos, ya que pueden causar problemas al ejecutar el comando
            let command: str = "vesta --ir-file ${file} --ir-opt 3 --ir-emit-only -o ${name}" 

            println("compilando ${command}")
            let res: map = shell_ex(command)
            if (res["code"] != 0) {
                println("${red()}error al compilar ${name}.ir${white()}")
                println(res["output"])
            }
        }
    }

}
