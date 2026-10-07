# Run from an elevated PowerShell in the folder holding dsvd.exe and DSVAsio.dll.
# Installs dsvd as a scheduled task that starts at logon of the current user
# (the ASIO driver and dsvd share a per-session memory section), and
# registers the ASIO driver.
$ErrorActionPreference = "Stop"
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$conf = Join-Path $env:ProgramData "DSV\dsv.conf"
if (-not (Test-Path $conf)) {
  New-Item -ItemType Directory -Force -Path (Split-Path $conf) | Out-Null
  Copy-Item (Join-Path $dir "dsv.conf.example") $conf
  Write-Host "Edit $conf (interface, flows) and re-run."
}
regsvr32.exe /s (Join-Path $dir "DSVAsio.dll")
$action = New-ScheduledTaskAction -Execute (Join-Path $dir "dsvd.exe") -Argument "-c `"$conf`""
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit 0 -Priority 0
Register-ScheduledTask -TaskName "DSV Soundcard" -Action $action -Trigger $trigger `
  -Settings $settings -RunLevel Highest -Force | Out-Null
Start-ScheduledTask -TaskName "DSV Soundcard"
Write-Host "DSV installed. Select 'DSV Virtual Soundcard' as ASIO device in your DAW."
