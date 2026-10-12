@echo off
rem Registers VirgilAsio.dll so DAWs list the ASIO driver "Virgil". Needs admin.
net session >nul 2>&1
if %errorlevel% neq 0 (
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
regsvr32 "%~dp0VirgilAsio.dll"
