@echo off
rem Registers DSVAsio.dll so DAWs list "DSV Virtual Soundcard". Needs admin.
net session >nul 2>&1
if %errorlevel% neq 0 (
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
regsvr32 "%~dp0DSVAsio.dll"
