# Vesta para Visual Studio Code

Soporte del lenguaje **Vesta** (`.vx`) apoyado en el servidor de lenguaje que
ya trae el proyecto (`vesta_lsp`).  El servidor lleva el compilador dentro, asi
que lo que ensena el editor no es una aproximacion: es lo que el compilador
piensa de verdad del codigo, incluido el IR, el bytecode y el codigo maquina
que va a generar.

## Que da

**Del servidor de lenguaje, sin configurar nada:**

- Diagnosticos al teclear, con la posicion exacta.
- Resaltado semantico, con los tipos propios del lenguaje: secuencias de
  escape, interpolaciones `${...}` y registros dentro de los bloques `asm`.
- Ir a la definicion, buscar referencias y ver la documentacion al posar el
  cursor.  La navegacion **entra en los modulos importados**, incluida la
  biblioteca estandar.
- Autocompletado, con acceso a miembros tras el punto.
- Nombre de cada parametro delante de su argumento en las llamadas.

**Ensamblador en linea, con NASM de verdad:** el ensamblador de Vesta ES NASM,
asi que dentro de un bloque manda una gramatica NASM completa -- mnemonicos,
prefijos, registros (generales, de segmento, de control, vectoriales y de
mascara, y los de aarch64), operandos de memoria, especificadores de tamano,
directivas de datos, las cinco formas de escribir un numero y el preprocesador.
Se reconocen las tres formas que admite el lenguaje:

```vesta
asm volatile noinfer { ... }                  // dentro de una funcion
asm ( rdi d = dst, reg c = n, ) { ... }       // atando registros a expresiones
@bits(16) @section(".text", "rx")
asm boot { ... }                              // bloque con nombre, en el nivel superior
bytes tabla { db 0x01, 'A', "Hi" }            // datos con las mismas directivas
```

Dentro del ensamblador valen los comentarios de NASM (`;`) y los del lenguaje
(`//`), que es lo que se usa en la biblioteca; y comentar con Ctrl+/ inserta el
de NASM.

**Vistas del compilador** (paleta de ordenes, categoria `Vesta`):

| Orden | Que ensena |
| :---- | :--------- |
| Ver el IR SSA | El IR del modulo, antes o despues de optimizar |
| Ver el diff del IR | Que le hizo exactamente el optimizador a una funcion |
| Ver el bytecode | El `.vel` del modulo o de una funcion |
| Ver el ensamblador del JIT / AOT | El desensamblado de una funcion, con su marco de pila y sus reubicaciones |
| Ver la correlacion fuente / IR / ensamblador | Tres columnas alineadas por linea; al posar el cursor en una fila se marcan las de las otras dos |
| Ver un diagrama | AST, IR, bytecode o codigo maquina, en pagina interactiva, Mermaid o Graphviz |
| Ver la complejidad Big-O | Coste por funcion, y si cuadra con lo declarado en `@complexity` |
| Ver el reporte de los tres modos | Que hace el interprete, el JIT y la compilacion anticipada con este modulo |
| Ver la compatibilidad AOT | Que impide compilar de forma anticipada, funcion por funcion |
| Ver la expansion de las macros | El codigo Vesta que generan los `@Macro` |
| Ver los valores comptime | Las constantes que el compilador resolvio al compilar |

**Compilar y ejecutar:**

- *Compilar el fichero activo*: usa el compilador embebido en el servidor, con
  el texto del editor aunque no este guardado.  Genera bytecode o un ejecutable
  nativo.
- *Compilar y ejecutar*: compila y lanza el programa en un terminal.

**Navegacion a los modulos:**

- *Abrir el modulo importado bajo el cursor* (tambien en el menu contextual):
  salta al fichero de un `import`, que es donde ir-a-la-definicion no llega
  porque ahi no hay ningun simbolo, sino una ruta.
- *Abrir un modulo de la biblioteca estandar*: lista los modulos por su nombre
  de importacion.
- *Mostrar las rutas en uso*: que servidor, que biblioteca y que maquina
  virtual se estan usando, y de donde salieron.  Es lo primero que hay que
  mirar cuando hay dos instalaciones en la misma maquina.

## Que hace falta

El ejecutable `vesta_lsp`.  La extension lo busca sola, en este orden:

1. El ajuste `vesta.server.path`.
2. La variable de entorno `VESTA_LSP`.
3. El `PATH`.
4. Las rutas de instalacion: `%ProgramFiles%\VestaVM\bin`, `VESTA_HOME`,
   `/usr/local/vesta` y las demas ubicaciones habituales de cada sistema.
5. Los directorios de compilacion del repositorio (`cmake-build-release/`,
   `build/`), subiendo desde la carpeta abierta.

Si no aparece, la extension no se rompe: quedan el resaltado por gramatica, los
fragmentos y la configuracion de edicion, y avisa una vez con un acceso directo
al ajuste.

La biblioteca estandar se localiza igual (ajuste `vesta.stdlibPath`, variable
`VX_STDLIB_DIR`, la carpeta del servidor, el arbol del repositorio) y se le pasa
al servidor, de modo que el editor y la compilacion no pueden discrepar sobre
cual se esta usando.

## Ajustes

| Ajuste | Que hace |
| :----- | :------- |
| `vesta.server.path` | Ruta del servidor de lenguaje |
| `vesta.server.arguments` | Argumentos extra para el servidor |
| `vesta.server.enable` | Arrancar o no el servidor |
| `vesta.trace.server` | Traza del protocolo, para depurar |
| `vesta.stdlibPath` | Ruta de `stdlib/vx` |
| `vesta.vmPath` | Ruta de la maquina virtual con la que se ejecuta |
| `vesta.reuseTerminal` | Reutilizar el terminal entre ejecuciones |
| `vesta.inlayHints.parameterNames` | Nombre de los parametros en las llamadas |
| `vesta.inspect.os` / `vesta.inspect.arch` | Objetivo de las vistas del compilador; vacio = el anfitrion |
| `vesta.aot.tier` | Nivel de runtime que asumen las vistas AOT |
| `vesta.diagram.format` | Pagina interactiva, Mermaid o Graphviz |
| `vesta.diagram.cost` | Anotar los diagramas con el coste de cada funcion |

Las vistas del compilador se pueden pedir **para otra plataforma**: poniendo
`vesta.inspect.os` y `vesta.inspect.arch` se ve el IR, el bytecode y el
ensamblador que el compilador generaria para ese objetivo, no para el anfitrion.

## Desarrollo

```bash
npm install
npm run compile        # o npm run watch mientras se itera
```

Para probarla, abrir esta carpeta en el editor y lanzar la depuracion de
extensiones (F5).

### Comprobaciones

```bash
npm test                 # compila, pasa el linter y comprueba las gramaticas
npm run test:grammar     # solo las gramaticas
npm run test:lsp         # solo el servidor (necesita Python y el binario)
```

**Gramaticas** (`test/grammar.test.js`): carga las dos con el mismo motor de
expresiones que usa el editor -- mas estricto que el de JavaScript -- y
comprueba que cada construccion del lenguaje cae en el ambito que le toca, con
casos tomados del corpus.  Ademas tokeniza los cerca de 500 ejemplos del
repositorio y exige que ninguno deje una construccion abierta al final, que es
como se cuela un resaltado que se apaga a mitad de fichero.  Una gramatica rota
no da error: se apaga en silencio.

**Servidor** (`test/smoke_lsp.py`): habla con el servidor **real** y comprueba
lo que la extension da por supuesto: los nombres de los campos de cada
respuesta, que la correlacion trae la linea fuente y la identidad de la
operacion del IR, que el diagrama en HTML es autocontenido y que la navegacion
a un modulo importado devuelve una direccion que el editor puede abrir.  Si el
servidor cambia un nombre, el editor no fallaria: dejaria de ensenar algo sin
decirlo, que es peor.  Por eso las suposiciones estan escritas.

```bash
python test/smoke_lsp.py --lsp RUTA --file EJEMPLO.vx
```

## Extensiones hermanas

En [`ext_code/`](..) viven las otras dos del ecosistema: `vel` (ensamblador de
la maquina virtual, `.vel`) y `vsh` (lenguaje de guion, `.vsh`).  Son
independientes: esta solo se ocupa de Vesta.
