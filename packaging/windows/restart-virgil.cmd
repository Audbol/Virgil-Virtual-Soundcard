@echo off
rem Restart the Virgil service (asks for elevation) so virgil.conf changes apply.
net session >nul 2>&1
if %errorlevel% neq 0 (
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
net stop Virgil
net start Virgil
timeout /t 3 >nul
