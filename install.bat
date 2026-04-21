@echo off
setlocal

:: --- CONFIGURACIÓN ---
set EXT=.velb
set PROG=%ProgramFiles%\VestaVM\vesta.exe
set ICON=%ProgramFiles%\VestaVM\icono.ico
set CLASS=VestaVM.velbfile

echo ================================
echo  Instalando asociacion .velb
echo ================================

:: Comprobar permisos de administrador
>nul 2>&1 net session
if %errorlevel% neq 0 (
    echo Debes ejecutar este instalador como administrador.
    pause
    exit /b 1
)

:: Crear la clave de la extensión
echo Creando clave HKEY_CLASSES_ROOT\%EXT%
reg add "HKEY_CLASSES_ROOT\%EXT%" /ve /d "%CLASS%" /f

:: Crear la clase del archivo
echo Creando clase %CLASS%
reg add "HKEY_CLASSES_ROOT\%CLASS%" /ve /d "Archivo VELB" /f

:: Asociar icono
echo Asociando icono...
reg add "HKEY_CLASSES_ROOT\%CLASS%\DefaultIcon" /ve /d "\"%ICON%"\" /f

:: Crear comando de apertura
echo Creando comando de apertura...
echo "HKEY_CLASSES_ROOT\%CLASS%\shell\open\command" /ve /d "%PROG% --run %%1" /f
reg add "HKEY_CLASSES_ROOT\%CLASS%\shell\open\command" /ve /d "\"%PROG%\" --run %%1" /f

echo.
echo Extension .velb asociada correctamente.
pause
