@echo off
title Upload Manggot IoT ke PTRFarhan
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\upload-ptrfarhan.ps1"
