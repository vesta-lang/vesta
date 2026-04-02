# VestaVM

<div style="display: flex; align-items: center; gap: 20px;">
  <img src="./Component 1.svg" width="250" height="250" />
  <p>
    Vesta es una VM de bajo nivel y distribuida que está siendo desarrollada para un lenguaje de programación.
    Esta VM se basa en el concepto de registros y memoria. Puede encontrar más información y documentación en el repositorio:<br><br>
    <a href="https://github.com/desmonHak/VMdoc">https://github.com/desmonHak/VMdoc</a>
  </p>
</div>

----

## Enlaces de interes

- [Licencia del proyecto](./LICENSE.md)  
  Aquí puede encontrar qué puede y qué no puede hacer con el código.

- [Cómo contribuir de forma correcta](./doc/CONTRIBUTING.md)  
  Guía para aportar al proyecto de manera ordenada y segura.

- [Información sobre el gitflow que manejamos](./doc/github_work.md)  
  Explicación del flujo de trabajo con ramas y versiones.

- [Qué dependencias usamos](./doc/DEPENDENCIES.md)  
  Lista de librerías y herramientas necesarias para compilar 


----

## Compilacion

clonar usando:

```bash
git clone  --recursive https://github.com/desmonHak/VM.git
```

## Instalacion en linux

```bash
sudo apt install build-essential cmake libssl-dev
```

### Arch linux

```bash
sudo pacman -S openssl
```

## Compilacion en linux

compilacion con CMAKE:
```bash
mkdir build
cd build
cmake -Wno-dev ..
cmake --build .
```
> En caso de que CMAKE le de error por usar una version no compatible con la version CMAKE de Keystone, puede intentar forzar el uso
> de la version que usted use añadiendo la flag ``-DCMAKE_POLICY_VERSION_MINIMUM=3.5`` para cambiar la version minima.
> Si compila en Windows, debe Mingw32/64 o TDM-GCC, y usar la flag ``-G "MinGW Makefiles"``

Si prefiere usar XMAKE:
```bash
curl -fsSL https://xmake.io/shget.text | bash
xmake f --toolchain=clang -m debug -v
xmake run
```

En caso de un error similar a este en linux:

```c
Enabling CAPSTONE_ARC_SUPPORT
: not foundskF/C/VM/libs/SourceCode/keystone/llvm/cmake/config.guess: 6:
: not foundskF/C/VM/libs/SourceCode/keystone/llvm/cmake/config.guess: 8:
: not foundskF/C/VM/libs/SourceCode/keystone/llvm/cmake/config.guess: 28:
: not foundskF/C/VM/libs/SourceCode/keystone/llvm/cmake/config.guess: 29:
: not foundskF/C/VM/libs/SourceCode/keystone/llvm/cmake/config.guess: 40:
: not foundskF/C/VM/libs/SourceCode/keystone/llvm/cmake/config.guess: 42:
: not foundskF/C/VM/libs/SourceCode/keystone/llvm/cmake/config.guess: 54:
: not foundskF/C/VM/libs/SourceCode/keystone/llvm/cmake/config.guess: 65:
: not foundskF/C/VM/libs/SourceCode/keystone/llvm/cmake/config.guess: 68:
/root/WinDiskF/C/VM/libs/SourceCode/keystone/llvm/cmake/config.guess: 71: Syntax error: word unexpected (expecting "in")
CMake Error at libs/SourceCode/keystone/llvm/cmake/modules/GetHostTriple.cmake:24 (message):
  Failed to execute
  /root/WinDiskF/C/VM/libs/SourceCode/keystone/llvm/cmake/config.guess
Call Stack (most recent call first):
  libs/SourceCode/keystone/llvm/cmake/config-ix.cmake:293 (get_host_triple)
  libs/SourceCode/keystone/llvm/CMakeLists.txt:325 (include)
```

puede necesitar convertir los CRLF de Windows en LF de linux usando el siguiente comandos:

```bash
find ../libs/SourceCode -type f -exec dos2unix {} \;
```

generalmente esto permite que el proyecto se compile correctamente si ocurre el anterior error.

### Elegir modo 'Debug' o 'Release'

debug:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

release:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Valgrind/profiling:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes ./vm```
```

----

## Forma de trabajo

Siga lellendo en: [github_work.md](./doc/github_work.md)

----
