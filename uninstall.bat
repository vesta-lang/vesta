@echo off
setlocal

:: --- CONFIGURACIÓN ---
set EXT=.velb
set CLASS=VestaVM.velbfile

echo ================================
echo  DESINSTALANDO asociacion .velb
echo ================================

:: Comprobar permisos de administrador
>nul 2>&1 net session
if %errorlevel% neq 0 (
    echo Debes ejecutar este desinstalador como administrador.
    pause
    exit /b 1
)

:: Eliminar la clave de la extensión
echo Eliminando clave HKEY_CLASSES_ROOT\%EXT%
reg delete "HKEY_CLASSES_ROOT\%EXT%" /f

:: Eliminar la clase del archivo
echo Eliminando clase %CLASS%
reg delete "HKEY_CLASSES_ROOT\%CLASS%" /f

echo.
echo Asociacion .velb eliminada correctamente.
pause
