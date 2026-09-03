@echo off
setlocal EnableExtensions
title LukeLang installer
color 0A
echo.
echo  ========================================
echo    LukeLang Windows installer
echo  ========================================
echo.
echo  This window will stay open until install finishes.
echo  Prefer PowerShell if you can:
echo    irm https://lukelang.org/mimo.ps1 ^| iex
echo.
set "MIMO_PAUSE=1"
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference='Stop'; Write-Host 'Fetching installer...'; $script = Invoke-RestMethod https://lukelang.org/mimo.ps1; $env:MIMO_PAUSE='1'; Invoke-Expression $script; if (-not $?) { exit 1 }"
set "ERR=%ERRORLEVEL%"
echo.
if "%ERR%"=="0" (
  echo  SUCCESS. Open a NEW terminal and run:  mimo doctor
) else (
  echo  FAILED with exit code %ERR%.
  echo  Tip: right-click downloaded files - Properties - Unblock
  echo  if Windows SmartScreen blocked the zip ^(unsigned binary^).
)
echo.
pause
exit /b %ERR%
