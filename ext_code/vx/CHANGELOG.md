# Cambios

## 0.1.0

Primera version.

Cliente del servidor de lenguaje de Vesta (`vesta_lsp`):

- Diagnosticos, resaltado semantico, hover, ir a la definicion, buscar
  referencias y autocompletado, todo servido por el compilador.
- Nombre de los parametros delante de sus argumentos en las llamadas.
- Localizacion automatica del servidor, de la biblioteca estandar y de la
  maquina virtual, con ajuste explicito para cada uno.

Vistas del compilador: IR (y su diff antes y despues de optimizar), bytecode,
ensamblador del JIT y del AOT, complejidad Big-O, reporte de los tres modos de
ejecucion, compatibilidad con la compilacion anticipada, expansion de macros y
valores resueltos en tiempo de compilacion.  Todas se pueden pedir para otro
sistema y otra arquitectura, no solo para el anfitrion.

Vista correlacionada del fuente, el IR y el ensamblador en tres columnas
alineadas por linea, con salto al codigo desde cualquier fila.

Diagramas del AST, del IR, del bytecode y del codigo maquina, renderizados en
el editor cuando se piden en HTML, o abiertos como texto en Mermaid y Graphviz.

Compilar y ejecutar el fichero activo: la compilacion la hace el compilador
embebido en el servidor, con el texto del editor aunque no este guardado.

Navegacion a los modulos: abrir el fichero del `import` bajo el cursor, listar
los modulos de la biblioteca estandar y ver que rutas se estan usando.

Gramatica, fragmentos y configuracion de edicion del lenguaje, que siguen
funcionando aunque el servidor no este disponible.
