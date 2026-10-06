@echo off
rem Starts the game on Windows through scripts\run_game.py, the launcher run.sh uses as well: it
rem builds the port through MSYS2 when needed and starts it.
rem Usage: run.bat [--game-dir DIR] [bb-probe options...]
rem MSYS2 is expected in C:\msys64 (set BB_MSYS2 otherwise); see README "Windows".
setlocal
if not defined BB_MSYS2 set "BB_MSYS2=C:\msys64"
set "BB_PYTHON=%BB_MSYS2%\clang64\bin\python.exe"
if not exist "%BB_PYTHON%" (
    echo MSYS2 Python not found at %BB_PYTHON%. Install MSYS2 and the packages listed in README.md, or set BB_MSYS2.
    exit /b 1
)
"%BB_PYTHON%" "%~dp0scripts\run_game.py" %*
exit /b %ERRORLEVEL%
