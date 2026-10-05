@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0diagnose_shell_integration.ps1" -OutputPath "%~dp0shell-integration-%RANDOM%.json"
if errorlevel 1 (
    echo Diagnostic collection failed. Please keep this window's error message.
) else (
    echo Diagnostic JSON saved beside this file. Please send it with the bug report.
)
pause
