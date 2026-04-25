# VestaShell (.vsh) — Referencia del lenguaje de scripting

VestaShell es el lenguaje de scripting embebido en VestaVM. Permite automatizar
tareas en el REPL, glue code entre modulos y scripting de sistema con acceso
completo al filesystem, shell y matematicas.

Los scripts tienen extension `.vsh` y se ejecutan con:

```
// Dentro del REPL (comando "script")
vesta> script mi_script.vsh

// Abrir el REPL interactivo VestaShell desde dentro del REPL
vesta> vsh

// Linea de comandos (sin abrir el REPL)
./vm --script mi_script.vsh

// REPL interactivo VestaShell al estilo Python (desde linea de comandos)
./vm --interprete
```

Los ficheros de estado (`vm_env.txt`, `vm_history.txt`, `vm_aliases.txt`) se
crean siempre junto al ejecutable, independientemente del directorio de trabajo
actual.

Los builtins son visibles como valores de primera clase: escribir `help` sin
parentesis en el REPL lo invoca automaticamente (comportamiento estilo Python).

---

## Indice

1. [Tipos de datos](#tipos-de-datos)
2. [Variables y anotaciones de tipo](#variables-y-anotaciones-de-tipo)
3. [Operadores](#operadores)
4. [Strings e interpolacion](#strings-e-interpolacion)
5. [Control de flujo](#control-de-flujo)
6. [Funciones](#funciones)
7. [Listas](#listas)
8. [Mapas](#mapas)
9. [Clases y POO](#clases-y-poo)
10. [Manejo de errores](#manejo-de-errores)
11. [Importacion de modulos](#importacion-de-modulos)
12. [Funciones integradas](#funciones-integradas)
13. [Sockets y red](#sockets-y-red)
14. [REPL interactivo VestaShell](#repl-interactivo-vestaShell)
15. [Integracion con el REPL](#integracion-con-el-repl)
16. [Comentarios y continuacion de linea](#comentarios-y-continuacion-de-linea)

---

## Tipos de datos

| Tipo       | Descripcion                                | Ejemplo                |
|------------|--------------------------------------------|------------------------|
| `null`     | Ausencia de valor                          | `null`                 |
| `bool`     | Booleano                                   | `true`, `false`        |
| `int`      | Entero de 64 bits con signo                | `42`, `-7`, `1_000`    |
| `float`    | Flotante IEEE 754 de 64 bits               | `3.14`, `-0.5`         |
| `string`   | Cadena de texto UTF-8                      | `"Hola"`, `'mundo'`    |
| `list`     | Lista dinamica de valores                  | `[1, "dos", true]`     |
| `map`      | Diccionario string -> valor                | `{"k": 1, "v": 2}`    |
| `function` | Funcion de primera clase (con closure)     | `fn(x) { return x*2 }` |
| `class`    | Descriptor de clase                        | `class Punto { ... }`  |
| `instance` | Instancia de clase                         | `Punto(1, 2)`          |

---

## Variables y anotaciones de tipo

```vsh
let nombre = "VestaShell"   // declaracion
nombre = "vsh"              // reasignacion (la variable ya debe existir)

let x = 10
let y = x + 5               // expresion en la inicializacion
let z = null                // null explicito
```

Las variables son dinamicamente tipadas por defecto. Opcionalmente se puede
anotar el tipo con `: tipo` para que el interprete lo verifique en tiempo de
ejecucion:

```vsh
let n: int    = 42          // solo acepta int
let s: str    = "hola"      // solo acepta str / string
let f: float  = 3.14        // solo acepta float
let b: bool   = true        // solo acepta bool
let lst: list = [1, 2, 3]   // solo acepta list
let mp: map   = {"k": 1}    // solo acepta map

// Error si el tipo no coincide:
let x: int = "texto"        // RuntimeError: variable 'x': se esperaba tipo 'int'
```

Nombres de tipo aceptados en anotaciones: `null`, `bool`, `int`, `float`,
`str`/`string`, `list`, `map`, `fn`/`function`, `class`, `instance`,
o el nombre de cualquier clase definida en el script.

Los parametros de funcion tambien admiten anotaciones de tipo:

```vsh
fn doble(x: int) { return x * 2 }
fn greet(nombre: str, edad) {       // 'edad' sin tipo: acepta cualquier valor
    return nombre + " tiene " + str(edad)
}

doble(5)        // ok
doble("text")   // RuntimeError: parametro 'x': se esperaba tipo 'int'
```

Las subclases pasan el check de tipo de la clase padre:

```vsh
class Animal { fn __init__(self, n) { self.nombre = n } }
class Perro : Animal { }

fn cuidar(a: Animal) { println(a.nombre) }
cuidar(Perro("Rex"))    // ok: Perro es subclase de Animal
```

---

## Operadores

### Aritmeticos

| Operador | Descripcion         | Ejemplo          |
|----------|---------------------|------------------|
| `+`      | Suma / concatenacion| `3 + 4`, `"a"+"b"` |
| `-`      | Resta / negacion    | `10 - 3`, `-x`   |
| `*`      | Multiplicacion      | `3 * 4`          |
| `/`      | Division            | `10 / 3` → `3`   |
| `%`      | Modulo              | `10 % 3` → `1`   |
| `**`     | Potencia (der)      | `2 ** 10` → `1024`|

### Comparacion

```vsh
a == b    a != b    a < b    a <= b    a > b    a >= b
```

Los tipos `int` y `float` son iguales si el valor numerico coincide: `1 == 1.0`.

### Logicos

```vsh
not expr          // negacion booleana
a and b           // conjuncion (cortocircuito)
a or  b           // disyuncion (cortocircuito)
```

### Asignacion compuesta

```vsh
x += 5     x -= 3     x *= 2     x /= 4     x %= 3
```

---

## Strings e interpolacion

```vsh
let s = "Hola mundo"
let t = 'tambien funciona con comilla simple'

// Interpolacion: ${expresion} dentro de la cadena
let nombre = "Ada"
let n = 36
let msg = "Hola ${nombre}, tienes ${n} anos"

// Las expresiones pueden ser arbitrarias (sin comillas anidadas)
let pi = 3.14159
let area = "El area es ${pi * 4.0}"
```

**Nota**: evitar usar comillas del mismo tipo dentro de `${}`.
Usar variables intermedias cuando sea necesario:

```vsh
let arg = "mundo"
let r = "${saludo(arg)}"    // correcto
// let r = "${saludo("mundo")}"  // produce error de sintaxis
```

### Escapes en strings

| Secuencia | Caracter       |
|-----------|----------------|
| `\n`      | Nueva linea    |
| `\t`      | Tabulacion     |
| `\r`      | Retorno carro  |
| `\\`      | Barra invertida|
| `\"`      | Comilla doble  |
| `\$`      | Literal `$`    |

---

## Control de flujo

### if / elif / else

```vsh
if condicion {
    // ...
} elif otra_condicion {
    // ...
} else {
    // ...
}
```

### while

```vsh
let i = 0
while i < 10 {
    i += 1
    if i == 5 { continue }
    if i == 8 { break }
    println(str(i))
}
```

### for / in

Itera sobre listas, mapas (claves) y strings (caracteres):

```vsh
// Lista
for elem in [1, 2, 3, 4, 5] {
    println(str(elem))
}

// Mapa (da claves)
let config = {"host": "localhost", "port": 8080}
for clave in config {
    println(clave + " = " + str(config[clave]))
}

// String (da caracteres)
for c in "hola" {
    println(c)
}

// Rango numerico
for i in range(10) {          // 0..9
    println(str(i))
}
for i in range(1, 6) {        // 1..5
    println(str(i))
}
for i in range(0, 10, 2) {    // 0,2,4,6,8
    println(str(i))
}
for i in range(5, 0, -1) {    // 5,4,3,2,1
    println(str(i))
}
```

### break / continue

`break` sale del bucle mas cercano. `continue` salta a la siguiente iteracion.
Funcionan dentro de `while`, `for/in` y dentro de `try/catch` en un bucle.

---

## Funciones

```vsh
// Declaracion nombrada
fn suma(a, b) {
    return a + b
}

// Funcion anonima (lambda)
let doble = fn(x) { return x * 2 }

// Sin return explicito devuelve null
fn efecto() {
    println("hola")
}

// Las funciones son valores de primera clase
fn aplicar(f, x) { return f(x) }
let resultado = aplicar(doble, 21)   // 42
```

### Docstrings

La primera sentencia del cuerpo de una funcion puede ser un string literal. Ese
string se convierte en la documentacion de la funcion y es accesible mediante
`doc()` y `help()`:

```vsh
fn factorial(n) {
    "Calcula n! de forma recursiva. factorial(0) = 1."
    if n <= 1 { return 1 }
    return n * factorial(n - 1)
}

doc(factorial)    // "Calcula n! de forma recursiva. factorial(0) = 1."
help(factorial)   // imprime la firma y el docstring; tambien devuelve el texto
```

Reglas:
- Solo string literal simple (no interpolado) como primera sentencia.
- Las funciones sin docstring devuelven `""` en `doc()`.
- Los docstrings se propagan a traves de `import`.

### Closures

```vsh
fn contador() {
    let n = 0
    return fn() {
        n = n + 1
        return n
    }
}

let c = contador()
c()   // 1
c()   // 2
c()   // 3
```

### Funciones almacenadas en mapas

```vsh
let ops = {
    "add": fn(a, b) { return a + b },
    "mul": fn(a, b) { return a * b },
}
ops["add"](10, 5)   // 15
```

---

## Listas

```vsh
let lista = [1, "dos", true, null, [3, 4]]

// Indexado (0-based; negativo desde el final)
lista[0]    // 1
lista[-1]   // [3, 4]

// Modificar
lista[1] = "TWO"

// Funciones
len(lista)              // longitud
append(lista, valor)    // anadir al final (modifica in-place)
pop(lista)              // eliminar y devolver el ultimo elemento
contains(lista, valor)  // true si valor esta en lista
```

---

## Mapas

```vsh
let m = {
    "nombre": "Eva",
    "edad": 30,
}

// Acceso (lanza error si la clave no existe)
m["nombre"]         // "Eva"

// Asignacion (crea la clave si no existe)
m["activo"] = true

// Funciones
keys(m)             // lista de claves
values(m)           // lista de valores
contains(m, "edad") // true si la clave existe
len(keys(m))        // numero de claves

// Acceso seguro a clave posiblemente inexistente
let val = null
try {
    val = m["clave_opcional"]
} catch e {
    val = "por_defecto"
}
```

### Mapas y listas anidados

```vsh
let datos = {
    "config": { "debug": true, "workers": 4 },
    "tags":   ["prod", "backend"],
}
datos["config"]["workers"]    # 4
datos["tags"][0]              # "prod"
```

---

## Clases y POO

### Definicion de clase

```vsh
class Punto {
    "Representa un punto 2D."          // docstring de clase

    fn __init__(self, x, y) {
        self.x = x
        self.y = y
    }

    fn distancia_al_origen(self) {
        "Devuelve la distancia al origen."
        return sqrt(self.x ** 2 + self.y ** 2)
    }
}

let p = Punto(3.0, 4.0)
println(p.distancia_al_origen())    // 5.0
println(p.x)                        // 3.0
p.x = 0.0                           // asignacion de atributo
```

- El primer parametro de cada metodo es `self` por convencion.
- `__init__` es el constructor; se llama automaticamente al crear la instancia.
- Los atributos se crean dinamicamente asignando `self.campo = valor`.

### Herencia

```vsh
class Punto3D : Punto {
    fn __init__(self, x, y, z) {
        self.x = x
        self.y = y
        self.z = z
    }

    fn suma(self) {
        return self.x + self.y + self.z
    }
}

let q = Punto3D(1, 2, 3)
println(isinstance(q, Punto3D))  // true
println(isinstance(q, Punto))    // true (herencia)
println(classname(q))            // "Punto3D"
```

`super` esta disponible dentro de los metodos como referencia a la clase padre:

```vsh
class Animal {
    fn __init__(self, nombre) { self.nombre = nombre }
    fn hablar(self) { return "..." }
}

class Perro : Animal {
    fn __init__(self, nombre) { self.nombre = nombre }
    fn hablar(self) { return "Guau!" }
}
```

### Predicados OOP

```vsh
is_class(Punto)          // true
is_instance(p)           // true
isinstance(p, Punto)     // true
classname(p)             // "Punto"
doc(Punto)               // docstring de la clase
help(Punto)              // imprime metodos y docstring de la clase
```

---

## Manejo de errores

```vsh
try {
    // codigo que puede fallar
    let contenido = read_file("archivo.txt")
} catch e {
    // e es el mensaje de error como string
    println("Error: " + e)
}
```

### Lanzar errores

```vsh
error("descripcion del error")        // lanza VshRuntimeError
assert(condicion, "mensaje")          // lanza si condicion es falsa
assert(x > 0)                         // mensaje por defecto "asercion fallida"
```

Los errores de tipo `VshParseError` (sintaxis) NO son capturables con `try/catch`.

### Errores tipados con clases

VestaShell incluye la clase `Error` como base. Se pueden crear jerarquias de
errores personalizadas:

```vsh
class ValorError : Error {
    fn __init__(self, msg, valor) {
        self.message = msg
        self.valor   = valor
    }
}

class RangoError : ValorError {
    fn __init__(self, msg, valor, min, max) {
        self.message = msg
        self.valor   = valor
        self.minimo  = min
        self.maximo  = max
    }
}

// Lanzar con throw
fn validar(x) {
    if x < 0 or x > 100 {
        throw RangoError("fuera de rango", x, 0, 100)
    }
    return x
}

// Capturar con catch tipado
try {
    validar(150)
} catch e : RangoError {
    println("Rango: " + e.message)    // variable e es la instancia
    println("Valor: " + str(e.valor))
} catch e : ValorError {
    println("Valor: " + e.message)
} catch e {
    println("Error: " + e)            // catch generico (recibe string)
}
```

Reglas del catch tipado:
- Se evaluan en orden; se ejecuta el primer catch cuyo tipo coincide.
- Un catch con tipo de clase padre atrapa instancias de subclases.
- Un catch sin tipo atrapa cualquier error (incluidos strings lanzados con `throw`).
- `throw expr` con una instancia de clase envia la instancia al catch.
- `throw expr` con un valor no-instancia lanza un error de string.

### Relanzar

```vsh
try {
    operacion_riesgosa()
} catch e {
    if not starts_with(e, "error esperado") {
        error("error inesperado: " + e)
    }
}
```

---

## Importacion de modulos

```vsh
import "ruta/al/modulo.vsh"
```

- La ruta es relativa al directorio de trabajo actual (CWD).
- El modulo se ejecuta en el scope global: sus funciones y variables quedan disponibles.
- La importacion circular se detecta y lanza un error.
- Importar el mismo fichero dos veces re-ejecuta sus definiciones (idempotente en la practica para funciones).

**Convencion recomendada**: poner la libreria en el mismo directorio que el script principal y usar rutas relativas a donde se lanza el binario `vm`.

---

## Funciones integradas

### Entrada / salida

| Funcion                | Descripcion                                   |
|------------------------|-----------------------------------------------|
| `echo(v1, v2, ...)`    | Imprime valores separados por espacio + newline |
| `print(v1, v2, ...)`   | Imprime sin newline final                     |
| `println(v1, v2, ...)`  | Imprime con newline final                    |
| `input([prompt])`      | Lee una linea de stdin                        |

### Conversion de tipos

| Funcion       | Descripcion                                   |
|---------------|-----------------------------------------------|
| `str(v)`      | Convierte a string                            |
| `int(v)`      | Convierte a int64 (trunca float)              |
| `float(v)`    | Convierte a float64                           |
| `bool(v)`     | Convierte a bool (truthy)                     |
| `type(v)`     | Devuelve el tipo como string                  |

### Strings

| Funcion                       | Descripcion                                |
|-------------------------------|--------------------------------------------|
| `len(s)`                      | Longitud en bytes                          |
| `upper(s)`                    | Todo en mayusculas                         |
| `lower(s)`                    | Todo en minusculas                         |
| `trim(s)`                     | Eliminar espacios al inicio y al final     |
| `split(s, sep)`               | Dividir por separador → lista              |
| `join(lista, sep)`            | Unir lista de strings con separador        |
| `starts_with(s, pref)`        | True si s empieza con pref                 |
| `ends_with(s, suf)`           | True si s termina con suf                  |
| `replace(s, old, new)`        | Sustituir todas las ocurrencias de old     |
| `substr(s, inicio, longitud)` | Subcadena desde inicio con longitud bytes  |
| `contains(s, sub)`            | True si sub esta en s                      |

### Listas

| Funcion              | Descripcion                                     |
|----------------------|-------------------------------------------------|
| `len(lista)`         | Numero de elementos                             |
| `append(lista, v)`   | Anadir v al final (in-place)                    |
| `pop(lista)`         | Eliminar y devolver el ultimo elemento          |
| `contains(lista, v)` | True si v esta en la lista (por valor)          |
| `range(n)`           | Lista `[0, 1, ..., n-1]`                        |
| `range(s, e)`        | Lista `[s, s+1, ..., e-1]`                      |
| `range(s, e, step)`  | Lista con incremento step (puede ser negativo)  |

### Mapas

| Funcion             | Descripcion                               |
|---------------------|-------------------------------------------|
| `keys(m)`           | Lista de claves del mapa                  |
| `values(m)`         | Lista de valores del mapa                 |
| `contains(m, clave)`| True si la clave existe en el mapa        |

### Predicados de tipo

```vsh
is_null(v)      is_bool(v)      is_int(v)       is_float(v)
is_str(v)       is_list(v)      is_map(v)        is_fn(v)
is_class(v)     is_instance(v)
```

`type(v)` devuelve el tipo como string: `"null"`, `"bool"`, `"int"`, `"float"`,
`"string"`, `"list"`, `"map"`, `"function"`, `"class"`, `"instance"`.

### OOP

| Funcion                       | Descripcion                                          |
|-------------------------------|------------------------------------------------------|
| `isinstance(obj, clase)`      | True si obj es instancia de clase o subclase         |
| `classname(obj)`              | Nombre de la clase de obj (o de la clase misma)      |
| `is_class(v)`                 | True si v es un descriptor de clase                  |
| `is_instance(v)`              | True si v es una instancia                           |

### Matematicas

| Funcion             | Descripcion                                  |
|---------------------|----------------------------------------------|
| `abs(x)`            | Valor absoluto                               |
| `min(a, b)`         | Minimo de dos valores                        |
| `max(a, b)`         | Maximo de dos valores                        |
| `floor(x)`          | Redondear hacia abajo                        |
| `ceil(x)`           | Redondear hacia arriba                       |
| `round(x)`          | Redondear al entero mas cercano              |
| `sqrt(x)`           | Raiz cuadrada                                |
| `pow(base, exp)`    | Potencia (equivalente a `base ** exp`)       |
| `log(x)`            | Logaritmo natural (base e)                   |

### Sistema de ficheros

| Funcion                    | Descripcion                                   |
|----------------------------|-----------------------------------------------|
| `exists(ruta)`             | True si la ruta existe                        |
| `is_file(ruta)`            | True si es un fichero regular                 |
| `is_dir(ruta)`             | True si es un directorio                      |
| `basename(ruta)`           | Nombre del fichero con extension              |
| `dirname(ruta)`            | Directorio padre                              |
| `stem(ruta)`               | Nombre sin extension                          |
| `extension(ruta)`          | Extension (incluye el punto)                  |
| `glob(patron)`             | Lista de rutas que coinciden con el patron    |
| `read_file(ruta)`          | Leer fichero a string; lanza si no existe     |
| `write_file(ruta, texto)`  | Escribir string en fichero (sobreescribe)     |

### Utilidades

| Funcion              | Descripcion                                   |
|----------------------|-----------------------------------------------|
| `shell(cmd)`         | Ejecutar comando de shell; devuelve la salida |
| `sleep(ms)`          | Pausar N milisegundos                         |
| `exit([codigo])`     | Salir del proceso                             |
| `assert(cond, [msg])`| Lanzar error si cond es falso                 |
| `error(msg)`              | Lanzar un VshRuntimeError con msg                      |
| `doc(fn_o_clase)`         | Devuelve el docstring de `fn` o clase (o `""`)         |
| `help(fn_o_clase)`        | Imprime firma + docstring; devuelve el texto como string |

---

## Sockets y red

VestaShell incluye soporte multiplataforma para TCP, UDP y TLS, ademas de
funciones de alto nivel para HTTP/HTTPS. Todas las funciones de socket devuelven
un **handle** (entero) que identifica la conexion. Siempre cierra el socket con
`socket_close(h)` cuando ya no lo necesites.

### TCP

```vsh
// Conectar a un servidor TCP
let h = tcp_connect("httpbin.org", 80)
let sent = socket_send(h, "GET / HTTP/1.0\r\nHost: httpbin.org\r\n\r\n")
let resp = socket_recv_all(h)    // leer hasta EOF
socket_close(h)

// Recibir en chunks (loop manual)
let chunk = socket_recv(h, 4096)
while len(chunk) > 0 {
    procesar(chunk)
    chunk = socket_recv(h, 4096)
}
```

```vsh
// Crear servidor TCP (acepta una conexion sincrona)
let srv = tcp_listen(8080)          // escuchar en puerto 8080
let srv2 = tcp_listen(0)            // puerto dinamico asignado por el SO
let client = tcp_accept(srv)        // bloquea hasta conexion entrante
let data = socket_recv_all(client)
socket_send(client, "HTTP/1.0 200 OK\r\n\r\nHola!")
socket_close(client)
socket_close(srv)
```

| Funcion | Descripcion |
|---------|-------------|
| `tcp_connect(host, port)` | Conectar TCP; devuelve handle |
| `tcp_listen(port [, backlog])` | Crear socket servidor; `port=0` → SO asigna puerto libre |
| `tcp_accept(handle)` | Aceptar conexion entrante (bloqueante); devuelve handle cliente |
| `socket_send(h, datos)` | Enviar string; devuelve bytes enviados |
| `socket_recv(h [, maxlen])` | Recibir hasta `maxlen` bytes (defecto 4096); `""` = EOF |
| `socket_recv_all(h)` | Leer hasta EOF y devolver todo como string |
| `socket_close(h)` | Cerrar socket y liberar recursos |

### UDP

```vsh
// Enviar datagram UDP
let u = udp_socket()
let n = udp_sendto(u, "payload", "127.0.0.1", 9)   // port 9 = discard
socket_close(u)

// Receptor UDP (bind + recvfrom)
let u = udp_socket()
udp_bind(u, 5000)                           // escuchar en puerto 5000
let r = udp_recvfrom(u, 1024)              // [datos, ip_remota, puerto_remoto]
println("de " + r[1] + ":" + str(r[2]))
println("datos: " + r[0])
socket_close(u)
```

| Funcion | Descripcion |
|---------|-------------|
| `udp_socket()` | Crear socket UDP; devuelve handle |
| `udp_bind(h, port)` | Enlazar a puerto local |
| `udp_sendto(h, datos, host, port)` | Enviar datagram; devuelve bytes enviados |
| `udp_recvfrom(h [, maxlen])` | Recibir datagram; devuelve `[datos, ip, puerto]` |

### TLS

```vsh
// Conexion TLS manual (equivale a HTTPS raw)
let h = tls_connect("example.com", 443)
socket_send(h, "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n")
let resp = socket_recv_all(h)
socket_close(h)
```

| Funcion | Descripcion |
|---------|-------------|
| `tls_connect(host, port)` | Conexion TCP + TLS handshake; devuelve handle cifrado |

Los handles TLS son compatibles con `socket_send`, `socket_recv`,
`socket_recv_all` y `socket_close`.

### HTTP / HTTPS de alto nivel

Estas funciones gestionan la conexion, peticion y cierre automaticamente y
devuelven directamente el **cuerpo** de la respuesta como string.

```vsh
// GET
let html  = http_get("http://example.com/api/users")
let json  = https_get("https://httpbin.org/get")

// POST
let resp  = http_post("http://api.example.com/items", "nombre=VestaShell&v=1")
let resp2 = https_post("https://httpbin.org/post", '{"x":1}', "application/json")

// PUT
let resp3 = http_put("http://api.example.com/items/1", "nombre=Nuevo")
let resp4 = https_put("https://httpbin.org/put", '{"y":2}', "application/json")

// DELETE
let resp5 = http_delete("http://api.example.com/items/1")
let resp6 = https_delete("https://httpbin.org/delete")
```

#### `http_request` — API generica

```vsh
let r = http_request("PATCH", "https://httpbin.org/patch",
                     '{"campo":"valor"}', "application/json")
// r es un mapa con tres claves:
println(str(r["status"]))   // ej: 200
println(r["headers"])       // cabeceras HTTP de la respuesta
println(r["body"])          // cuerpo de la respuesta
```

| Funcion | Firma | Descripcion |
|---------|-------|-------------|
| `http_get(url)` | `(url) -> str` | HTTP GET; devuelve cuerpo |
| `https_get(url)` | `(url) -> str` | HTTPS GET; devuelve cuerpo |
| `http_post(url, body [, ct])` | `-> str` | HTTP POST |
| `https_post(url, body [, ct])` | `-> str` | HTTPS POST |
| `http_put(url, body [, ct])` | `-> str` | HTTP PUT |
| `https_put(url, body [, ct])` | `-> str` | HTTPS PUT |
| `http_delete(url)` | `-> str` | HTTP DELETE |
| `https_delete(url)` | `-> str` | HTTPS DELETE |
| `http_request(method, url [, body [, ct]])` | `-> map` | Peticion generica; devuelve `{status, headers, body}` |

El parametro `ct` es el `Content-Type` (defecto: `application/x-www-form-urlencoded`
para las funciones especializadas; `application/json` para `http_request`).

### Ejemplo completo: scraping HTTPS

```vsh
// Obtener IP publica via HTTPS
let r = https_get("https://httpbin.org/ip")
if contains(r, "origin") {
    println("IP publica detectada en la respuesta")
}

// Peticion con control total de estado
let resp = http_request("POST", "https://httpbin.org/post",
                        "usuario=admin&pass=1234",
                        "application/x-www-form-urlencoded")
if resp["status"] == 200 {
    println("POST exitoso")
}
```

### Ejemplo completo: servidor TCP echo

```vsh
// Servidor echo de una sola conexion en puerto 7777
let srv = tcp_listen(7777)
println("Escuchando en :7777 ...")
let cli = tcp_accept(srv)
let msg = socket_recv(cli, 1024)
socket_send(cli, msg)            // eco
socket_close(cli)
socket_close(srv)
```

---

## REPL interactivo VestaShell

```bash
./vm --interprete
```

Abre un shell interactivo de VestaShell al estilo Python:

```
VestaShell REPL interactivo - escribe 'exit' o 'quit' para salir
>>> let x = 5
>>> x * 3
15
>>> fn suma(a, b) { return a + b }
>>> suma(2, 3)
5
>>> class Punto {
...     fn __init__(self, x, y) { self.x = x; self.y = y }
... }
>>> let p = Punto(1, 2)
>>> p.x
1
>>> exit
```

Comportamiento:
- Las expresiones simples imprimen su valor (si no es `null`) como `repr`.
- Los bloques multilinea se detectan contando `{` / `}`.
- El prompt cambia a `... ` cuando se espera continuacion.
- Los errores de sintaxis y ejecucion se muestran sin terminar el REPL.
- Las definiciones (`fn`, `class`, `let`) persisten durante toda la sesion.
- `doc(fn)` y `help(fn)` son especialmente utiles en el REPL para explorar el API.

---

## Integracion con el REPL

Cuando el script se ejecuta desde dentro del REPL con `script archivo.vsh`,
todos los comandos del REPL son accesibles como si fueran funciones:

```vsh
// Compilar y ejecutar un programa Vesta desde un script .vsh
build("src/main.vel", "-o", "programa.velb")
run("mi-app", "programa.velb", "--schedulers", "4")

// Navegar el sistema de ficheros
cd("src/")
ls("*.vel")
pwd()

// Ver el estado de las VMs
vms()
```

Los argumentos se pasan como strings y se reensamblan en la linea de comando
antes de enviarse al despachador del REPL.

---

## Comentarios y continuacion de linea

```vsh
// Comentario de linea: desde // hasta el fin de linea

/* Comentario de bloque:
   puede abarcar multiples lineas. */

let valor = 1 + 2 + \
            3 + 4    // barra invertida continua la sentencia
```

Los comentarios `//` y `/* */` son compatibles con el preprocesador de Vesta,
lo que permite usar directivas como `#include` en ficheros `.vsh` procesados
por `vpp` antes de interpretarlos.

---

## Convenios de estilo

- Indentacion: 4 espacios (recomendado).
- Nombres de variables y funciones: `snake_case`.
- Constantes globales: `UPPER_SNAKE_CASE`.
- Terminadores de sentencia: NEWLINE o `;` (ambos equivalentes).
- Los bloques `{}` siempre en la misma linea que la palabra clave que los abre.

---

## Ejemplos

Ver la carpeta `examples_codes_vsh/` del repositorio para scripts de ejemplo
completos que cubren toda la sintaxis y los casos de uso tipicos:

| Fichero                              | Contenido                                              |
|--------------------------------------|--------------------------------------------------------|
| `01_tipos_basicos.vsh`               | Tipos, variables, operadores                           |
| `02_strings.vsh`                     | Strings, interpolacion, builtins                       |
| `03_control_flujo.vsh`               | if/elif/else, while, for/in, range                     |
| `04_funciones.vsh`                   | Funciones, closures, orden superior, docstrings        |
| `05_listas_mapas.vsh`                | Listas, mapas, indexado, iteracion                     |
| `06_errores.vsh`                     | try/catch, error(), assert()                           |
| `07_matematicas.vsh`                 | Builtins matematicos                                   |
| `08_archivos.vsh`                    | Sistema de ficheros                                    |
| `09_lib_utils.vsh` + `09_import.vsh` | Libreria y sistema de importacion                      |
| `10_clases.vsh`                      | Clases, herencia, POO, isinstance, classname, help     |
| `11_errores_tipados.vsh`             | Errores personalizados, throw, catch tipado, jerarquia |
| `12_tipado.vsh`                      | Anotaciones de tipo en let y parametros de funcion     |
| `13_sockets.vsh`                     | TCP, UDP, TLS, HTTP/HTTPS GET/POST/PUT/DELETE          |

Ejecutar la suite completa (desde el directorio raiz del proyecto):

```bash
for f in examples_codes_vsh/0[1-9]_*.vsh examples_codes_vsh/1[0-9]_*.vsh; do
    ./build/vm.exe --script "$f"
done
```
