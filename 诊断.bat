@echo off
chcp 65001 >nul
title FTPSrv diag
cd /d "%~dp0"

echo ===== [1] is the server listening on 2121 ? =====
netstat -ano | findstr :2121
echo.
echo ===== [2] local IPv4 addresses =====
ipconfig | findstr /i "IPv4"
echo.
echo ===== [3] self test from this PC (loopback) =====
curl -v --max-time 10 ftp://127.0.0.1:2121/ --user admin:admin
echo.
echo ===== [4] firewall rules mentioning ftpsrv =====
netsh advfirewall firewall show rule name=all | findstr /i "ftpsrv"
echo.
echo ===== [5] firewall profile states (ON = blocking enabled) =====
netsh advfirewall show allprofiles state
echo.
echo ===== done. please send a screenshot of this window =====
pause
