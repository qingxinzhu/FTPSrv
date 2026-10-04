@echo off
chcp 65001 >nul
cd /d "%~dp0"
echo building FTPSrv with mingw-w64 g++ ...
echo.
set RES=
where windres >nul 2>nul
if errorlevel 1 (
    echo note: windres not found - building without the program icon
) else (
    echo --- resources (icon + version) ---
    windres -i icon.rc -O coff -o icon.res
    if errorlevel 1 goto fail
    set RES=icon.res
)
echo --- console build ---
g++ -O2 -std=c++17 -static -Wall -o ftpsrv.exe ftpsrv.cpp %RES% -lws2_32
if errorlevel 1 goto fail
echo --- GUI build ---
g++ -O2 -std=c++17 -static -Wall -mwindows -o ftpsrv-gui.exe ftpsrv_gui.cpp %RES% -lws2_32 -lshell32 -lole32 -luuid -lcomdlg32
if errorlevel 1 goto fail
echo.
echo [OK] ftpsrv.exe + ftpsrv-gui.exe built.
dir ftpsrv*.exe | findstr ftpsrv
goto end
:fail
echo.
echo [FAILED] check that g++ (mingw-w64) is in PATH.
:end