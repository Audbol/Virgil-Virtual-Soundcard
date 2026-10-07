@echo off
rem Removes the ASIO driver registration (run before deleting this folder).
net session >nul 2>&1
if %errorlevel% neq 0 (
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
regsvr32 /u "%~dp0DSVAsio.dll"
