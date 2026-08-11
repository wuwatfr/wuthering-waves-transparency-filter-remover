:: SPDX-License-Identifier: GPL-3.0-only
:: Copyright (C) 2026 WuwaTFR contributors

@echo off
chcp 65001 >nul
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_windows.ps1"
exit /b %errorlevel%
