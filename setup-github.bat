@echo off
title Setup Manggot IoT ke GitHub
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\setup-github.ps1"
