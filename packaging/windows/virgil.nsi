; NSIS installer for the Virgil virtual soundcard (Windows x64).
; Built by packaging/windows/build-installer.sh (or makensis directly):
;   makensis -DVERSION=0.1.0 -DBIN_DIR=<dir with virgild.exe, VirgilAsio.dll, ...>
;            -DSRC_DIR=<repo root> -DOUT_FILE=<setup.exe> virgil.nsi

Target amd64-unicode   ; native 64-bit installer (Virgil is x64-only)
SetCompressor /SOLID lzma

!ifndef VERSION
  !error "pass -DVERSION=x.y.z"
!endif
!ifndef BIN_DIR
  !error "pass -DBIN_DIR=<build output>"
!endif
!ifndef SRC_DIR
  !define SRC_DIR "..\.."
!endif
!ifndef OUT_FILE
  !define OUT_FILE "Virgil-${VERSION}-win64-setup.exe"
!endif

!include "MUI2.nsh"
!include "x64.nsh"
!include "LogicLib.nsh"

!define PRODUCT "Virgil Virtual Soundcard"
!define SERVICE "Virgil"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Virgil"
!define FIREWALL_RULE "Virgil Virtual Soundcard (Dante)"

Name "${PRODUCT}"
OutFile "${OUT_FILE}"
InstallDir "$PROGRAMFILES64\Virgil"
InstallDirRegKey HKLM "${UNINST_KEY}" "InstallLocation"
RequestExecutionLevel admin
BrandingText "${PRODUCT} ${VERSION}"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${PRODUCT}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "${PRODUCT} installer"
VIAddVersionKey "LegalCopyright" "Virgil Project"

!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TEXT "This installs the Virgil virtual soundcard:$\r$\n$\r$\n\
  - virgild, a background service that appears in Dante Controller as a Dante device \
(unofficial; built on the open-source Inferno project)$\r$\n\
  - an ASIO driver named $\"${PRODUCT}$\" for your DAW$\r$\n$\r$\n\
After installing, open Virgil Control (Start menu or desktop) to pick the \
network interface and the streams to receive."
!define MUI_FINISHPAGE_SHOWREADME "$INSTDIR\README.md"
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Open the README"
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "Open Virgil Control (status and settings)"
!define MUI_FINISHPAGE_RUN_FUNCTION OpenControl

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Var ConfDir

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "${PRODUCT} requires 64-bit Windows."
    Abort
  ${EndIf}
  SetRegView 64
  SetShellVarContext all
FunctionEnd

Function un.onInit
  SetRegView 64
  SetShellVarContext all
FunctionEnd

Function OpenControl
  Exec '"$INSTDIR\virgil-control.exe"'
FunctionEnd

;; Stop the service and wait (up to ~20 s) until it has really exited, so its
;; files can be replaced or deleted. Falls back to killing the process.
!macro StopService UN
Function ${UN}StopService
  nsExec::Exec '"$SYSDIR\sc.exe" query ${SERVICE}'
  Pop $0
  ${If} $0 != 0
    Return  ; not installed
  ${EndIf}
  nsExec::Exec '"$SYSDIR\sc.exe" stop ${SERVICE}'
  Pop $0
  StrCpy $1 0
  ${Do}
    nsExec::Exec '"$SYSDIR\cmd.exe" /c ""$SYSDIR\sc.exe" query ${SERVICE} | "$SYSDIR\find.exe" "STOPPED""'
    Pop $0
    ${If} $0 == 0
      ${Break}
    ${EndIf}
    Sleep 500
    IntOp $1 $1 + 1
  ${LoopUntil} $1 >= 40
  nsExec::Exec '"$SYSDIR\taskkill.exe" /F /IM virgild.exe'
  Pop $0
  Sleep 500
FunctionEnd
!macroend
!insertmacro StopService ""
!insertmacro StopService "un."

Section "Virgil" SecMain
  SectionIn RO
  StrCpy $ConfDir "$APPDATA\Virgil"

  Call StopService

  SetOutPath "$INSTDIR"
  File "${BIN_DIR}\virgild.exe"
  File "${BIN_DIR}\VirgilAsio.dll"
  File "${BIN_DIR}\virgil-latency-probe.exe"
  File "${BIN_DIR}\virgil-control.exe"
  File "${SRC_DIR}\README.md"
  File "${SRC_DIR}\LICENSE"
  File "${SRC_DIR}\config\virgil.conf.example"
  File "${SRC_DIR}\packaging\windows\restart-virgil.cmd"

  ; Configuration: keep an existing (user-edited) file on upgrade.
  CreateDirectory "$ConfDir"
  ${IfNot} ${FileExists} "$ConfDir\virgil.conf"
    SetOutPath "$ConfDir"
    File "/oname=virgil.conf" "${SRC_DIR}\packaging\virgil.conf"
  ${EndIf}
  ; Let local users edit the configuration without elevation.
  nsExec::Exec '"$SYSDIR\icacls.exe" "$ConfDir\virgil.conf" /grant *S-1-5-32-545:M'
  Pop $0

  ; ASIO driver (64-bit regsvr32).
  ${DisableX64FSRedirection}
  ExecWait '"$SYSDIR\regsvr32.exe" /s "$INSTDIR\VirgilAsio.dll"' $0
  ${EnableX64FSRedirection}
  ${If} $0 != 0
    DetailPrint "regsvr32 failed ($0): ASIO driver not registered"
  ${EndIf}

  ; Service: auto start, restart on failure, logs next to the config.
  nsExec::Exec '"$SYSDIR\sc.exe" query ${SERVICE}'
  Pop $0
  ${If} $0 == 0
    nsExec::ExecToLog '"$SYSDIR\sc.exe" config ${SERVICE} binPath= "\"$INSTDIR\virgild.exe\" --service --log \"$ConfDir\virgild.log\"" start= auto'
  ${Else}
    nsExec::ExecToLog '"$SYSDIR\sc.exe" create ${SERVICE} binPath= "\"$INSTDIR\virgild.exe\" --service --log \"$ConfDir\virgild.log\"" start= auto DisplayName= "${PRODUCT}"'
  ${EndIf}
  Pop $0
  nsExec::Exec '"$SYSDIR\sc.exe" description ${SERVICE} "Dante network audio engine for the Virgil virtual soundcard (ASIO)."'
  Pop $0
  nsExec::Exec '"$SYSDIR\sc.exe" failure ${SERVICE} reset= 86400 actions= restart/2000/restart/5000/restart/30000'
  Pop $0

  ; Firewall: RTP, PTP (319/320) and SAP arrive unsolicited.
  nsExec::Exec '"$SYSDIR\netsh.exe" advfirewall firewall delete rule name="${FIREWALL_RULE}"'
  Pop $0
  nsExec::Exec '"$SYSDIR\netsh.exe" advfirewall firewall add rule name="${FIREWALL_RULE}" dir=in action=allow program="$INSTDIR\virgild.exe" protocol=udp enable=yes profile=any'
  Pop $0

  nsExec::ExecToLog '"$SYSDIR\sc.exe" start ${SERVICE}'
  Pop $0

  ; Start menu.
  CreateDirectory "$SMPROGRAMS\Virgil"
  CreateShortcut "$SMPROGRAMS\Virgil\Virgil Control.lnk" "$INSTDIR\virgil-control.exe"
  CreateShortcut "$DESKTOP\Virgil Control.lnk" "$INSTDIR\virgil-control.exe"
  CreateShortcut "$SMPROGRAMS\Virgil\Edit configuration.lnk" "$WINDIR\notepad.exe" '"$ConfDir\virgil.conf"'
  CreateShortcut "$SMPROGRAMS\Virgil\Restart Virgil service.lnk" "$INSTDIR\restart-virgil.cmd"
  CreateShortcut "$SMPROGRAMS\Virgil\Virgil status.lnk" "$SYSDIR\cmd.exe" '/k ""$INSTDIR\virgild.exe" --status"'
  CreateShortcut "$SMPROGRAMS\Virgil\Log file.lnk" "$WINDIR\notepad.exe" '"$ConfDir\virgild.log"'
  CreateShortcut "$SMPROGRAMS\Virgil\README.lnk" "$INSTDIR\README.md"
  CreateShortcut "$SMPROGRAMS\Virgil\Uninstall.lnk" "$INSTDIR\uninstall.exe"

  ; Add/Remove Programs.
  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "${PRODUCT}"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "Virgil Project"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\virgild.exe"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "EstimatedSize" 4096
SectionEnd

Section "Uninstall"
  Call un.StopService
  nsExec::Exec '"$SYSDIR\sc.exe" delete ${SERVICE}'
  Pop $0

  ${DisableX64FSRedirection}
  ExecWait '"$SYSDIR\regsvr32.exe" /u /s "$INSTDIR\VirgilAsio.dll"'
  ${EnableX64FSRedirection}

  nsExec::Exec '"$SYSDIR\netsh.exe" advfirewall firewall delete rule name="${FIREWALL_RULE}"'
  Pop $0

  Delete /REBOOTOK "$INSTDIR\virgild.exe"
  Delete /REBOOTOK "$INSTDIR\VirgilAsio.dll"
  Delete /REBOOTOK "$INSTDIR\virgil-latency-probe.exe"
  Delete /REBOOTOK "$INSTDIR\virgil-control.exe"
  Delete "$DESKTOP\Virgil Control.lnk"
  Delete /REBOOTOK "$INSTDIR\README.md"
  Delete /REBOOTOK "$INSTDIR\LICENSE"
  Delete /REBOOTOK "$INSTDIR\virgil.conf.example"
  Delete /REBOOTOK "$INSTDIR\restart-virgil.cmd"
  Delete /REBOOTOK "$INSTDIR\uninstall.exe"
  RMDir /REBOOTOK "$INSTDIR"
  RMDir /r "$SMPROGRAMS\Virgil"
  DeleteRegKey HKLM "${UNINST_KEY}"
  ; The configuration and log in ProgramData\Virgil are kept on purpose.
SectionEnd
