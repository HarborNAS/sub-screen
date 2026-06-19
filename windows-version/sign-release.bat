@echo off
setlocal

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\sign-release.ps1" %*
exit /b %ERRORLEVEL%
