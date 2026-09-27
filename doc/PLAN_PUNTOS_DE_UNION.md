# Puntos de union y tejido: el mecanismo de ganchos

Acordado con el usuario el 2026-09-27.  Sustituye a cuatro mecanismos que hacen
variantes de lo mismo, cada uno por su lado, y cierra un fallo que los cuatro
comparten: el tejido se confunde con el programa.

---

## 0. Estado de las decisiones

| | decidido | donde |
| :-- | :------- | :---- |
| los ganchos cambian los contratos | **NO**.  *"los hooks no deberian cambiar los contratos"* | 1, 7.2 |
| representacion | **PROPIA, en el IR**: no una llamada normal, no una marca en la llamada, no bajar dos veces | 1.3 |
| alcance del diseno | un MECANISMO (puntos + advices + enlace), no un parche para `@Hook`.  Mi primera propuesta -- `hook.enter/exit` y expandir a `CALL` -- se rechazo por *"quedarse muy corta ... no es expandible para el futuro"* | 1.4 |
| los puntos de union se marcan | **SIEMPRE**: *"es lo que permite el diseno mas optimo, y que la cache se reutilice de forma eficiente sin que se anule"* | 4.5, 8 |
| efectos de un advice que cambia el programa | los **DECLARA el advice**: *"para que el diseno sea mas reutilizable y la informacion mas granular"* | 5.5, 7.2 |
| el vocabulario de efectos | **UNO para todo, sin casos especiales**: *"todos esos efectos deben poder ponerse tanto a externs, como a funciones de nuestro lenguaje como a los advice"* | 5.6 |
| primera entrega | el mecanismo completo -- tabla de clases de punto, advices, enlace, estrategias `static` y `counter` -- migrando `@Hook` y el tejido estatico de `@Aspect`.  `patchable` y `dynamic` despues, **sin cambiar la forma** | 13 |
| que existe ya y que se reutiliza | **investigado** el 2026-09-27: el tejido del AOP en devirt es la expansion de `call`; la tabla de `@Hook` es la semilla de las clases de punto; enlace entre fusionar y los contratos; expansion antes de emitir y de guardar el `.vxir` | 15 |
| bugs vistos al investigar | nueve (tres comprobados a mano); **se cierran antes** del mecanismo | 15.4 |
| que hay que unificar ANTES | seis puntos; bloqueante el 1 (los dos caminos de compilacion comparten la etapa despues de fusionar); estructurales el 2 (tabla unica de propiedades de `IrOp`) y el 3 (vocabulario y comprobadores de efectos) | 16 |

---

## 1. De donde sale

### 1.1 El fallo

`544_hook_nombres.vx` declara `@Hook(enter)` y `@Hook(exit)` sin selector: se
tejen en TODAS las funciones, tambien en las de la biblioteca.  En `-m aot`
el compilador comprueba los contratos de `std.atomic` -- `@alloc(0)`,
`@stack(0)` -- sobre el codigo ya tejido, y el gancho imprime: 67 errores
`VXT004` y ningun ejecutable.  El programa es correcto.

Salio al registrar en la suite los ejemplos 543 y 544, que no tenian caso.
Ya fallaba antes (comprobado con un binario anterior).

### 1.2 Por que falla: el tejido es indistinguible del programa

`Lowering::emit_hook_calls` (`src/vx/lowering/module.cpp`) teje una `CALL`
normal al gancho mas la construccion de sus argumentos (el `fn_name` como
`string`: por eso `compare_swap` sale con "1 reserva PROPIA").  Desde ese
momento, contratos, efectos, el ASA y el optimizador ven codigo del programa:
no hay nada que diga que eso lo puso un gancho.

Y tiene un segundo coste, menos visible: el IR de un modulo **depende de los
ganchos del raiz**, porque se teje al bajar.  Por eso existe `hooks_fp` en la
clave de cache de cada modulo -- sin ella, la stdlib guardada con el gancho
dentro rompia el siguiente programa --, y por eso cambiar un gancho invalida
la cache de todo lo que instrumenta.

### 1.3 Lo que decidio el usuario

Que los ganchos tengan **representacion propia en el IR**.  No una marca en
la `CALL` (seguiria siendo una llamada para todo el que no mire la marca), ni
comprobar los contratos sobre un segundo bajado sin ganchos (doble coste y dos
IR que pueden divergir).

### 1.4 Por que un mecanismo y no un parche

Hoy hay **cuatro** piezas que tejen o sustituyen comportamiento:

| pieza | que hace | limite |
| :---- | :------- | :----- |
| `@Hook(punto[, selector])` | llamadas de OBSERVACION en entrada, salida y aterrizaje de excepcion; tejido ESTATICO; los tres modos | solo observa; solo esos tres puntos; teje al bajar |
| `@Aspect` + `@Before`/`@After`/`@AfterReturning`/`@Around` | cambia el comportamiento de un metodo; semantica de la cadena fijada (ver 10.2) | solo DINAMICO (`advice_chain` en ejecucion); no llega a nativo; solo metodos `Clase.metodo`; el tejido estatico esta pendiente |
| `--instrument trace/profile` | traza o perfila cada llamada | atado a la VM (`getproc`); `fprintf` por llamada |
| `@Provides(builtin)` y los proveedores (`@AllocatorOverride`, `@PanicHandler`) | sustituyen un SERVICIO entero | el grano es el servicio, no un punto del programa |

Las tres primeras son la misma idea -- "en este punto del programa, ademas o en
lugar de lo que hay, haz esto" -- con tres implementaciones que no se hablan.
La cuarta es su caso limite (sustituir siempre, en todo sitio).  Un mecanismo
de ganchos real tiene que poder expresarlas todas y las que vengan.

---

## 2. Que tiene que poder hacer un mecanismo de ganchos real

Sacado de lo que ya piden las cuatro piezas y de lo que el lenguaje persigue
(sin runtime obligatorio, tres modos identicos, todo se puede mirar):

1. **Puntos de muchas clases**, no tres: entrada, salida y aterrizaje de una
   funcion; antes, despues y alrededor de una llamada; reserva y liberacion;
   lanzar y capturar; leer y escribir un campo; la vuelta de un bucle; tomar y
   soltar un cerrojo; `spawn`, `yield`, `resume`, `await`; una sentencia.
   Anadir una clase tiene que ser anadir una FILA, no tocar el mecanismo.
2. **Acciones de varias clases**: observar (no puede cambiar nada), envolver
   con `proceed`, sustituir, recibir el resultado.  Es la semantica del AOP
   generalizada.
3. **Seleccion expresiva**: por nombre y namespace, por modulo, por
   anotacion, por tipo del receptor, por firma, por clase de punto; con `y`,
   `o`, `no`.
4. **Se paga solo lo que se pide**, en tiempo y en codigo: un punto sin
   accion no emite nada, y el contexto que nadie pide no se calcula.
5. **Varias implementaciones del mismo gancho**: llamada directa, cuerpo
   inlinado, contador en linea, hueco parcheable, despacho en ejecucion.  El
   programa no cambia; cambia como se ejecuta el gancho.
6. **Los tres modos dan lo mismo**, como todo en el lenguaje.
7. **El analisis no se enganya**: observar no cambia los contratos; envolver o
   sustituir si cambia el programa, y lo que cambia lo DECLARA el gancho.
8. **La cache no se anula por instrumentar**.
9. **Cruza modulos**: un gancho de una biblioteca se activa con importarla.
10. **Se puede mirar**: que gancho se enlazo a que punto, y por que no se
    enlazo uno que se esperaba.

---

## 3. Tres conceptos separados

Hoy van pegados en una sola `CALL`.  Separados, cada uno tiene un dueno:

| concepto | responde a | vive en | lo produce |
| :------- | :--------- | :------ | :--------- |
| **punto de union** | DONDE se puede actuar | el IR de cada modulo | el bajado, SIEMPRE |
| **advice** | QUE se hace ahi | una tabla del modulo que lo declara (y su `.vxi`) | el comprobador, al leer las anotaciones |
| **enlace** | QUIEN actua en cada punto, y COMO se implementa | el IR fusionado | un pase, al fusionar |

---

## 4. Puntos de union: el DONDE

### 4.1 Las clases de punto, en UNA tabla

Como los builtins o las banderas de entorno: una tabla del compilador, y
cada fila dice:

- el **nombre** (`fn.enter`, `call`, `alloc`...), que es lo que se escribe en
  el fuente;
- la **forma**: PUNTUAL (un instante) o ENVOLVENTE (rodea una operacion);
- el **ancla**: a que entidad del IR se sujeta (ver 4.2);
- el **contexto** que ofrece, campo a campo, con su tipo;
- que **acciones** admite (una reserva se puede envolver; la entrada de una
  funcion no);
- en que **modos** existe (algunos, como `field.read`, pueden no existir
  nunca en una VM si su coste no se puede pagar).

Inicial (lo que ya hay): `fn.enter`, `fn.exit`, `fn.unwind`, y `call` (lo que
necesita el tejido estatico del AOP).  Despues, cada una una fila: `alloc`,
`free`, `throw`, `catch`, `field.read`, `field.write`, `loop.backedge`,
`lock`, `unlock`, `spawn`, `yield`, `resume`, `await`, `stmt`.

La tabla de campos de `@Hook` que ya existe (`fn_id`, `fn_name`, `call_site`,
`depth`, `ret_value`, validada contra el punto) es la semilla del contexto.

### 4.2 El ancla: el punto se sujeta a lo que ya esta en el IR

La leccion de `borrow_facts` -- una tabla al lado con ids que nadie mantiene
se queda rancia -- y la de la coordenada -- *"o se mantiene el hecho, o se
rechaza la transformacion"* -- deciden esto: **un punto de union es una
instruccion del IR**, con dos formas:

- **`jp.at`, puntual**: una instruccion propia en el sitio (inicio del bloque
  de entrada, justo antes de cada `ret`, tras el enlace de la excepcion en un
  `catch`).
- **`jp.wrap`, envolvente**: una instruccion que CONTIENE la operacion que
  rodea (una llamada, una reserva) y da su resultado.  Sin enlazar, es esa
  operacion y nada mas.

El optimizador las trata como a cualquier otra: las mueve, las duplica o las
borra con la operacion a la que van unidas.  Al inlinar, las del llamado
viajan con el (decision abierta, 14).

### 4.3 El contexto no alarga la vida de nada

Los operandos de un punto son SOLO valores que la operacion anclada ya usa, o
parametros de la funcion: el valor que devuelve el `ret` siguiente, los
argumentos de la llamada envuelta, el tamano de la reserva.  **Un punto de
union nunca mantiene vivo un valor que sin el estaria muerto.**  Lo que no se
puede sacar de ahi (el nombre de la funcion, su id, la direccion de retorno) no
es un operando: lo construye la expansion (6.3) solo si un advice enlazado lo
pide.

Es lo que permite marcarlos siempre sin que el optimizador haga otra cosa.

### 4.4 La identidad: la del plan del grafo de sentencias

Un punto necesita un nombre estable ENTRE compilaciones: un contador de
cobertura, un perfil de PGO o una sonda del depurador guardados ayer tienen
que encontrar hoy su punto.  Esa identidad ya esta disenada, y se reusa tal
cual (seccion 7-ter de [`PLAN_GRAFO_SENTENCIAS.md`](PLAN_GRAFO_SENTENCIAS.md)):

> **`(funcion, posicion entre las hermanas del AST ESCRITO)`**, con la huella
> de tokens en la carga para re-encontrarla si se movio.

Para un punto: `(funcion, clase, sentencia, ordinal dentro de la sentencia)`.
`fn.enter` es `(funcion, fn.enter)` a secas.  Lo que la bajada fabrica
(`for` desazucarado, `defer`) no numera: cuelga de la sentencia que lo produjo,
igual que en ese plan.

### 4.5 Se marcan SIEMPRE, y cuestan cero si nadie los usa

Decision del usuario.  El bajado emite todos los puntos de las clases que la
tabla declare "siempre marcadas", haya o no un gancho en el programa.  Lo que
eso compra:

- el IR de un modulo **ya no depende de los ganchos de nadie**: se guarda una
  vez y sirve con cualquier juego de ganchos (seccion 8);
- el enlace ocurre al fusionar, con todos los advices a la vista;
- un punto sin advice **se borra en el enlace, antes de optimizar**: el
  optimizador y el emisor no lo ven nunca, asi que el codigo generado es
  IDENTICO al de hoy sin ganchos.  Es la regla del proyecto -- *"el motor
  genera el MISMO codigo con o sin analisis/perfilado"* -- cumplida por
  construccion.

El precio es un IR guardado algo mas grande.  Se MIDE antes de decidir que
clases van "siempre" (una `jp.at` por funcion en entrada y salida es poco;
una por lectura de campo puede no serlo): ver 13.

---

## 5. Advices: el QUE

### 5.1 Las clases de accion

La semantica del AOP, que ya esta fijada y medida (`379_aop_cadena_orden`,
`380_aop_around_anidado`, `381_aop_after_returning`), generalizada:

| clase | puede | recibe | valor |
| :---- | :---- | :----- | :---- |
| `observe` (antes / despues) | nada: solo mirar | el contexto pedido | se descarta |
| `after_returning` | nada | el RESULTADO | se descarta |
| `after` | reemplazar el resultado | los argumentos ORIGINALES | reemplaza el valor |
| `around` | envolver: decide si y como seguir con `proceed` | el contexto + `proceed` | es el valor |
| `replace` | sustituir la operacion | el contexto | es el valor |

`@Hook` de hoy es `observe`.  `@Before`, `@After`, `@AfterReturning` y
`@Around` son las demas.  **Solo `observe` y `after_returning` no pueden cambiar
el programa**, y esa linea es la que decide como los ve el analisis (7.2).

### 5.2 El selector es un predicado, no un texto

Hoy hay dos gramaticas: el glob de `@Hook` (`"std.*"`, probado contra el
nombre aplanado Y contra el reescrito a mano de `__` a `.`) y el pointcut
exacto de `@Aspect` (`"Service.run"`).  Pasan a ser dos FORMAS DE ESCRIBIR el
mismo predicado estructurado:

- sobre la funcion: nombre escrito, namespace, modulo, anotaciones, firma,
  si es metodo y de que tipo;
- sobre el punto: su clase, y lo que su ancla sepa (el llamado de un `call`,
  el tipo de una reserva);
- combinable con `y`, `o`, `no`.

Se evalua contra el nombre ESCRITO, que tiene dueno
(`demangle_symbol` / `written_name`), no contra una conversion a mano.

### 5.3 Orden y composicion

Hoy es el orden de declaracion, con el primer `@Around` como el mas externo
(medido).  Pasa a ser un DATO del advice (un orden explicito, con ese valor
por defecto), porque con advices de varias bibliotecas el orden de declaracion
deja de estar definido.  Dos advices sin orden entre ellos sobre el mismo
punto, cuando el resultado dependa del orden (`around`, `after`, `replace`),
son un error que nombra a los dos -- la misma regla que ya siguen
`@HelperOverride` (VX4016) y `@StringConcat` (VX2118).

### 5.4 El contexto se pide en la firma

Se mantiene lo que ya funciona: el advice declara sus parametros, el
compilador los valida contra el contexto que la clase de punto ofrece, y solo
se construye lo pedido.  Pedir lo que el punto no da es error con la lista de
lo que si hay.

### 5.5 Los efectos de un advice los DECLARA el advice

Decision del usuario.  Un advice lleva su declaracion de efectos con el
vocabulario que ya existe para funciones (`@io`, `@allocator`, `@nothrow`,
`@noblock`, `@pure`...; `ir::IrNativeEffects`), y:

- **es una PROMESA** en el sentido de
  [`PLAN_ASA_AST_IR.md`](PLAN_ASA_AST_IR.md): la firma el autor del advice, y
  su cuerpo es la evidencia con la que el ASA la DESCARGA.  Incumplirla es un
  error del autor del advice, con la prueba y el sitio (seccion 1.1 de ese
  plan);
- **no declarar nada es "opaco"**, como una externa sin efectos declarados:
  la funcion donde se teja un `around` opaco no puede demostrar sus
  contratos, y se DICE que es por el advice -- no se da por incumplido
  (*"no poder demostrar no es demostrar que no"*);
- es **granular**: la contribucion de cada advice es un hecho aparte, con su
  sujeto, y se puede preguntar por ella sin recalcular la funcion.

### 5.6 Un solo vocabulario de efectos, sin casos especiales

Decision del usuario (2026-09-27).  Hoy hay dos vocabularios segun a quien se
le ponga:

| a | se puede declarar hoy | como |
| :-- | :-------------------- | :--- |
| una `extern` | todo: `io`, `alloc`, `allocator`, `frees`, `maps`, `blocks`, `traps`, `throws`, `panics`, `nondet`, `reads_env`, `writes_env`, `keeps_state`, y sus negaciones | `Parser::parse_extern_effects_` -> `ast::ExternEffects` -> `ir::IrNativeEffects` |
| una funcion del lenguaje | solo COTAS: `@pure`, `@nothrow`, `@nopanic`, `@alloc(N)`, `@stack(N)`, con `when:` | otro parseo (`parser.cpp`, `parse_member_contracts_`) -> `FunctionContracts` |
| un advice | nada (no existe) | -- |

Pasa a haber **UNO**, el mismo para los tres, con la misma sintaxis y el mismo
parseo.  Lo unico que cambia entre ellos no es el vocabulario sino **que
significa declararlo**, y eso ya lo da el plan de promesas:

- en una `extern` no hay cuerpo: la declaracion es **indescargable**, se cree
  y se dice que se cree (es su estado final, no uno pendiente);
- en una funcion del lenguaje y en un advice hay cuerpo: la declaracion es una
  **promesa firmada por el autor**, que el ASA DESCARGA contra el cuerpo o
  acusa de incumplida con la prueba (seccion 1.1 de `PLAN_ASA_AST_IR.md`);
- las COTAS de hoy (`@pure`, `@alloc(N)`...) son parte del mismo vocabulario,
  no un segundo sistema: `@pure` es "ningun efecto", `@alloc(0)` es una cota
  sobre el eje `allocates`.

Consecuencia que hay que resolver al implementarlo, no esquivar: hoy los dos
vocabularios llegan a DOS estructuras (`IrNativeEffects` y `FunctionContracts`)
y a tres comprobadores con criterios distintos (15.5).  Unificar el vocabulario
obliga a que llegue a una sola forma.

---

## 6. El enlace: el QUIEN y el COMO

### 6.1 Cuando

**Al fusionar, antes de optimizar -- y antes de comprobar los contratos**
(enmienda tras la investigacion, 15.2: los contratos se comprueban sobre el IR
pre-opt fusionado).  Es el primer momento en que estan todos los modulos y
todos los advices.  El IR de cada modulo que se guarda en cache es ANTERIOR a
esto.

### 6.2 Que hace

Para cada punto de union del IR fusionado:

1. evalua los selectores de los advices de su clase;
2. si ninguno casa, **borra el punto** (un `jp.wrap` se queda en la operacion
   que envolvia);
3. si casan, lo **enlaza**: el punto pasa a llevar la lista ordenada de
   advices, y se comprueba la composicion (5.3) y el contexto (5.4);
4. publica lo que hizo, para que se pueda mirar (seccion 11): que advice
   quedo en que punto, y que advice no alcanzo ningun punto (es el aviso que
   `@Hook` ya da, `VXW933`).

`@NoInstrument` y la deteccion de ciclos (un advice que acaba llamando a algo
instrumentado por el mismo) trabajan sobre ESTE grafo de enlaces, que ahora es
un dato y no un efecto lateral del bajado.

### 6.3 La expansion, y sus estrategias

Un punto enlazado se EXPANDE a codigo ejecutable **despues** de optimizar y de
los informes post-opt, y antes de emitir y de guardar el `.vxir` -- que es lo
que relee el AOT (15.2) --, por UNA pieza comun a los tres modos.  Para `call`
la expansion ya existe: es el tejido del AOP en `devirt_monomorphic_impl`
(15.1), que se reubica aqui en vez de reescribirse.  Cada advice lleva su
estrategia:

| estrategia | para que | se expande a |
| :--------- | :------- | :----------- |
| `static` | `@Hook` de hoy, tejido estatico del AOP | llamada directa, o el cuerpo inlinado si es pequeno |
| `counter` | cobertura, recuento de PGO | un incremento en linea sobre un contador estatico, sin llamada |
| `patchable` (despues) | activar trazas sin recompilar, sondas del depurador | hueco parcheable + trampolin |
| `dynamic` (despues) | el `@Aspect` dinamico de hoy, advices cargados en ejecucion | despacho por tabla en ejecucion |

Aqui van tambien las implementaciones mejores que hoy no caben: el `fn_name`
como vista de solo lectura sobre una constante en vez de construir una cadena
en cada llamada; agrupar los advices de un punto en una sola llamada.

`patchable` y `dynamic` no cambian la forma: son filas nuevas de esta tabla.

---

## 7. Como lo ve cada consumidor

### 7.1 El optimizador

- Un punto sin enlazar no existe para el (se borro en 6.2).
- Un punto enlazado a `observe`/`after_returning` es un efecto OBSERVABLE:
  no se borra, no se reordena con otros efectos, pero no es un efecto de la
  funcion a efectos de sus contratos.
- Uno enlazado a `around`/`after`/`replace` es, para el optimizador, una
  llamada al advice con los efectos que el advice declara.

### 7.2 Contratos, efectos y ASA

- `observe`/`after_returning`: **transparentes**.  Los contratos se juzgan
  como si no estuvieran: es la decision del usuario, y es lo que arregla el
  fallo de 1.1.  El ASA puede publicar aparte lo que cuesta la
  instrumentacion, como hecho con su propio sujeto.
- `around`/`after`/`replace`: el contrato SUMA los efectos declarados del
  advice, que a su vez son una promesa descargada o no (5.5).  El veredicto
  dice que parte viene del advice.

### 7.3 Excepciones

`fn.unwind` ya existe porque una excepcion no sale por el epilogo (medido: 3
entradas, 1 salida).  Se conserva la regla que costo encontrar: el punto va
DESPUES del enlace de la excepcion y de recargar los spills, o pisa `r0`.  Un
`around` sobre una llamada que lanza tiene que dejar pasar la excepcion por su
`proceed` sin tragarsela.

### 7.4 El depurador y los diagramas

- El codigo expandido es **generado**: `OriginKind`/`LoweringKind::Generated`
  del modelo de vxdbg, colgando del punto que lo produjo.  El paso a paso
  puede saltarse el codigo de los ganchos.
- La vista del AST no cambia: un punto de union no es una sentencia.

---

## 8. Cache y modulos

- **La clave de cache de un modulo deja de llevar la huella de los ganchos**
  (`hooks_fp`, `BuildConfig::hooks_fp`, `RootWeaving::hooks_source_fp`): su IR
  ya no depende de ellos.  Cambiar un gancho no invalida la stdlib.
- Lo que SI depende del juego de ganchos es el modulo fusionado y enlazado,
  que es el artefacto final: su cache (la de `full_fingerprint`) lleva la
  huella de los advices enlazados.
- **Los advices viajan en el `.vxi`** de su modulo: importar una biblioteca
  de perfilado la activa (`import std.profiling;`), que es un pendiente
  apuntado desde que existe `@Hook`.  Que un advice de una biblioteca se
  aplique a quien la importa, o solo a si misma, es una decision (14).

---

## 9. Como encaja con el plan AST + IR

Son tres documentos que se apoyan: la COORDENADA (el DONDE del fuente), el
GRAFO DE SENTENCIAS (el QUE del fuente) y las PROMESAS (lo que se afirma y
quien lo comprueba).  Este es el cuarto, y usa los tres.

| de ese plan | aqui | como |
| :---------- | :--- | :--- |
| la coordenada de fuente (fichero por funcion, tramo por instruccion) | cada punto se puede senyalar en el fuente | la hereda de la instruccion a la que se ancla; el codigo expandido lleva la del punto |
| la identidad de una sentencia `(funcion, indice en el AST escrito)` + huella | la identidad de un punto | 4.4: la misma, con clase y ordinal |
| lo generado no numera, cuelga de quien lo produjo | lo tejido es generado | 4.4 y 7.4 |
| una promesa NO es evidencia; se descarga con alcance | los efectos declarados de un advice | 5.5 y 7.2 |
| el ASA es el dueno; vxdbg y los diagramas son consumidores | quien mira el enlace | 11 |
| *"o se mantiene el hecho, o se rechaza la transformacion"* | los puntos al optimizar | 4.2 |

### 9.1 Por que ESTO si va en el IR

El plan del grafo descarta meter la tabla de sentencias en el IR: *"un hecho
no puede volverse operando"*, y una sentencia es un HECHO sobre el programa
-- un registro que no cambia lo que el programa hace --.

Un punto de union no es un hecho: es un sitio donde PUEDE haber
comportamiento.  Si hay un advice, ahi se ejecuta codigo; si el optimizador
mueve la operacion, el punto se tiene que mover con ella.  Es exactamente el
caso del prestamo, que por eso mismo dejo de ser una tabla al lado y paso a
ser `IrOp::BORROW`: *"una instruccion, justo para que el optimizador la
mantenga o la borre como a cualquier otra"*.

Y lo que SI es un hecho -- que advice se enlazo donde, cuanto cuesta, por que
uno no alcanzo nada -- va al ASA como hecho, no al IR.

### 9.2 Lo que este plan le devuelve a aquellos

- Un consumidor para la identidad de sentencias: cobertura y PGO por
  sentencia son advices `counter` sobre puntos `stmt`.
- El primer caso de promesas de EFECTOS firmadas por un tercero (el autor del
  advice) sobre codigo que no es suyo.

---

## 10. Migracion de lo que existe

### 10.1 `@Hook`

- Su notacion se queda (`@Hook(punto[, selector])` sobre la funcion que
  provee): pasa a declarar un advice `observe` con estrategia `static`.
- `emit_hook_calls` y los siete sitios que la llaman (`module.cpp`, `oop.cpp`,
  `statements.cpp`, `exceptions.cpp`) pasan a emitir puntos de union, que se
  emiten siempre.
- Desaparecen: la conversion de `__` a `.` a mano del selector, la recogida de
  ganchos del raiz antes de compilar (`project/root_weaving`, su parte de
  ganchos) y la huella de ganchos en la cache.
- Se conserva: la validacion de firma contra la tabla de campos, `@NoInstrument`,
  `VXW933`, la regla del raiz (una biblioteca que usa un perfilador no
  instrumenta a quien la usa), `RETURN_ADDR` para `call_site`.

### 10.2 `@Aspect`: el tejido estatico

Es el "escalon 1" que ya estaba planeado (memoria `proj_aop_tejido`), ahora
sobre esta forma:

- los advices del aspecto se leen como advices de clase `observe` (`@Before`),
  `after`, `after_returning` y `around`, sobre puntos `call` con selector
  `callee(Clase.metodo)`;
- el `proceed` de un `around` es ESTATICO: cada advice tiene un destino
  conocido (el siguiente `around` o el metodo);
- la semantica medida se conserva al pie de la letra: `@Before` en orden y
  descartado; el primer `@Around` el mas externo; `@After` con los argumentos
  ORIGINALES y reemplazando el valor; `@AfterReturning` con el resultado en el
  primer argumento; el receptor ORIGINAL en todos; al heredar, la cadena se
  resetea;
- donde el receptor no se conoce, la cadena en ejecucion se queda hasta que
  exista la estrategia `dynamic`: no hay doble ejecucion, porque un sitio
  tejido no entra al despacho.

### 10.3 `--instrument trace/profile`

Pasa a ser un advice de biblioteca (`counter` para perfilar, `static` para
trazar) en vez de un modo atado a la VM.  No es de la primera entrega.

### 10.4 Los proveedores (`@Provides`)

No se tocan: sustituyen un servicio en todo sitio, y el grano es el builtin.
Se apunta la relacion (un `replace` con selector "todos los sitios de este
builtin" es lo mismo) por si algun dia conviene unificarlos.

---

## 11. Como se mira

- El enlace publica hechos: para cada punto, que advices; para cada advice,
  cuantos puntos alcanzo y, si ninguno, POR QUE (selector que no casa, punto
  que no existe en este modo, excluido por `@NoInstrument`).
- `--vx-emit-ir` saca los puntos y la tabla de advices en el IR antes y
  despues de enlazar.
- El coste de la instrumentacion es un hecho del ASA con su sujeto.

---

## 12. La forma en el IR

### 12.1 Antes de enlazar (lo que se guarda en cache)

```
@function acumular(%n: i32) -> i32 {
entry_0:
    jp.at   fn.enter                                 ; #0
    %4 = const.i64 0
    %5 = sext.i64 %n
    %6 = cmp.le.bool %5, %4
    br.cond %6, if_then_1, if_merge_2
if_then_1:
    %8 = trunc.i32 %7
    jp.at   fn.exit  %8                              ; #1: el valor del ret
    ret.i32 %8
if_merge_2:
    ...
    %12 = jp.wrap call.i32 @acumular(%15)            ; #2: sin advice, es la llamada
    ...
    jp.at   fn.exit  %17                             ; #3
    ret.i32 %17
}
```

### 12.2 Tabla de advices (del modulo que los declara, y en su `.vxi`)

```
@advice 0 observe on fn.enter  where ns("ejemplos.*") & !annot(NoInstrument)
          -> @al_entrar(fn_name)       strategy static  order 0
          effects(io)
@advice 1 around  on call      where callee(Service.run)
          -> @logged(proceed, args)    strategy static  order 0
          effects(io, allocates)
```

### 12.3 Despues de enlazar (antes de optimizar)

```
    jp.at   fn.enter  [0]                            ; enlazado a advice 0
    ...
    jp.at   fn.exit  %8  []                          ; sin advice: se borro
    ...
```

### 12.4 Despues de expandir (lo que ven los generadores de codigo)

```
    %h = str_view.ptr "acumular"                      ; constante, sin construir
    call @ejemplos__hook_nombres__al_entrar(%h)
```

La sintaxis exacta (nombres de las operaciones, como se escribe el predicado)
se fija al implementar; lo que queda fijado aqui es la FORMA.

---

## 13. Primera entrega y orden de trabajo

1. **Investigar lo que existe** para no duplicar (seccion 15) -- HECHO.
1-bis. **Cerrar los bugs de 15.4** que tocan esto (1, 3, 4, 5, 6, 7), cada
   uno con su caso; el 2 y el 8 son independientes.
2. La **tabla de clases de punto** (inicial: `fn.enter`, `fn.exit`,
   `fn.unwind`, `call`), con su contexto y acciones.
3. **`jp.at` / `jp.wrap` en el IR**, su serializacion y su version (las DOS
   versiones del IR suben), y el bajado emitiendolos siempre.  MEDIR el
   tamano del IR guardado y el tiempo de compilar, con el caso de 144k lineas.
4. La **tabla de advices** en el IR y en el `.vxi`, leida de `@Hook` y de
   `@Aspect`, con sus efectos declarados.
5. El **enlace** al fusionar: selector, orden, contexto, borrado de los no
   enlazados, hechos publicados.
6. La **expansion** con `static` y `counter`, comun a los tres modos.
7. **Contratos y ASA**: transparentes para `observe`; suma de efectos
   declarados para los demas; la descarga de esas promesas.
8. **Migrar `@Hook`** y quitar lo que sobra (10.1); **tejido estatico de
   `@Aspect`** (10.2).
9. Casos: 543/544 en los tres modos (544 en `-m aot` es el que hoy falla), la
   cadena del AOP (379/380/381) tejida, un contrato de la stdlib que se
   mantiene con un gancho encima, y la cache: cambiar un gancho NO recompila
   la stdlib.

Cada paso con su commit y la suite entera.

### 13.1 Lo que pide la regla para un nodo nuevo del IR

- **(a) Como baja a MachineIR**: no baja.  La expansion (6.3) lo convierte en
  instrucciones que ya existen antes de que llegue ningun generador de codigo.
  Sin ganchos, ni siquiera llega a la expansion: se borro en el enlace.
- **(b) Runtime**: `static` y `counter` no necesitan ninguno.  `dynamic`
  necesitara un registro en ejecucion alcanzable desde `libvesta_rt` (hoy
  existe solo en la VM para el AOP); se decide al llegar a esa estrategia.
- El **bytecode** no gana instrucciones: la VM recibe lo expandido.
- Suben de version el formato del IR (sus dos constantes) y el del `.vxi`.

---

## 14. Lo que queda por decidir

1. **Que clases se marcan "siempre"** y cuales solo si algun advice del
   programa las nombra (una lectura de campo marcada siempre puede pesar).
   Se decide con la medida del paso 3.
2. **Inlining**: una funcion inlinada, sigue reportando su entrada y su
   salida?  (Hoy si, porque las llamadas del gancho se copian.)
3. **Alcance de un advice de biblioteca**: se aplica a quien la importa, o
   solo al raiz lo decide?  Hoy la regla es "solo los del raiz".
4. **La sintaxis del selector estructurado** en el fuente.
5. **`fn_name`**: el nombre escrito o el simbolo aplanado (hoy el aplanado,
   documentado como tal en 544).
6. **`depth`**: ya NO esta en la tabla de campos (se quito, `hook_points.h`);
   queda decidir si vuelve como campo derivado.
7. ~~Sintaxis de efectos positivos en una funcion con cuerpo~~ -- DECIDIDO
   (5.6): un solo vocabulario y una sola sintaxis para los tres.  Queda por
   fijar la forma concreta de escribirlo, partiendo de la de `extern`.

---

## 15. Lo que ya existe (investigado el 2026-09-27)

Inventario hecho leyendo el codigo antes de escribir nada, con los puntos que
deciden el diseno comprobados a mano.  Las lineas son de ese dia: se miran,
no se creen.

### 15.1 Lo que se REUTILIZA

| pieza | donde | para que sirve aqui |
| :---- | :---- | :------------------ |
| la tabla de puntos y campos de `@Hook` | `include/vx/hook_points.h` (`HookPoint`, `kHookPoints`, `kHookFields`, `hook_field_for`...) | es la semilla de la tabla de CLASES de punto y de su contexto (4.1) |
| validacion de firma y aviso de alcance | `Lowering::collect_hook_providers`, `warn_unreached_hooks` (`lowering/module.cpp`) | la validacion del contexto pedido (5.4) y el "no alcanzo nada" (6.2) |
| `RETURN_ADDR` | `IrOp::RETURN_ADDR`, emitido para `call_site` | el campo `call_site` en la expansion |
| la ATRIBUCION del AOP | `IrModule::ChainedAdvice` / `advice_chains` / `all_advices_attributed` (`ssa_ir.h`), rellenada en `lowering/module.cpp` | el proto-enlace: objetivo -> cadena ordenada de advices |
| el `proceed` ESTATICO | `emit_proceed` (`lowering/instructions.cpp`) + `proceed_target_` | el `proceed` de un `around` enlazado (10.2) |
| el TEJIDO del AOP en el sitio de llamada | `devirt_monomorphic_impl` (`src/ir/ir_optimizer.cpp`, ~12078-12172) | es ya "expandir befores + around + afters + after_returning en una llamada": la expansion de 6.3 para `call`, con la semantica medida ya escrita |
| los ejemplos 16, 23, 379, 380, 381 | `examples_codes_vx/` (99, 1007, 42, 147, 742) | la especificacion ejecutable de la cadena |
| el vocabulario de efectos de una externa | `ir::IrNativeEffects` (`ssa_ir.h`), `ast::ExternEffects` (con `any`), `Parser::parse_extern_effects_` (`parser.cpp`) | la declaracion de efectos de un advice (5.5): mismo vocabulario, mismo parseo |
| aplicar efectos DECLARADOS en el analisis | huella: `compose_fingerprints` (`src/analyze/fingerprint.cpp`, rama de nativas declaradas); motor: `aplicar_decl` (`src/analysis/effects/ir_effects.cpp`) | sumar los efectos declarados de un `around`/`replace` (7.2), igual que una nativa declarada |
| la promesa con autoria | `IrParamContract` (`holds`/`proven`/`declared`) y su productor `param_contracts_producer.cpp` (`Source::Declared`, reglas `param.*`) | el modelo de "promesa del autor" para los efectos de un advice |
| publicar hechos | `register_producer` + `Production::assert_fact`/`say_unknown` (`include/analysis/asa/producers.h`); plantilla `src/analyze/asa_fingerprint.cpp` | los hechos del enlace (11) y el coste de la instrumentacion (7.2) |
| nombre escrito | `vx::demangle_symbol` (`module/namespace_flatten.cpp`), `TypeChecker::written_name` | evaluar el selector contra el nombre ESCRITO (5.2) |
| marca de "generado" | `vxdbg::LoweringKind::Generated`, `OriginKind` (`include/vxdbg/lowering_map.h`) | lo tejido (7.4) -- ver 15.3: le falta un valor y no tiene productor |
| el patron de tabla por funcion | `IrFunction::inline_sites` + `IrInstr::inline_site`, remapeado al inlinar y al fusionar | si el enlace necesita tabla por funcion |
| el patron de tabla por modulo | `IrModule::source_files` + `assign_source_file`, remapeada al fusionar | la tabla de advices (12.2) |

### 15.2 Donde hay que ENGANCHARSE (lo que decide el diseno)

**El orden real de la tuberia** (proyecto; el fichero suelto sigue el mismo):

```
bajar (HOY SE TEJE AQUI) -> fusionar -> run_pre_opt_checks (CONTRATOS, IR pre-opt)
  -> ASA pre-opt -> prestamos -> ir_verify -> ir_optimize (HOY SE TEJE EL AOP AQUI, en devirt)
  -> informes y ASA post-opt -> emitir -> emit_ir_module_cache
```

Consecuencias, que ENMIENDAN la seccion 6:

- **El enlace va entre fusionar y `run_pre_opt_checks`** (proyecto: tras las
  deduplicaciones de la fusion; suelto: antes del bloque de contratos).  Los
  contratos se comprueban sobre el IR PRE-opt fusionado, asi que los puntos
  ENLAZADOS estan presentes cuando se comprueban: la transparencia de 7.2 la
  tiene que implementar la huella, no se consigue "expandiendo despues".
- **La expansion va despues de los informes post-opt y ANTES de
  `ir_emit_module` y de `emit_ir_module_cache`.**  El AOT no fusiona ni
  optimiza por su cuenta: relee el `.vxir` que deja la compilacion
  (`aot_build.cpp`), asi que tiene que recibirlo ya expandido.  Los runtimes
  que el AOT funde despues (`vx_exc`, `vx_io`...) pasan por su propia
  compilacion y por su propio enlace.
- **Toda tabla nueva tiene que viajar por el `.vxir` Y fusionarse**: ver el
  hueco de 15.4.

**Los tres sitios del analisis** donde un punto `observe` enlazado tiene que
ser transparente:

1. huella de contratos: `compute_fingerprint` (`fingerprint.cpp`, donde las
   `CALL` van a `fp.calls`) -- y `is_pure_op`, que hoy no lista `RETURN_ADDR`;
2. motor semantico: `callees_of` (`effect_analysis.cpp`) y `effects_of_instr`
   (`ir_effects.cpp`), donde `RETURN_ADDR` cae en `UnmodeledOp` -> `top()`;
3. el linter, que relee los hechos de la huella (`linter.cpp`).

Con una operacion PROPIA esto es un `case` en cada uno, no reconocer una
`CALL` especial: es la ventaja de la representacion propia.

**Anadir la operacion** (patron `IrOp::BORROW`): `ssa_ir.h` (enum; ojo, un
valor repetido compila en silencio), `OP_TABLE` en `ssa_ir.cpp` (nombre,
impresion, parseo), `ir_verify`, el optimizador (`is_side_effecting`,
`is_pure`, `is_sched_barrier`, DCE), la tabla de efectos por opcode
(`ir_effects.cpp`), los dominios con caso por op (`value_range`,
`loop_structure`, `escape`, `points_to`...), y los emisores (bytecode, JIT
x86/ARM64, AOT, port C) -- que no deberian verla nunca porque llega expandida,
y lo tienen que DECIR si la ven.  La serializacion por instruccion es
generica.  **Hay espacio**: 193 operaciones, la mayor `0xEA`, 63 huecos bajo
`0x100`; pero `OpNameTable` se indexa con un BYTE y `IrOp` es de 16 bits.

**Las dos versiones del formato**: `IR_SECTION_VERSION = 19` (`@ir` del
`.velb`) e `IR_MODULE_CACHE_VERSION = 20` (`.vxir`), en
`ssa_ir_serialize.h`.  Comparten `serialize_function`: cambiar el cuerpo de una
funcion sube las dos.  Una tabla de modulo solo sube la del `.vxir`.  Y el
`.vxi` (`VXI_FORMAT_VERSION = 22`) si los advices viajan en el (8).

### 15.3 Lo que NO existe y hay que hacer

- **Un verificador de promesas de efectos contra un cuerpo.**  Hoy lo
  declarado se CREE (las externas no tienen cuerpo), y `Source::Declared` no
  lo consume nadie fuera de su productor.  Los efectos de un advice (5.5)
  serian la primera promesa de efectos con verificador: encaja con los pasos
  3 y 4 del orden de trabajo de `PLAN_ASA_AST_IR.md`.
- **Efectos POSITIVOS sobre una funcion con cuerpo.**  Una funcion Vesta solo
  puede ACOTAR (`@pure`, `@nothrow`, `@alloc(N)`, `@stack(N)`); declarar
  `@io` o `allocates` solo existe para `extern`.  DECIDIDO: un solo
  vocabulario para `extern`, funciones del lenguaje y advices (5.6).
- **Un valor de `OriginKind` para lo tejido**, y un productor de
  `LoweringMap`: hoy nadie lo rellena (es el mismo hueco que el plan del
  grafo, seccion 2).
- **La inversa de `namespace_names`** (`__` -> `.`): existe como
  `demangle_symbol` en `namespace_flatten`, fuera del dueno de la forma de los
  nombres.

### 15.4 Bugs encontrados al investigar (hay que cerrarlos ANTES)

La regla del proyecto es primero los bugs.  Comprobados en el codigo los
marcados con (c):

1. **(c) `CALLM` y `CALLITF` pierden `@Around` y `@AfterReturning`.**  La
   cadena del AOP en ejecucion esta copiada tres veces
   (`exec_instruction_oop.cpp`: `CALLVIRT` completa; `CALLM` y `CALLITF` solo
   recogen `ADVICE_BEFORE` y `ADVICE_AFTER`).  El mismo advice se ejecuta o no
   segun por donde se llame el metodo: otro VALOR, sin error.
2. **(c) `@Log` se acepta y no hace nada**: el parser rellena
   `ClassDecl::lombok_log` y nadie lo lee.  Una anotacion que se traga en
   silencio.
3. **(c) `advice_chains`, `all_advices_attributed` y `usa_return_addr` no se
   serializan en el `.vxir` ni se fusionan** (`merged` es el IR del raiz y
   solo conserva lo suyo).  Un aspecto declarado en una dependencia no se
   teje; un modulo servido de la cache pierde las tres cosas; un `call_site`
   tejido en la stdlib no dispara `VXW934`.
4. **`fn_id` de `@Hook` no es estable entre modulos**: `hook_fn_ids_` es de
   cada `Lowering`, o sea uno por modulo, y dos funciones de modulos
   distintos reciben el mismo id.  Un perfilador indexado por `fn_id` mezcla
   funciones.
5. **`--instrument` no tiene `unwind`**: con una excepcion la traza se
   descuadra (la misma razon por la que `@Hook` tiene `fn.unwind`).  Y
   `trace` y `profile` producen el mismo IR.
6. **`emit_hook_calls` reescribe `__` -> `.` a mano, sin la guarda de
   identificador** que si tiene `demangle_symbol`: `__lambda` pasa a
   `.lambda`.  Es una de las ~20 copias de esa regla repartidas por el
   compilador (`linter.cpp`, `ufcs.cpp`, `module_interop.cpp`,
   `type_checker.cpp`, `lsp_server.cpp`, `c_backend.cpp`...), que merecen su
   propio barrido de centralizacion.
7. **`RETURN_ADDR` no esta modelado en ningun analisis**: rompe la pureza
   local en la huella y sube a `top()` en el motor, asi que pedir `call_site`
   vuelve opaca la funcion instrumentada.
8. **`loader.cpp` detecta AOP buscando el texto `"addadvice"` en `RAW_ASM`**,
   que ya no se genera (el bajado emite `IrOp::ADDADVICE`): codigo muerto.
   Lo mismo en `module_has_unattributed_aop` (`ir_optimizer.cpp`).
9. Menores: `VXW930` no puede dispararse (todos los campos estan en la lista
   blanca); comentarios desfasados (`depth` en `module.cpp`, "Around no
   implementado" en `parser.cpp`, la especulacion con aspectos en `oop.cpp`).

### 15.5 Duplicaciones que este plan ABSORBE

- La semantica de la cadena del AOP, escrita tres veces (runtime,
  `proceed_target_`, tejido en devirt): queda UNA, la del enlace.
- Dos lenguajes de selector (glob de `@Hook`, `"Clase.metodo"` parseado en
  tres sitios): queda el predicado (5.2).
- `--instrument` y `@Hook` emitidos en paralelo en los mismos siete-ocho
  sitios del bajado: quedan los puntos de union.
- Tres criterios de efectos y contratos (huella de contratos, motor
  semantico, linter), con dos `ContractCheck` y dos definiciones de `pure`
  que no coinciden: **no es de este plan**, pero el enganche de 15.2 hay que
  hacerlo en los tres mientras sigan siendo tres.  Se apunta para su propio
  paso.

### 15.6 (ver 16 para lo que hay que UNIFICAR ANTES)

### 15.7 Lo que se RETIRA al terminar

`emit_hook_calls` y `emit_instrument_*` (sustituidos por la emision de
puntos), la parte de ganchos de `project/root_weaving` y la huella de ganchos
en la cache (8), el plugin `vx_trace` basado en `getproc` (10.3), y -- cuando
exista la estrategia `dynamic` -- `ADDADVICE`/`PROCEED` en la VM, los campos
del AOP en todos los `FrameHeader` y el registro en `__module_init`.

---

## 16. Lo que hay que UNIFICAR ANTES, y lo que aportan los planes del IR

Investigado el 2026-09-27, a peticion del usuario: que mecanismos hay que
juntar primero para que el nuevo no se construya encima de copias, y si el
plan de valores anchos y vectoriales o el del IR independiente del backend
tienen algo que este necesite.

### 16.1 Prerrequisitos, por orden

**1. Terminar de unificar los dos caminos de compilacion** (el plan en curso,
5c y siguientes).  Es BLOQUEANTE: el enlace ocurre "al fusionar", y el camino
de fichero suelto no fusiona -- tiene su propia secuencia de contratos, ASA,
optimizacion y emision --.  Si no se unifica antes, el enlace y la expansion
se escriben DOS veces, que es el origen de todos los olvidos de 5c.2.  Basta
con que los dos caminos compartan la etapa "despues de fusionar" (contratos
-> ASA -> optimizar -> expandir -> emitir): es el `optimize_module` +
`emit_module` de la arquitectura acordada de ese plan.

**2. La "Fase 0" de las OPERACIONES: una tabla unica de propiedades de cada
`IrOp`.**  Es el mismo defecto que el plan de valores anchos cerro para los
TIPOS (`ir/ir_type_info.h`: doce respuestas a "cuanto mide", dos incorrectas,
todas con `default`).  Medido hoy para las operaciones:

| | |
| :-- | --: |
| etiquetas `case` sobre `IrOp` | ~2.850 |
| ficheros con `switch` sobre `IrOp` | 39 |
| de ellos con `default` | la mayoria |
| "termina el bloque" | 2 implementaciones (`effects.cpp`, `ir_optimizer.cpp`) |
| "es pura" | 3, con criterios distintos (`is_pure_op` de la huella, `is_pure` del optimizador, el motor de efectos) |
| "escribe memoria" | 2 (`effects.h`, `ir_vec_ops.h`) |
| "reserva" / "tiene efectos laterales" / "es barrera" | una cada una, en su pase |

Anadir `jp.at`/`jp.wrap` hoy es tocar esos sitios uno a uno, y los que tienen
`default` la clasificarian en silencio con la respuesta de otra operacion --
justo lo que hizo `RETURN_ADDR`, que nadie modelo y que vuelve opaca a la
funcion que lo usa (15.4, bug 7).  Un `ir/ir_op_info.h` con las PROPIEDADES
de cada operacion (no las politicas de cada pase), switch exhaustivo sin
`default`, como `ir_type_info.h`.  Vale por si solo, igual que su Fase 0.

**3. El vocabulario de efectos UNO** (5.6, decision del usuario): un parseo,
una estructura, para `extern`, funciones y advices.  Y los comprobadores
contra ella: hoy son tres (huella de contratos, motor semantico, linter) con
dos `ContractCheck` y dos `pure` que no coinciden.  Sin unificarlos, la
transparencia de `observe` y la suma de efectos declarados se escriben tres
veces.

**4. Las tablas del modulo que se pierden al fusionar y al cachear.**  Hoy
`advice_chains`, `all_advices_attributed` y `usa_return_addr` no viajan en el
`.vxir` ni se fusionan (15.4, bug 3): cada tabla de `IrModule` tiene que
acordarse de estar en `emit_ir_module_cache`, `parse_ir_module_cache`,
`adopt_cached_module` y la fusion, y tres se olvidaron.  La tabla de advices
seria la cuarta.  Antes: que las tablas de modulo se declaren en UN sitio que
esas cuatro puertas recorran, para que olvidarse deje de ser posible.

**5. Los nombres `__` <-> `.`**: un dueno con las dos direcciones
(`namespace_names` tiene una; la inversa es `demangle_symbol`, en
`namespace_flatten`) y fuera las ~20 copias a mano (15.4, bug 6).  El
selector se evalua contra el nombre escrito; con veinte reglas no hay "el"
nombre escrito.

**6. La cadena del AOP, escrita tres veces** (runtime x3 con dos copias
incompletas, `proceed_target_`, tejido en devirt): los bugs primero (15.4,
bug 1) y despues una sola semantica, que es la que el enlace reutiliza.

Los tres primeros son estructurales y el nuevo mecanismo se apoya en ellos;
el 4, 5 y 6 son bugs o duplicaciones que se cierran por la regla de primero
los bugs.

### 16.2 Lo que aporta el plan de valores ANCHOS y VECTORIALES

(`plan_ir_valores_anchos` en memoria; su Fase 0 esta HECHA: `ir_type_info.h`.
El resto -- anchura y carriles EN EL VALOR -- todavia no: `IrValue` solo lleva
`IrType`.)

- **El contexto de un punto se tipa con el VALOR anclado, no con un tipo
  fijo.**  Hoy `ret_value` es `u64` en la tabla de campos: una funcion que
  devuelve un `i128` o un `<4 x i32>` no se puede observar entera.  La tabla
  de clases de punto (4.1) dice "el valor que devuelve el `ret`" y su tipo sale
  del valor, con `ir_type_info` hoy y con la anchura del valor cuando llegue.
  No bloquea la primera entrega; lo que si hay que evitar es volver a escribir
  `u64` en la tabla.
- **El mismo reparto de responsabilidades**, y conviene decirlo igual: el IR
  dice QUE (el punto, el advice), el ASA dice si conviene y si es seguro, y
  quien baja dice COMO (la estrategia, la realizacion por modo).  La expansion
  no puede llevar decisiones de un backend dentro.
- **Vectorizacion**: un punto ENLAZADO dentro del cuerpo de un bucle es un
  efecto ordenado, asi que el vectorizador no lo ensancha -- correcto, y se
  tiene que DECIR (que bucle no se vectorizo porque tenia un gancho).  Un
  advice `counter` si podria ensancharse (sumar los carriles al salir): se
  apunta para cuando existan los dos.  Un punto SIN enlazar no molesta, porque
  se borra antes de optimizar.
- **La Fase 0 de tipos es el modelo** de la de operaciones (16.1, punto 2).

### 16.3 Lo que aporta el plan del IR INDEPENDIENTE DEL BACKEND

(`plan_ir_independiente_backend` en memoria: *"el IR debe ser TONTO"*; 312
consultas de `native_poo` en el frontend.)

- **Es el mismo principio que "los puntos se marcan siempre"**: el IR de un
  modulo no debe depender ni del backend ni de los ganchos, por la misma
  razon medible -- la cache --.  Este plan no anade ninguna dependencia nueva;
  quita una (`hooks_fp`).
- **`fn_name` choca con su pieza pendiente.**  Hoy el nombre se construye con
  `emit_string_literal_repr`, que ELIGE la representacion de `string` segun el
  backend en el frontend -- exactamente lo que ese plan quiere invertir (*"que
  en vez de elegir en el frontend, emita una op que cada backend resuelva"*).
  Con los puntos de union el nombre deja el frontend y pasa a la expansion,
  donde el backend si se conoce; pero lo correcto es que la expansion emita la
  operacion NEUTRA de construir una cadena desde un literal, y esa operacion
  es la que falta en aquel plan.  Si no existe cuando se implemente esto, la
  expansion reusa `emit_string_literal_repr` y se deja escrito el porque.
- **Dos niveles de hechos** (del IR / del codigo final): los hechos del
  enlace son del IR (valen para los tres modos); el coste de la
  instrumentacion expandida es del codigo final (con `isa`).
