@echo off
title Build APK Manggot IoT
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\build-apk.ps1"
