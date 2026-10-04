@echo off
chcp 65001 >nul
title FTPSrv - single folder
cd /d "%~dp0"

echo ============================================
echo   FTPSrv - SINGLE FOLDER share mode
echo   shared folder : %~dp0share
echo   port          : 2121
echo   login         : admin / admin   (change it below!)
echo ============================================
echo.

if not exist share mkdir share

if not exist ftpsrv.exe (
  if exist bin\ftpsrv.exe (
    copy /y bin\ftpsrv.exe ftpsrv.exe >nul
  ) else (
    echo [ERROR] ftpsrv.exe not found in this folder.
    pause
    exit /b 1
  )
)

ftpsrv.exe --root "%~dp0share" --port 2121 --user admin --pass admin --log ftpsrv.log

echo.
echo server stopped.
pause
