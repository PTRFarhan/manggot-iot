@echo off
title Ganti Akun GitHub
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\ganti-akun-github.ps1"
