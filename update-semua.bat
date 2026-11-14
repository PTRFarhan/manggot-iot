@echo off
title Update semua: GitHub + APK
cd /d "%~dp0"
echo ==========================================================
echo   SEKALI KLIK: upload ke GitHub lalu build APK Android
echo ==========================================================
set NOPAUSE=1
call "%~dp0publish.bat"
set NOPAUSE=
echo.
echo Lanjut build APK...
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\build-apk.ps1"
